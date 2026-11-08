// zones.cpp — voir zones.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

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
#include "bridge_client.h"
#include "sim/ZoneAction.h"
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

// Une zone : nom, action a envoyer au pont SimConnect, position apprise. Lues dans zones.ini (une section par zone).
struct ZoneState {
    std::string id;
    hands::sim::ZoneAction action;
    bool defined = false;  // position apprise
    em::Vec3 pos;
    float radius = 0.02f;
    em::ZoneTracker tracker[2];
    ULONGLONG lastNear = 0;
    bool inside = false;        // un bout d'index est dans la zone (derniere image)
    ULONGLONG lastFire = 0;     // dernier declenchement
    ULONGLONG lastCapture = 0;  // derniere memorisation
};
std::vector<ZoneState> g_zone;

bool g_loaded = false;
FILETIME g_loadedStamp{};
ULONGLONG g_lastCheck = 0;
bool g_prevTeach = false, g_prevClear = false;
ULONGLONG g_teachAt = 0, g_teachTick = 0;  // enregistrement differe (0 = aucun)
em::Vec3 g_teachTip;                       // bout d'index qui sera enregistre (pour le repere visuel)
bool g_teachTipValid = false;
int g_buttonLogsLeft = 200;

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
    g_zone.clear();
    std::ifstream f(ZonesPath());
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        const auto ini = hands::config::IniFile::Parse(ss.str());
        for (const std::string& sec : ini.Sections()) {
            ZoneState z;
            z.id = sec;
            z.action.type = ini.GetString(sec, "action", "");
            z.action.target = ini.GetString(sec, "target", "");
            z.action.unit = ini.GetString(sec, "unit", "number");
            z.action.value = ini.GetDouble(sec, "value", 1.0);
            z.action.mode = ini.GetString(sec, "mode", z.action.type == "event" || z.action.type == "code" ? "press" : "set");
            z.radius = static_cast<float>(CurrentConfig()->zoneRadiusMm / 1000.0);
            if (ini.Has(sec, "x")) {
                z.pos = {static_cast<float>(ini.GetDouble(sec, "x", 0)), static_cast<float>(ini.GetDouble(sec, "y", 0)),
                         static_cast<float>(ini.GetDouble(sec, "z", 0))};
                const double r = ini.GetDouble(sec, "radius_mm", 0.0);
                if (r >= 5.0 && r <= 100.0) z.radius = static_cast<float>(r / 1000.0);
                z.defined = true;
                for (auto& t : z.tracker) t.Disarm();  // le doigt est peut-etre deja dans la zone : pas de declenchement au chargement
            }
            g_zone.push_back(std::move(z));
        }
    }
    if (g_zone.empty()) {  // premier lancement : une zone d'essai (frein de parc), a apprendre
        ZoneState z;
        z.id = "frein_parc";
        z.action.type = "event";
        z.action.target = "PARKING_BRAKES";
        z.action.mode = "press";
        z.radius = static_cast<float>(CurrentConfig()->zoneRadiusMm / 1000.0);
        g_zone.push_back(std::move(z));
    }
}

void SaveZones() {
    std::ofstream f(ZonesPath(), std::ios::trunc);
    if (!f) {
        Logger::Get().Log(LogLevel::Warn, "zones : ecriture de zones.ini impossible (%s) ; reinstallez la couche (install.ps1 cree data)", ZonesPath().c_str());
        return;
    }
    f << "; Zones du pochoir. Une section par zone. Position apprise (espace du jeu, metres) = x, y, z ; l'action est envoyee au pont SimConnect.\n"
         "; action = event | var | input | code     target = nom (PARKING_BRAKES, L:AZP_RAF_X, nom d'Input Event, ou code avionique)\n"
         "; mode = press | set | toggle              unit = number (variables)      value = 1\n"
         "; Une zone sans x/y/z attend d'etre apprise : le bouton de memorisation apprend la premiere de la liste qui n'a pas de position.\n\n";
    for (const ZoneState& z : g_zone) {
        char b[160];
        f << "[" << z.id << "]\naction = " << z.action.type << "\ntarget = " << z.action.target << "\nmode = " << z.action.mode << "\n";
        if (z.action.type == "var") f << "unit = " << z.action.unit << "\n";
        std::snprintf(b, sizeof(b), "value = %g\n", z.action.value);
        f << b;
        if (z.defined) {
            std::snprintf(b, sizeof(b), "x = %.4f\ny = %.4f\nz = %.4f\nradius_mm = %.0f\n", z.pos.x, z.pos.y, z.pos.z, z.radius * 1000.0f);
            f << b;
        }
        f << "\n";
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

// Envoie l'action de la zone au pont SimConnect (HandsBridge.exe).
void Fire(const ZoneState& z) {
    std::string err;
    const std::string req = hands::sim::BuildBridgeRequest(z.action, &err);
    if (req.empty()) {
        Logger::Get().Log(LogLevel::Warn, "zones : '%s' : action inutilisable (%s) ; corrigez zones.ini", z.id.c_str(), err.c_str());
        return;
    }
    Bridge_Send(req, z.id);
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
                Logger::Get().Log(LogLevel::Info, "zones : activees (fichier : %s) ; Ctrl+Alt+F9 = apprendre la prochaine zone sans position au bout de l'index, Ctrl+Alt+F10 = tout effacer",
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
                // Zone a apprendre : la premiere de la liste qui n'a pas encore de position.
                ZoneState* target = nullptr;
                for (ZoneState& c : g_zone)
                    if (!c.defined) { target = &c; break; }
                if (!target) {
                    Logger::Get().Log(LogLevel::Warn, "zones : toutes les zones ont deja une position ; effacez-les (bouton d'effacement) ou ajoutez-en dans zones.ini");
                    Play(Snd::Fail);
                } else {
                ZoneState& z = *target;
                z.pos = frame.hand[h].joint[kIndexTip].p;
                z.radius = static_cast<float>(cfg->zoneRadiusMm / 1000.0);
                z.defined = true;
                z.lastCapture = now;
                for (auto& t : z.tracker) t.Disarm();  // le doigt est deja dans la zone : il doit en sortir avant de declencher
                SaveZones();
                g_loadedStamp = FileStamp();
                Logger::Get().Log(LogLevel::Info, "zones : '%s' apprise avec la main %s en (%.3f, %.3f, %.3f), rayon %.0f mm", z.id.c_str(),
                                  h == 1 ? "droite" : "gauche", z.pos.x, z.pos.y, z.pos.z, z.radius * 1000.0f);
                Play(Snd::Captured);
                }
            }
        }
    }
    if (clearNow) {
        for (auto& z : g_zone) z.defined = false;  // les actions sont conservees, seules les positions sont effacees
        SaveZones();
        g_loadedStamp = FileStamp();
        Logger::Get().Log(LogLevel::Info, "zones : positions de toutes les zones effacees");
        Play(Snd::Clear);
    }
    g_prevTeach = teachKey;
    g_prevClear = clearKey;

    for (ZoneState& z : g_zone) {
        z.inside = false;
        if (!z.defined) continue;
        for (int h = 0; h < 2; ++h) {
            const HandPoseData& hd = frame.hand[h];
            const bool ok = hd.valid && hd.jointValid[kIndexTip];
            const em::Vec3 tip = ok ? hd.joint[kIndexTip].p : em::Vec3{};
            if (ok && em::Length(tip - z.pos) <= z.radius) z.inside = true;
            if (ok && em::Length(tip - z.pos) < 0.10f && now - z.lastNear > 500) {  // trace de mise au point : a quelle distance passe le doigt ?
                z.lastNear = now;
                Logger::Get().Log(LogLevel::Info, "zones : '%s' main %s a %.0f mm", z.id.c_str(), h == 1 ? "droite" : "gauche",
                                  em::Length(tip - z.pos) * 1000.0f);
            }
            if (z.tracker[h].Update(tip, ok, z.pos, z.radius, static_cast<double>(now), cfg->zoneDwellMs, cfg->zoneRearmMs)) {
                z.lastFire = now;
                Logger::Get().Log(LogLevel::Info, "zones : '%s' touchee par la main %s", z.id.c_str(), h == 1 ? "droite" : "gauche");
                Play(Snd::Trigger);
                Fire(z);
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
    for (const ZoneState& z : g_zone) {
        if (!z.defined) continue;
        if (z.lastCapture != 0 && now - z.lastCapture < 800) add(z.pos, 0.040f, 1.0f, 1.0f, 1.0f);       // memorisee : flash blanc
        else if (z.lastFire != 0 && now - z.lastFire < 800) add(z.pos, 0.030f, 0.10f, 1.0f, 0.20f);   // declenchee : flash vert
        else if (z.inside) add(z.pos, 0.020f, 1.0f, 1.0f, 0.10f);                                      // doigt dedans : jaune
        else add(z.pos, 0.010f, 1.0f, 0.25f, 0.90f);                                                    // zone apprise : magenta
    }
}

}  // namespace layer
