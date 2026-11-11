// mouse_click.cpp — voir mouse_click.h.
#include "mouse_click.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "config/IniFile.h"
#include "emulation/PoseMath.h"
#include "log/Logger.h"
#include "servo.h"
#include "state.h"

namespace layer {
namespace em = hands::emulation;
using hands::log::LogLevel;
using hands::log::Logger;

namespace {

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Reglages (mouse.ini) : sensibilite de la souris de bureau, oeil de reference de l'asservissement.
// ---------------------------------------------------------------------------------------------------------------------------------------------
double g_gain = 3500.0;  // pixels par metre de deplacement de la main (souris de bureau)
int g_eye = 0;
bool g_loaded = false;

std::string IniPath() {
    std::error_code ec;
    const std::filesystem::path data = LayerBaseDir() / "data";
    if (std::filesystem::is_directory(data, ec)) return (data / "mouse.ini").string();
    return (LayerBaseDir() / "logs" / "mouse.ini").string();
}

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(IniPath());
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    const auto ini = hands::config::IniFile::Parse(ss.str());
    g_gain = std::fmax(500.0, std::fmin(20000.0, ini.GetDouble("mouse", "gain", g_gain)));
    g_eye = ini.GetInt("mouse", "eye", 0) == 1 ? 1 : 0;
}

void Save() {
    std::ofstream f(IniPath(), std::ios::trunc);
    if (!f) {
        Logger::Get().Log(LogLevel::Warn, "souris : ecriture de mouse.ini impossible (%s)", IniPath().c_str());
        return;
    }
    char b[160];
    std::snprintf(b, sizeof b, "[mouse]\neye = %d\ngain = %.0f\n", g_eye, g_gain);
    f << b;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Injection : fil dedie. Chaque paquet = un deplacement relatif (+ un changement de bouton) ; il est decoupe en evenements de 8 comptes au plus (zone ou
// l'acceleration du pointeur de Windows vaut ~1 : un evenement de 30 comptes arriverait multiplie par 2), envoyes en un seul SendInput pour garder l'ordre.
// ---------------------------------------------------------------------------------------------------------------------------------------------
struct Packet {
    int dx = 0, dy = 0;
    int button = -1;  // -1 : inchange, 0 : relache, 1 : enfonce
};

constexpr int kMaxEventCounts = 8;

struct Injector {
    std::mutex mu;
    std::vector<Packet> queue;
    HANDLE wake = nullptr;
    bool started = false;
    bool buttonDown = false;  // etat voulu (ecrit sous verrou par les producteurs)
};

Injector& Inj() {
    static Injector* p = new Injector;  // jamais detruit : le fil peut survivre a la DLL pendant l'arret du processus
    return *p;
}

void SendPacket(const Packet& p) {
    INPUT ev[64];
    int n = 0;
    if (p.dx != 0 || p.dy != 0) {
        const int steps = std::max((std::abs(p.dx) + kMaxEventCounts - 1) / kMaxEventCounts, (std::abs(p.dy) + kMaxEventCounts - 1) / kMaxEventCounts);
        const int cnt = std::max(1, std::min(steps, 56));
        int px = 0, py = 0;
        for (int i = 1; i <= cnt; ++i) {  // decoupage equilibre : la somme des evenements vaut exactement (dx, dy)
            const int tx = static_cast<int>(std::lround(static_cast<double>(p.dx) * i / cnt)), ty = static_cast<int>(std::lround(static_cast<double>(p.dy) * i / cnt));
            INPUT& e = ev[n++];
            e = INPUT{};
            e.type = INPUT_MOUSE;
            e.mi.dx = tx - px;
            e.mi.dy = ty - py;
            e.mi.dwFlags = MOUSEEVENTF_MOVE;
            px = tx;
            py = ty;
        }
    }
    if (p.button >= 0) {
        INPUT& e = ev[n++];
        e = INPUT{};
        e.type = INPUT_MOUSE;
        e.mi.dwFlags = p.button == 1 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    }
    if (n > 0) SendInput(static_cast<UINT>(n), ev, sizeof(INPUT));
}

DWORD WINAPI InjectorThread(LPVOID) {
    Injector& in = Inj();
    std::vector<Packet> local;
    for (;;) {
        WaitForSingleObject(in.wake, INFINITE);
        {
            std::lock_guard<std::mutex> lock(in.mu);
            local.swap(in.queue);
        }
        for (const Packet& p : local) SendPacket(p);
        local.clear();
    }
}

void Post(const Packet& p) {
    Injector& in = Inj();
    {
        std::lock_guard<std::mutex> lock(in.mu);
        if (!in.started) {
            in.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            HANDLE t = in.wake ? CreateThread(nullptr, 0, InjectorThread, nullptr, 0, nullptr) : nullptr;
            if (!t) return;
            CloseHandle(t);
            in.started = true;
        }
        if (in.queue.size() > 512) in.queue.erase(in.queue.begin(), in.queue.begin() + 256);  // fil bloque : on ne laisse pas la file grandir
        in.queue.push_back(p);
        if (p.button >= 0) in.buttonDown = p.button == 1;
    }
    SetEvent(in.wake);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Curseur de Windows confine a la fenetre du jeu : les comptes de souris continuent d'arriver a MSFS (entree brute, jamais confinee) mais le curseur ne peut plus
// quitter l'ecran de MSFS vers l'autre moniteur (MSFS ne recoit alors plus les clics tant qu'on ne clique pas de nouveau dans sa fenetre). Relache a l'arret du mode.
// ---------------------------------------------------------------------------------------------------------------------------------------------
bool g_clipped = false;

struct WinSearch {
    DWORD pid = 0;
    RECT best{};
    long long area = 0;
};

BOOL CALLBACK EnumWinProc(HWND h, LPARAM lp) {
    WinSearch* w = reinterpret_cast<WinSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != w->pid || !IsWindowVisible(h)) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    const long long a = static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
    if (a > w->area) {
        w->area = a;
        w->best = r;
    }
    return TRUE;
}

void ClipToGameWindow(bool on) {
    if (!on) {
        if (g_clipped) {
            ClipCursor(nullptr);
            g_clipped = false;
            Logger::Get().Log(LogLevel::Info, "souris : curseur Windows libere");
        }
        return;
    }
    if (g_clipped) return;
    WinSearch w;
    w.pid = GetCurrentProcessId();
    EnumWindows(EnumWinProc, reinterpret_cast<LPARAM>(&w));
    const long long width = w.best.right - w.best.left, height = w.best.bottom - w.best.top;
    if (width < 800 || height < 600) return;  // pas de fenetre de jeu reconnaissable : on ne confine rien
    if (ClipCursor(&w.best)) {
        g_clipped = true;
        Logger::Get().Log(LogLevel::Info, "souris : curseur Windows confine a la fenetre du jeu (%ld,%ld)-(%ld,%ld)", w.best.left, w.best.top, w.best.right, w.best.bottom);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Type de souris et touche de secours
// ---------------------------------------------------------------------------------------------------------------------------------------------
MouseKind g_kind = MouseKind::Off;
MouseKind g_lastOn = MouseKind::Servo;
bool g_modeKeyWas = false;

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Souris de bureau : le doigt EST la souris. Le deplacement de la main (a gauche / a droite, en haut / en bas, dans le plan de la vue) devient un
// deplacement RELATIF. Reference de la main : la base de l'index (articulation 7), qui bouge peu quand on pince ; le bout de l'index se deplace vers le
// pouce et ferait deraper le pointeur au moment du clic. Index replie = souris « soulevee » (la main peut revenir sans deplacer le pointeur).
// ---------------------------------------------------------------------------------------------------------------------------------------------
constexpr int kIndexTip = 10;
int g_hand = -1;
bool g_prevValid = false;
em::Vec3 g_prev;
double g_remX = 0, g_remY = 0;  // fractions de pixel non encore envoyees
bool g_down = false;

void ReleaseDesktopButton() {
    if (!g_down) return;
    MouseInject_Button(false);
    g_down = false;
}

void UpdateDesktop(const HandsFrame& frame) {
    auto usable = [&](int k) { return frame.hand[k].valid && frame.hand[k].jointValid[1] && frame.hand[k].jointValid[7] && frame.hand[k].jointValid[kIndexTip]; };
    int h = -1;
    if (g_hand >= 0 && usable(g_hand)) h = g_hand;
    if (h < 0 && !g_down) {  // main droite de preference, sinon la gauche
        for (int k : {1, 0})
            if (usable(k)) {
                h = k;
                break;
            }
    }
    if (h < 0 || !frame.headValid) {
        ReleaseDesktopButton();
        g_hand = -1;
        g_prevValid = false;
        return;
    }
    if (h != g_hand) g_prevValid = false;
    g_hand = h;
    const HandPoseData& hd = frame.hand[h];
    const bool pinch = hd.pressed;
    const em::Vec3 pos = hd.joint[7].p;
    // Index tendu ? (bout nettement plus loin du poignet que la base de l'index) ; en pincement on suit toujours (glisser).
    const bool extended = em::Length(hd.joint[kIndexTip].p - hd.joint[1].p) - em::Length(pos - hd.joint[1].p) > 0.045f;
    if (g_prevValid && (pinch || extended)) {
        const em::Vec3 d = pos - g_prev;
        const em::Vec3 fwd = em::Rotate(frame.head.q, em::Vec3{0.0f, 0.0f, -1.0f});
        em::Vec3 fl{fwd.x, 0.0f, fwd.z};
        if (em::Length(fl) > 0.1f) {
            fl = em::Normalized(fl);
            const em::Vec3 right{-fl.z, 0.0f, fl.x};
            g_remX += g_gain * (d.x * right.x + d.z * right.z);
            g_remY += -g_gain * d.y;
            const long ix = std::lround(g_remX), iy = std::lround(g_remY);
            g_remX -= static_cast<double>(ix);
            g_remY -= static_cast<double>(iy);
            if (ix != 0 || iy != 0) MouseInject_Move(static_cast<int>(std::max(-300L, std::min(300L, ix))), static_cast<int>(std::max(-300L, std::min(300L, iy))));
        }
    }
    g_prev = pos;
    g_prevValid = true;
    if (pinch && !g_down) {
        MouseInject_Button(true);
        g_down = true;
    } else if (!pinch && g_down) {
        ReleaseDesktopButton();
        g_hand = -1;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------------------------------------------------------------------------
void MouseInject_Move(int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    Packet p;
    p.dx = dx;
    p.dy = dy;
    Post(p);
}

void MouseInject_Button(bool leftDown) {
    Packet p;
    p.button = leftDown ? 1 : 0;
    Post(p);
}

void MouseInject_ReleaseIfHeld() {
    Injector& in = Inj();
    bool held;
    {
        std::lock_guard<std::mutex> lock(in.mu);
        held = in.buttonDown;
    }
    if (held) MouseInject_Button(false);
}

MouseKind Mouse_Kind() { return g_kind; }
bool Mouse_ModeOn() { return g_kind != MouseKind::Off; }
bool Mouse_Attached() { return (g_kind == MouseKind::Off ? g_lastOn : g_kind) == MouseKind::Servo; }

void Mouse_SetKind(MouseKind k) {
    Load();
    if (k == g_kind) return;
    ReleaseDesktopButton();
    if (g_kind == MouseKind::Servo) Servo_Stop();
    MouseInject_ReleaseIfHeld();
    g_hand = -1;
    g_prevValid = false;
    g_remX = g_remY = 0;
    g_kind = k;
    if (k != MouseKind::Off) g_lastOn = k;
    ClipToGameWindow(k != MouseKind::Off);
    Logger::Get().Log(LogLevel::Info, "souris : %s",
                      k == MouseKind::Off ? "mode souris desactive" : (k == MouseKind::Servo ? "mode souris ACTIVE : pointeur colle au doigt (asservi)" : "mode souris ACTIVE : souris de bureau (relatif)"));
}

void Mouse_ToggleMode() { Mouse_SetKind(g_kind == MouseKind::Off ? g_lastOn : MouseKind::Off); }

void Mouse_SetAttached(bool on) {
    const MouseKind k = on ? MouseKind::Servo : MouseKind::Desktop;
    if (g_kind == MouseKind::Off) {
        g_lastOn = k;  // pris en compte a la prochaine activation
        return;
    }
    Mouse_SetKind(k);
}

void Mouse_Gain(double factor) {
    Load();
    g_gain = std::fmax(500.0, std::fmin(20000.0, g_gain * factor));
    Save();
    Logger::Get().Log(LogLevel::Info, "souris : sensibilite de la souris de bureau %.0f px par metre", g_gain);
}

int Mouse_Eye() {
    Load();
    return g_eye;
}

void Mouse_SetEye(int eye) {
    Load();
    g_eye = eye == 1 ? 1 : 0;
    Save();
    Servo_Stop();  // la boucle repart avec l'autre oeil
    Logger::Get().Log(LogLevel::Info, "souris : oeil de reference = %s", g_eye == 0 ? "gauche" : "droit");
}

void Mouse_Frame(const HandsFrame& frame) {
    Load();
    const bool key = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_F11) & 0x8000);
    if (key && !g_modeKeyWas) Mouse_ToggleMode();
    g_modeKeyWas = key;
    if (g_kind == MouseKind::Desktop) UpdateDesktop(frame);
}

}  // namespace layer
