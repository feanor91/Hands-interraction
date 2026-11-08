// zones.cpp — voir zones.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config/IniFile.h"
#include "emulation/ZoneTracker.h"
#include "log/Logger.h"
#include "state.h"
#include "zones.h"
#include "JoyPoll.h"

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
};
ZoneState g_zone[kZoneCount];

bool g_loaded = false;
FILETIME g_loadedStamp{};
ULONGLONG g_lastCheck = 0;
bool g_prevTeach = false, g_prevClear = false;
ULONGLONG g_teachAt = 0, g_teachTick = 0;  // enregistrement differe (0 = aucun)
std::atomic<bool> g_posting{false};
// Alloue et jamais detruit : pas de COM dans un destructeur statique (dechargement de la DLL sous verrou du chargeur).
hands::win::JoyPoll& JoyInstance() {
    static hands::win::JoyPoll* p = new hands::win::JoyPoll;
    return *p;
}

std::string ZonesPath() { return (LayerBaseDir() / "zones.ini").string(); }

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
    }
}

void SaveZones() {
    std::ofstream f(ZonesPath(), std::ios::trunc);
    if (!f) {
        Logger::Get().Log(LogLevel::Warn, "zones : ecriture de zones.ini impossible (%s)", ZonesPath().c_str());
        return;
    }
    f << "; Zones apprises (espace du jeu, metres). Genere par la couche (Ctrl+Alt+F9 : apprendre, Ctrl+Alt+F10 : tout effacer).\n";
    for (int i = 0; i < kZoneCount; ++i) {
        if (!g_zone[i].defined) continue;
        char b[256];
        std::snprintf(b, sizeof(b), "[%s]\nx = %.4f\ny = %.4f\nz = %.4f\nradius_mm = %.0f\n\n", kDefs[i].name, g_zone[i].pos.x, g_zone[i].pos.y,
                      g_zone[i].pos.z, g_zone[i].radius * 1000.0f);
        f << b;
    }
}

// Bip court (sur le son du PC, que Virtual Desktop transmet au casque) : retour de l'apprentissage et du declenchement.
void BeepAsync(DWORD freq, DWORD ms) {
    std::thread([freq, ms] { Beep(freq, ms); }).detach();
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
                Logger::Get().Log(LogLevel::Info, "zones : activees ; Ctrl+Alt+F9 = apprendre 'frein_parc' au bout de l'index, Ctrl+Alt+F10 = tout effacer");
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
            Logger::Get().Log(LogLevel::Info, "zones : bouton %d enfonce (%s)", ev.button, ev.device.c_str());
            if (ev.button == cfg->zoneTeachButton) teachBtn = true;
            if (ev.button == cfg->zoneClearButton) clearBtn = true;
        }
    }
    const bool clearNow = (clearKey && !g_prevClear) || clearBtn;
    // L'enregistrement a lieu `teach_delay_ms` apres l'appui (un bip par seconde), le temps de poser le bout de l'index sur la commande.
    if ((teachKey && !g_prevTeach) || teachBtn) {
        g_teachAt = now + static_cast<ULONGLONG>(cfg->zoneTeachDelayMs);
        g_teachTick = now;
        Logger::Get().Log(LogLevel::Info, "zones : apprentissage dans %.0f s, posez le bout de l'index sur la commande", cfg->zoneTeachDelayMs / 1000.0);
        BeepAsync(800, 60);
    }
    if (g_teachAt != 0 && now - g_teachTick >= 1000 && now < g_teachAt) {
        g_teachTick = now;
        BeepAsync(800, 40);
    }
    if (g_teachAt != 0 && now >= g_teachAt) {
        int h = -1;
        for (int k : {1, 0})
            if (frame.hand[k].valid && frame.hand[k].jointValid[kIndexTip]) {
                h = k;
                break;
            }
        if (h < 0) {
            // Main pas (encore) suivie : on reessaie a chaque image pendant 5 s avant d'abandonner.
            if (now >= g_teachAt + 5000) {
                g_teachAt = 0;
                Logger::Get().Log(LogLevel::Warn, "zones : apprentissage abandonne, aucun bout d'index suivi pendant 5 s (la main est-elle visible des cameras ?)");
                BeepAsync(300, 200);
            }
        } else {
            g_teachAt = 0;
            ZoneState& z = g_zone[0];
            z.pos = frame.hand[h].joint[kIndexTip].p;
            z.radius = static_cast<float>(cfg->zoneRadiusMm / 1000.0);
            z.defined = true;
            z.tracker[0].Reset();
            z.tracker[1].Reset();
            SaveZones();
            g_loadedStamp = FileStamp();
            Logger::Get().Log(LogLevel::Info, "zones : '%s' apprise avec la main %s en (%.3f, %.3f, %.3f), rayon %.0f mm", kDefs[0].name,
                              h == 1 ? "droite" : "gauche", z.pos.x, z.pos.y, z.pos.z, z.radius * 1000.0f);
            BeepAsync(1500, 90);
        }
    }
    if (clearNow) {
        for (auto& z : g_zone) z.defined = false;
        SaveZones();
        g_loadedStamp = FileStamp();
        Logger::Get().Log(LogLevel::Info, "zones : toutes les zones effacees");
        BeepAsync(500, 150);
    }
    g_prevTeach = teachKey;
    g_prevClear = clearKey;

    for (int i = 0; i < kZoneCount; ++i) {
        ZoneState& z = g_zone[i];
        if (!z.defined) continue;
        for (int h = 0; h < 2; ++h) {
            const HandPoseData& hd = frame.hand[h];
            const bool ok = hd.valid && hd.jointValid[kIndexTip];
            const em::Vec3 tip = ok ? hd.joint[kIndexTip].p : em::Vec3{};
            if (ok && em::Length(tip - z.pos) < 0.10f && now - z.lastNear > 500) {  // trace de mise au point : a quelle distance passe le doigt ?
                z.lastNear = now;
                Logger::Get().Log(LogLevel::Info, "zones : '%s' main %s a %.0f mm", kDefs[i].name, h == 1 ? "droite" : "gauche",
                                  em::Length(tip - z.pos) * 1000.0f);
            }
            if (z.tracker[h].Update(tip, ok, z.pos, z.radius, static_cast<double>(now), cfg->zoneDwellMs, cfg->zoneRearmMs)) {
                Logger::Get().Log(LogLevel::Info, "zones : '%s' touchee par la main %s", kDefs[i].name, h == 1 ? "droite" : "gauche");
                BeepAsync(1000, 50);
                PostPress(kDefs[i], cfg->zoneServerPort);
            }
        }
    }
}

}  // namespace layer
