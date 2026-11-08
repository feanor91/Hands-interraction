// zones.cpp — voir zones.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <winhttp.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "JoyPoll.h"
#include "config/IniFile.h"
#include "emulation/ZoneTracker.h"
#include "log/Logger.h"
#include "state.h"
#include "zones.h"

namespace layer {
namespace {

namespace em = hands::emulation;
using hands::log::LogLevel;
using hands::log::Logger;

constexpr int kIndexTip = 10;  // XR_HAND_JOINT_INDEX_TIP_EXT

// Zones connues : nom, touche de StreamSim à presser (profil, page, indice). Pour ce premier essai : le frein de parc (touche 'Frein libre' de la page MSFS).
struct ZoneDef {
    const char* name;
    const char* profile;
    const char* page;
    int index;
};
const ZoneDef kDefs[] = {{"frein_parc", "default", "od16uzxc", 1}};
constexpr int kZoneCount = sizeof(kDefs) / sizeof(kDefs[0]);

struct ZoneState {
    bool defined = false;
    em::Vec3 pos;
    float radius = 0.02f;
    em::ZoneTracker tracker[2];
    ULONGLONG lastNear = 0;
    bool inside = false;        // un bout d'index est dans la zone (derniere image)
    ULONGLONG lastFire = 0;     // dernier declenchement
    ULONGLONG lastCapture = 0;  // derniere memorisation
};
ZoneState g_zone[kZoneCount];

bool g_loaded = false;
FILETIME g_loadedStamp{};
ULONGLONG g_lastCheck = 0;
bool g_prevTeach = false, g_prevClear = false;
ULONGLONG g_teachAt = 0, g_teachTick = 0;  // enregistrement differe (0 = aucun)
em::Vec3 g_teachTip;                       // bout d'index qui sera enregistre (pour le repere visuel)
bool g_teachTipValid = false;
int g_buttonLogsLeft = 200;
std::atomic<bool> g_posting{false};

// Alloue et jamais detruit : pas de COM dans un destructeur statique (dechargement de la DLL sous verrou du chargeur).
hands::win::JoyPoll& JoyInstance() {
    static hands::win::JoyPoll* p = new hands::win::JoyPoll;
    return *p;
}

// zones.ini : le dossier de la DLL n'est inscriptible que sur logs\ (et data\ depuis la version du 8 novembre de install.ps1) ; on prend data\
// s'il existe, sinon logs\ (installations anterieures).
std::string ZonesPath() {
    std::error_code ec;
    const std::filesystem::path data = LayerBaseDir() / "data";
    if (std::filesystem::is_directory(data, ec)) return (data / "zones.ini").string();
    return (LayerBaseDir() / "logs" / "zones.ini").string();
}

FILETIME FileStamp() {
    WIN32_FILE_ATTRIBUTE_DATA d;
    return GetFileAttributesExA(ZonesPath().c_str(), GetFileExInfoStandard, &d) ? d.ftLastWriteTime : FILETIME{};
}

void LoadZones() {
    for (auto& z : g_zone) z.defined = false;
    std::ifstream f(ZonesPath());
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    const auto ini = hands::config::IniFile::Parse(ss.str());
    for (int i = 0; i < kZoneCount; ++i) {
        if (!ini.Has(kDefs[i].name, "x")) continue;
        g_zone[i].pos = {static_cast<float>(ini.GetDouble(kDefs[i].name, "x", 0)), static_cast<float>(ini.GetDouble(kDefs[i].name, "y", 0)),
                         static_cast<float>(ini.GetDouble(kDefs[i].name, "z", 0))};
        const double r = ini.GetDouble(kDefs[i].name, "radius_mm", 0.0);
        g_zone[i].radius = static_cast<float>((r >= 5.0 && r <= 100.0 ? r : CurrentConfig()->zoneRadiusMm) / 1000.0);
        g_zone[i].defined = true;
        for (auto& t : g_zone[i].tracker) t.Disarm();  // le doigt est peut-etre deja dans la zone : pas de declenchement au chargement
    }
}

void SaveZones() {
    std::ofstream f(ZonesPath(), std::ios::trunc);
    if (!f) {
        Logger::Get().Log(LogLevel::Warn, "zones : ecriture de zones.ini impossible (%s) ; reinstallez la couche (install.ps1 cree data\\)", ZonesPath().c_str());
        return;
    }
    f << "; Zones apprises (espace du jeu, metres). Genere par la couche (bouton de memorisation de la tray, ou Ctrl+Alt+F9 / F10).\n";
    for (int i = 0; i < kZoneCount; ++i) {
        if (!g_zone[i].defined) continue;
        char b[256];
        std::snprintf(b, sizeof(b), "[%s]\nx = %.4f\ny = %.4f\nz = %.4f\nradius_mm = %.0f\n\n", kDefs[i].name, g_zone[i].pos.x, g_zone[i].pos.y,
                      g_zone[i].pos.z, g_zone[i].radius * 1000.0f);
        f << b;
    }
}

// --- Retour sonore : de petits sons generes en memoire et joues par PlaySound (passent par le peripherique audio par defaut, que Virtual
// Desktop transmet au casque ; Beep() n'est pas fiable). Les tampons restent valides pendant toute la lecture asynchrone (statiques).
std::vector<uint8_t> MakeTone(int freq, int ms) {
    const int rate = 22050;
    const int n = rate * ms / 1000;
    std::vector<uint8_t> w(44 + static_cast<size_t>(n) * 2);
    auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) w[o + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i)); };
    auto put16 = [&](size_t o, uint16_t v) { w[o] = static_cast<uint8_t>(v); w[o + 1] = static_cast<uint8_t>(v >> 8); };
    std::memcpy(&w[0], "RIFF", 4);
    put32(4, static_cast<uint32_t>(36 + n * 2));
    std::memcpy(&w[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);   // PCM
    put16(22, 1);   // mono
    put32(24, rate);
    put32(28, rate * 2);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(&w[36], "data", 4);
    put32(40, static_cast<uint32_t>(n * 2));
    const int fade = rate / 100;  // 10 ms d'attaque et de relachement : pas de claquement
    for (int i = 0; i < n; ++i) {
        float env = 1.0f;
        if (i < fade) env = static_cast<float>(i) / static_cast<float>(fade);
        else if (i > n - fade) env = static_cast<float>(n - i) / static_cast<float>(fade);
        const float s = std::sin(6.2831853f * static_cast<float>(freq) * static_cast<float>(i) / static_cast<float>(rate)) * 0.6f * env;
        put16(44 + static_cast<size_t>(i) * 2, static_cast<uint16_t>(static_cast<int16_t>(s * 32000.0f)));
    }
    return w;
}

enum class Snd { Start, Tick, Captured, Trigger, Clear, Fail };
void Play(Snd s) {
    static const std::vector<uint8_t> tones[] = {MakeTone(800, 90), MakeTone(800, 45), MakeTone(1500, 160), MakeTone(1000, 90), MakeTone(500, 180), MakeTone(300, 300)};
    const std::vector<uint8_t>& t = tones[static_cast<int>(s)];
    if (!PlaySoundW(reinterpret_cast<LPCWSTR>(t.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT)) {
        static const DWORD freq[] = {800, 800, 1500, 1000, 500, 300};
        const DWORD f = freq[static_cast<int>(s)];
        std::thread([f] { Beep(f, 80); }).detach();  // repli
    }
}

// POST /api/press vers StreamSim, sur un fil a part : le rendu n'attend jamais le reseau.
void PostPress(const ZoneDef& def, int port) {
    if (g_posting.exchange(true)) return;
    const std::string body =
        std::string("{\"profileId\":\"") + def.profile + "\",\"pageId\":\"" + def.page + "\",\"index\":" + std::to_string(def.index) + "}";
    const std::string name = def.name;
    std::thread([body, name, port] {
        bool ok = false;
        DWORD status = 0;
        if (HINTERNET s = WinHttpOpen(L"HandsLayer", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)) {
            WinHttpSetTimeouts(s, 1000, 1000, 2000, 3000);
            if (HINTERNET c = WinHttpConnect(s, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0)) {
                if (HINTERNET r = WinHttpOpenRequest(c, L"POST", L"/api/press", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)) {
                    const wchar_t* hdr = L"Content-Type: application/json\r\n";
                    if (WinHttpSendRequest(r, hdr, static_cast<DWORD>(-1L), const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                                           static_cast<DWORD>(body.size()), 0) &&
                        WinHttpReceiveResponse(r, nullptr)) {
                        DWORD sz = sizeof(status);
                        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                            WINHTTP_NO_HEADER_INDEX);
                        ok = status >= 200 && status < 300;
                    }
                    WinHttpCloseHandle(r);
                }
                WinHttpCloseHandle(c);
            }
            WinHttpCloseHandle(s);
        }
        Logger::Get().Log(ok ? LogLevel::Info : LogLevel::Warn, "zones : '%s' -> StreamSim /api/press : %s (HTTP %lu)", name.c_str(),
                          ok ? "ok" : "ECHEC (StreamSim lance ?)", static_cast<unsigned long>(status));
        g_posting = false;
    }).detach();
}

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Main dont le bout d'index sert a memoriser : la droite si elle est suivie, sinon la gauche. -1 si aucune.
int TeachHand(const HandsFrame& frame) {
    for (int k : {1, 0})
        if (frame.hand[k].valid && frame.hand[k].jointValid[kIndexTip]) return k;
    return -1;
}

}  // namespace

void Zones_Frame(const HandsFrame& frame) {
    const auto cfg = CurrentConfig();
    if (!cfg->zonesEnabled) return;
    const ULONGLONG now = GetTickCount64();

    if (!g_loaded || now - g_lastCheck > 1000) {  // rechargement a chaud de zones.ini
        g_lastCheck = now;
        const FILETIME stamp = FileStamp();
        if (!g_loaded || stamp.dwLowDateTime != g_loadedStamp.dwLowDateTime || stamp.dwHighDateTime != g_loadedStamp.dwHighDateTime) {
            LoadZones();
            g_loadedStamp = stamp;
            if (!g_loaded)
                Logger::Get().Log(LogLevel::Info, "zones : activees (fichier : %s) ; Ctrl+Alt+F9 = apprendre 'frein_parc' au bout de l'index, Ctrl+Alt+F10 = tout effacer",
                                  ZonesPath().c_str());
            g_loaded = true;
        }
    }

    // Apprentissage / effacement (touches lues meme sans le focus).
    const bool mods = KeyDown(VK_CONTROL) && KeyDown(VK_MENU);
    const bool teachKey = mods && KeyDown(VK_F9), clearKey = mods && KeyDown(VK_F10);
    // Boutons de joystick / HOTAS choisis dans la tray (front montant).
    bool teachBtn = false, clearBtn = false;
    if (cfg->zoneTeachButton > 0 || cfg->zoneClearButton > 0) {
        static bool listed = false;
        if (!listed) {
            listed = true;
            for (const std::string& n : JoyInstance().Devices()) Logger::Get().Log(LogLevel::Info, "zones : joystick lu : %s", n.c_str());
            Logger::Get().Log(LogLevel::Info, "zones : boutons surveilles : memoriser = %d, effacer = %d (peripherique : '%s')", cfg->zoneTeachButton,
                              cfg->zoneClearButton, cfg->zoneJoystick.c_str());
        }
        for (const hands::win::JoyEvent& ev : JoyInstance().PollAllPressed(cfg->zoneJoystick)) {
            if (g_buttonLogsLeft > 0) {
                --g_buttonLogsLeft;
                Logger::Get().Log(LogLevel::Info, "zones : bouton %d enfonce (%s)", ev.button, ev.device.c_str());
            }
            if (ev.button == cfg->zoneTeachButton) teachBtn = true;
            if (ev.button == cfg->zoneClearButton) clearBtn = true;
        }
    }
    const bool clearNow = (clearKey && !g_prevClear) || clearBtn;
    // L'enregistrement a lieu `teach_delay_ms` apres l'appui (un son par seconde, repere blanc au bout de l'index), le temps de poser le doigt.
    if ((teachKey && !g_prevTeach) || teachBtn) {
        g_teachAt = now + static_cast<ULONGLONG>(cfg->zoneTeachDelayMs);
        g_teachTick = now;
        Logger::Get().Log(LogLevel::Info, "zones : apprentissage dans %.1f s, posez le bout de l'index sur la commande", cfg->zoneTeachDelayMs / 1000.0);
        Play(Snd::Start);
    }
    g_teachTipValid = false;
    if (g_teachAt != 0) {
        const int h = TeachHand(frame);
        if (h >= 0) {
            g_teachTip = frame.hand[h].joint[kIndexTip].p;
            g_teachTipValid = true;
        }
        if (now < g_teachAt && now - g_teachTick >= 1000) {
            g_teachTick = now;
            Play(Snd::Tick);
        }
        if (now >= g_teachAt) {
            if (h < 0) {
                // Main pas (encore) suivie : on reessaie a chaque image pendant 5 s avant d'abandonner.
                if (now >= g_teachAt + 5000) {
                    g_teachAt = 0;
                    Logger::Get().Log(LogLevel::Warn, "zones : apprentissage abandonne, aucun bout d'index suivi pendant 5 s (la main est-elle visible des cameras ?)");
                    Play(Snd::Fail);
                }
            } else {
                g_teachAt = 0;
                ZoneState& z = g_zone[0];
                z.pos = frame.hand[h].joint[kIndexTip].p;
                z.radius = static_cast<float>(cfg->zoneRadiusMm / 1000.0);
                z.defined = true;
                z.lastCapture = now;
                for (auto& t : z.tracker) t.Disarm();  // le doigt est deja dans la zone : il doit en sortir avant de declencher
                SaveZones();
                g_loadedStamp = FileStamp();
                Logger::Get().Log(LogLevel::Info, "zones : '%s' apprise avec la main %s en (%.3f, %.3f, %.3f), rayon %.0f mm", kDefs[0].name,
                                  h == 1 ? "droite" : "gauche", z.pos.x, z.pos.y, z.pos.z, z.radius * 1000.0f);
                Play(Snd::Captured);
            }
        }
    }
    if (clearNow) {
        for (auto& z : g_zone) z.defined = false;
        SaveZones();
        g_loadedStamp = FileStamp();
        Logger::Get().Log(LogLevel::Info, "zones : toutes les zones effacees");
        Play(Snd::Clear);
    }
    g_prevTeach = teachKey;
    g_prevClear = clearKey;

    for (int i = 0; i < kZoneCount; ++i) {
        ZoneState& z = g_zone[i];
        z.inside = false;
        if (!z.defined) continue;
        for (int h = 0; h < 2; ++h) {
            const HandPoseData& hd = frame.hand[h];
            const bool ok = hd.valid && hd.jointValid[kIndexTip];
            const em::Vec3 tip = ok ? hd.joint[kIndexTip].p : em::Vec3{};
            if (ok && em::Length(tip - z.pos) <= z.radius) z.inside = true;
            if (ok && em::Length(tip - z.pos) < 0.10f && now - z.lastNear > 500) {  // trace de mise au point : a quelle distance passe le doigt ?
                z.lastNear = now;
                Logger::Get().Log(LogLevel::Info, "zones : '%s' main %s a %.0f mm", kDefs[i].name, h == 1 ? "droite" : "gauche",
                                  em::Length(tip - z.pos) * 1000.0f);
            }
            if (z.tracker[h].Update(tip, ok, z.pos, z.radius, static_cast<double>(now), cfg->zoneDwellMs, cfg->zoneRearmMs)) {
                z.lastFire = now;
                Logger::Get().Log(LogLevel::Info, "zones : '%s' touchee par la main %s", kDefs[i].name, h == 1 ? "droite" : "gauche");
                Play(Snd::Trigger);
                PostPress(kDefs[i], cfg->zoneServerPort);
            }
        }
    }
}

void Zones_FillMarkers(HandsFrame* frame) {
    if (!frame || !CurrentConfig()->zonesEnabled) return;
    const ULONGLONG now = GetTickCount64();
    auto add = [&](const em::Vec3& p, float size, float r, float g, float b) {
        if (frame->markerCount >= kMaxMarkers) return;
        MarkerData& m = frame->marker[frame->markerCount++];
        m.pos = p;
        m.size = size;
        m.r = r;
        m.g = g;
        m.b = b;
    };
    // Apprentissage en cours : repere blanc qui pulse au bout de l'index (c'est ce point qui sera enregistre).
    if (g_teachAt != 0 && g_teachTipValid) add(g_teachTip, ((now / 250) % 2 == 0) ? 0.012f : 0.020f, 1.0f, 1.0f, 1.0f);
    for (int i = 0; i < kZoneCount; ++i) {
        const ZoneState& z = g_zone[i];
        if (!z.defined) continue;
        if (z.lastCapture != 0 && now - z.lastCapture < 800) add(z.pos, 0.040f, 1.0f, 1.0f, 1.0f);       // memorisee : flash blanc
        else if (z.lastFire != 0 && now - z.lastFire < 800) add(z.pos, 0.030f, 0.10f, 1.0f, 0.20f);   // declenchee : flash vert
        else if (z.inside) add(z.pos, 0.020f, 1.0f, 1.0f, 0.10f);                                      // doigt dedans : jaune
        else add(z.pos, 0.010f, 1.0f, 0.25f, 0.90f);                                                    // zone apprise : magenta
    }
}

}  // namespace layer
