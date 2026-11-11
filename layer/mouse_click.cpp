// mouse_click.cpp — voir mouse_click.h.
#include "mouse_click.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "config/IniFile.h"
#include "emulation/PoseMath.h"
#include "log/Logger.h"
#include "state.h"

namespace layer {
namespace em = hands::emulation;

namespace {

struct Calib {
    bool ok = false;
    double cx = 0, sx = 0, cy = 0, sy = 0;  // pixel = c + s * tan(angle) ; angles relatifs a la tete
};
Calib g_cal;
bool g_loaded = false;

struct Sample {
    double tx, ty, px, py;
};
bool g_calibrating = false;
int g_calStep = 0;
std::vector<Sample> g_samples;
ULONGLONG g_calStart = 0;
bool g_headValid = false;
em::Pose g_head;
bool g_calKeyWas = false, g_modeKeyWas = false, g_lbWas = false;
em::Vec3 g_markerWorld;      // repere de l'etalonnage en cours : FIXE dans le monde (on peut bouger la tete pour viser)
bool g_markerPending = true;  // a placer des que la tete est connue
constexpr float kMarkerDist = 0.6f;  // distance du repere : celle d'un tableau de bord, pour que le pointeur de MSFS et le repere soient en contact
bool g_mode = false;
bool g_tune = false, g_tuneKeyWas = false;  // reglage a vue : un cube vert montre ou le calcul place le pointeur
ULONGLONG g_tuneStep = 0;

// Reperes : (azimut, elevation) en degres, a 1 m devant la tete.
constexpr double kPts[5][2] = {{0, 0}, {-18, 0}, {18, 0}, {0, -12}, {0, 12}};
constexpr double kDeg = 3.14159265358979 / 180.0;
constexpr int kIndexTip = 10;

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
    if (!ini.Has("mouse", "sx")) return;
    g_cal.cx = ini.GetDouble("mouse", "cx", 0);
    g_cal.sx = ini.GetDouble("mouse", "sx", 0);
    g_cal.cy = ini.GetDouble("mouse", "cy", 0);
    g_cal.sy = ini.GetDouble("mouse", "sy", 0);
    g_cal.ok = g_cal.sx != 0 && g_cal.sy != 0;
    if (g_cal.ok) hands::log::Logger::Get().Log(hands::log::LogLevel::Info, "souris : etalonnage charge (x = %.1f + %.1f*tan, y = %.1f + %.1f*tan)", g_cal.cx, g_cal.sx, g_cal.cy, g_cal.sy);
}

void Save() {
    std::ofstream f(IniPath(), std::ios::trunc);
    if (!f) {
        hands::log::Logger::Get().Log(hands::log::LogLevel::Warn, "souris : ecriture de mouse.ini impossible (%s)", IniPath().c_str());
        return;
    }
    char b[256];
    std::snprintf(b, sizeof b, "[mouse]\ncx = %.3f\nsx = %.3f\ncy = %.3f\nsy = %.3f\n", g_cal.cx, g_cal.sx, g_cal.cy, g_cal.sy);
    f << b;
}

// Moindres carres : p = c + s * t.
bool Fit(const std::vector<double>& t, const std::vector<double>& p, double* c, double* s) {
    const double n = static_cast<double>(t.size());
    double st = 0, sp = 0, stt = 0, stp = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        st += t[i];
        sp += p[i];
        stt += t[i] * t[i];
        stp += t[i] * p[i];
    }
    const double d = n * stt - st * st;
    if (std::fabs(d) < 1e-9) return false;
    *s = (n * stp - st * sp) / d;
    *c = (sp - *s * st) / n;
    return true;
}

// Coordonnees absolues pour SendInput (bureau virtuel entier).
void SendMouse(double px, double py, DWORD flags) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>((px - vx) * 65535.0 / (vw - 1));
    in.mi.dy = static_cast<LONG>((py - vy) * 65535.0 / (vh - 1));
    in.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof in);
}

bool Project(const em::Vec3& world, double* px, double* py) {
    if (!g_cal.ok || !g_headValid) return false;
    const em::Vec3 d = em::Rotate(em::Conjugate(g_head.q), world - g_head.p);
    if (d.z > -0.05f) return false;  // derriere ou tout pres
    *px = g_cal.cx + g_cal.sx * (d.x / -d.z);
    *py = g_cal.cy + g_cal.sy * (d.y / -d.z);
    return true;
}

// Etat du pointeur.
int g_hand = -1;          // main qui pilote le pointeur (verrouillee pendant l'appui)
bool g_down = false;      // bouton gauche enfonce par nous
double g_sx = 0, g_sy = 0;  // position lissee
bool g_haveSmooth = false;

void ReleaseButton() {
    if (!g_down) return;
    SendMouse(g_sx, g_sy, MOUSEEVENTF_LEFTUP);
    g_down = false;
}

void UpdatePointer(const HandsFrame& frame) {
    int h = -1;
    if (g_hand >= 0 && frame.hand[g_hand].valid && frame.hand[g_hand].jointValid[kIndexTip]) h = g_hand;
    if (h < 0 && !g_down) {  // main droite de preference, sinon la gauche
        for (int k : {1, 0})
            if (frame.hand[k].valid && frame.hand[k].jointValid[kIndexTip]) {
                h = k;
                break;
            }
    }
    if (h < 0) {
        ReleaseButton();
        g_hand = -1;
        g_haveSmooth = false;
        return;
    }
    g_hand = h;
    double px, py;
    if (!Project(frame.hand[h].joint[kIndexTip].p, &px, &py)) return;
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    px = std::fmax(vx, std::fmin(vx + vw - 1, px));
    py = std::fmax(vy, std::fmin(vy + vh - 1, py));
    if (!g_haveSmooth) {
        g_sx = px;
        g_sy = py;
        g_haveSmooth = true;
    } else {  // lissage leger contre le tremblement du suivi
        g_sx += 0.6 * (px - g_sx);
        g_sy += 0.6 * (py - g_sy);
    }
    SendMouse(g_sx, g_sy, MOUSEEVENTF_MOVE);
    const bool pinch = frame.hand[h].pressed;
    if (pinch && !g_down) {
        SendMouse(g_sx, g_sy, MOUSEEVENTF_LEFTDOWN);
        g_down = true;
    } else if (!pinch && g_down) {
        ReleaseButton();
        g_hand = -1;
    }
}

}  // namespace

bool Mouse_IsCalibrated() {
    Load();
    return g_cal.ok;
}

bool Mouse_Calibrating() { return g_calibrating; }
bool Mouse_ModeOn() { return g_mode; }

void Mouse_ToggleMode() {
    Load();
    if (!g_mode && !g_cal.ok) {
        hands::log::Logger::Get().Log(hands::log::LogLevel::Warn, "souris : pas d'etalonnage ; faites d'abord Ctrl+Alt+F10");
        return;
    }
    g_mode = !g_mode;
    if (!g_mode) {
        ReleaseButton();
        g_hand = -1;
        g_haveSmooth = false;
    }
    hands::log::Logger::Get().Log(hands::log::LogLevel::Info, "souris : mode souris %s", g_mode ? "ACTIVE" : "desactive");
}

void Mouse_StartCalibration() {
    Load();
    if (g_calibrating) return;
    g_markerPending = true;
    ReleaseButton();
    g_mode = false;
    g_calibrating = true;
    g_calStep = 0;
    g_samples.clear();
    g_calStart = GetTickCount64();
    hands::log::Logger::Get().Log(hands::log::LogLevel::Info, "souris : etalonnage lance ; pointeur sur le repere 1/5 puis Ctrl+Alt+F10");
}

void Mouse_Frame(const HandsFrame& frame) {
    Load();
    g_headValid = frame.headValid;
    if (frame.headValid) g_head = frame.head;
    const ULONGLONG now = GetTickCount64();

    if (g_calibrating && g_markerPending && frame.headValid && g_calStep < 5) {  // repere fixe dans le monde, a l'endroit ou la tete le voit maintenant
        const em::Vec3 d = em::Normalized(em::Vec3{static_cast<float>(std::tan(kPts[g_calStep][0] * kDeg)), static_cast<float>(std::tan(kPts[g_calStep][1] * kDeg)), -1.0f});
        g_markerWorld = frame.head.p + em::Rotate(frame.head.q, d * kMarkerDist);
        g_markerPending = false;
    }
    const bool lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool lbEdge = lb && !g_lbWas;
    g_lbWas = lb;

    const bool ctrlAlt = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    const bool calKey = ctrlAlt && (GetAsyncKeyState(VK_F10) & 0x8000);
    const bool modeKey = ctrlAlt && (GetAsyncKeyState(VK_F11) & 0x8000);
    const bool calEdge = calKey && !g_calKeyWas, modeEdge = modeKey && !g_modeKeyWas;
    g_calKeyWas = calKey;
    g_modeKeyWas = modeKey;
    if (modeEdge) Mouse_ToggleMode();

    // Reglage a vue (Ctrl+Alt+F12) : un cube vert est dessine la ou le calcul place le pointeur ; fleches = decaler, Page haut / bas = ecarter / resserrer,
    // et on compare avec le vrai pointeur de MSFS. Ctrl+Alt+F12 de nouveau : enregistre.
    const bool tuneKey = ctrlAlt && (GetAsyncKeyState(VK_F12) & 0x8000);
    if (tuneKey && !g_tuneKeyWas) {
        g_tune = !g_tune;
        if (g_tune) {
            g_mode = false;
            if (!g_cal.ok) {  // point de depart : centre de l'ecran principal
                g_cal.cx = GetSystemMetrics(SM_CXSCREEN) / 2.0;
                g_cal.cy = GetSystemMetrics(SM_CYSCREEN) / 2.0;
                g_cal.sx = 1200;
                g_cal.sy = -1200;
            }
            hands::log::Logger::Get().Log(hands::log::LogLevel::Info, "souris : reglage a vue ; fleches, Page haut/bas, puis Ctrl+Alt+F12");
        } else {
            g_cal.ok = true;
            Save();
            hands::log::Logger::Get().Log(hands::log::LogLevel::Info, "souris : reglage enregistre (x = %.1f + %.1f*tan, y = %.1f + %.1f*tan)", g_cal.cx, g_cal.sx, g_cal.cy, g_cal.sy);
        }
    }
    g_tuneKeyWas = tuneKey;
    if (g_tune && ctrlAlt && now - g_tuneStep > 60) {
        g_tuneStep = now;
        const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (down(VK_RIGHT)) g_cal.cx -= 6;
        if (down(VK_LEFT)) g_cal.cx += 6;
        if (down(VK_UP)) g_cal.cy += 6;
        if (down(VK_DOWN)) g_cal.cy -= 6;
        if (down(VK_PRIOR)) {
            g_cal.sx *= 0.985;
            g_cal.sy *= 0.985;
        }
        if (down(VK_NEXT)) {
            g_cal.sx *= 1.015;
            g_cal.sy *= 1.015;
        }
    }

    if (g_calibrating && now - g_calStart > 180000) {
        g_calibrating = false;
        hands::log::Logger::Get().Log(hands::log::LogLevel::Warn, "souris : etalonnage abandonne (3 min)");
    }
    if (g_mode && !g_calibrating) UpdatePointer(frame);

    if (!calEdge && !(lbEdge && g_calibrating && !g_markerPending)) return;
    if (!g_calibrating) {
        Mouse_StartCalibration();
        return;
    }
    if (g_markerPending || !frame.headValid) return;
    POINT p;
    GetCursorPos(&p);
    const em::Vec3 loc = em::Rotate(em::Conjugate(frame.head.q), g_markerWorld - frame.head.p);
    if (loc.z > -0.05f) return;  // repere derriere la tete : on ne retient rien
    g_samples.push_back({loc.x / -loc.z, loc.y / -loc.z, static_cast<double>(p.x), static_cast<double>(p.y)});
    hands::log::Logger::Get().Log(hands::log::LogLevel::Info, "souris : repere %d/5 pris : pointeur en (%ld, %ld)", g_calStep + 1, p.x, p.y);
    g_markerPending = true;
    if (++g_calStep < 5) return;
    g_calibrating = false;
    std::vector<double> tx, ty, px, py;
    for (const Sample& s : g_samples) {
        tx.push_back(s.tx);
        ty.push_back(s.ty);
        px.push_back(s.px);
        py.push_back(s.py);
    }
    // Pixels carres : meme echelle en x et en y (px = cx + s*tx ; py = cy - s*ty). Un repere mal vise (pointeur minuscule, difficile a voir) fausse
    // beaucoup moins qu'avec deux echelles libres.
    Calib c;
    double mtx = 0, mty = 0, mpx = 0, mpy = 0;
    const double n = static_cast<double>(tx.size());
    for (size_t i = 0; i < tx.size(); ++i) {
        mtx += tx[i] / n;
        mty += ty[i] / n;
        mpx += px[i] / n;
        mpy += py[i] / n;
    }
    double num = 0, den = 0;
    for (size_t i = 0; i < tx.size(); ++i) {
        num += (tx[i] - mtx) * (px[i] - mpx) - (ty[i] - mty) * (py[i] - mpy);
        den += (tx[i] - mtx) * (tx[i] - mtx) + (ty[i] - mty) * (ty[i] - mty);
    }
    if (den > 1e-9 && num / den > 50.0) {
        c.sx = num / den;
        c.sy = -c.sx;
        c.cx = mpx - c.sx * mtx;
        c.cy = mpy - c.sy * mty;
        c.ok = true;
        g_cal = c;
        Save();
        for (size_t i = 0; i < tx.size(); ++i)
            hands::log::Logger::Get().Log(hands::log::LogLevel::Info, "souris : repere %zu : tan (%.3f, %.3f), pointeur (%.0f, %.0f), ecart apres calcul (%.0f, %.0f) px", i + 1, tx[i], ty[i], px[i],
                                          py[i], px[i] - (c.cx + c.sx * tx[i]), py[i] - (c.cy + c.sy * ty[i]));
        hands::log::Logger::Get().Log(hands::log::LogLevel::Info, "souris : etalonnage termine (x = %.1f + %.1f*tan, y = %.1f + %.1f*tan)", c.cx, c.sx, c.cy, c.sy);
    } else {
        hands::log::Logger::Get().Log(hands::log::LogLevel::Warn, "souris : etalonnage impossible (reperes confondus)");
    }
}

void Mouse_FillMarkers(HandsFrame* frame) {
    if (frame && g_tune && frame->headValid && frame->markerCount < kMaxMarkers && g_cal.sx != 0 && g_cal.sy != 0) {
        POINT p;
        GetCursorPos(&p);
        const em::Vec3 d = em::Normalized(em::Vec3{static_cast<float>((p.x - g_cal.cx) / g_cal.sx), static_cast<float>((p.y - g_cal.cy) / g_cal.sy), -1.0f});
        MarkerData& m = frame->marker[frame->markerCount++];
        m.pos = frame->head.p + em::Rotate(frame->head.q, d * kMarkerDist);
        m.size = 0.012f;
        m.r = 0.1f;
        m.g = 1.0f;
        m.b = 0.2f;
    }
    if (!frame || !g_calibrating || g_markerPending || g_calStep >= 5) return;
    if (frame->markerCount >= kMaxMarkers) return;
    MarkerData& m = frame->marker[frame->markerCount++];
    m.pos = g_markerWorld;
    m.size = 0.02f;
    m.r = m.g = m.b = 1.0f;
}

}  // namespace layer
