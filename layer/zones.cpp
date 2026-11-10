// zones.cpp — voir zones.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "JoyPoll.h"
#include "bridge_client.h"
#include "config/IniFile.h"
#include "emulation/CockpitAnchor.h"
#include "emulation/ZoneBox.h"
#include "emulation/ZoneTracker.h"
#include "log/Logger.h"
#include "sim/ZoneFile.h"
#include "util/KeyNames.h"
#include "state.h"
#include "zones.h"

namespace layer {
namespace {

namespace em = hands::emulation;
namespace sim = hands::sim;
using hands::log::LogLevel;
using hands::log::Logger;

constexpr int kIndexTip = 10;  // XR_HAND_JOINT_INDEX_TIP_EXT
constexpr ULONGLONG kEngageMs = 200;  // pointage : maintien du doigt dans la boite avant de saisir (evite les effleurements)
constexpr int kPalm = 0, kWrist = 1, kMiddleProximal = 12;

// Une zone : type de commande + actions (zones.ini), position(s) apprise(s), et l'etat des gestes en cours.
struct ZoneState {
    std::string id;
    sim::ZoneControl ctrl;
    bool defined = false;   // premier point appris
    em::Vec3 pos;
    bool defined2 = false;  // curseur : fin de course apprise
    em::Vec3 pos2;
    sim::ZoneGeom geom;  // boite : dimensions (mm) et orientation (deg)
    em::ZoneTracker tracker[2];
    ULONGLONG lastNear = 0;
    bool inside = false;        // un bout d'index est dans la zone (derniere image)
    ULONGLONG lastFire = 0;     // dernier declenchement
    ULONGLONG lastCapture = 0;  // derniere memorisation
    // Gestes (rotatif, curseur) : une main saisit la commande en pincant dedans.
    bool prevPinch[2] = {false, false};
    ULONGLONG nearSince[2] = {0, 0};  // geste « pointage » (glisse sans pincement) : depuis quand le doigt est dans la boite
    int grabHand = -1;
    int grabLevel = 1;      // molette a deux niveaux : 1 = bague exterieure, 2 = bouton central
    bool insideInner = false;
    ULONGLONG hudUntil = 0;   // indicateur de position (slider affiche au-dessus de la molette) visible jusqu'a cet instant
    ULONGLONG lastRead = 0;   // derniere lecture de la position dans le simulateur
    double hudLevel = 0.5;    // curseur : position 0..1 a afficher
    bool engagedLatch[2] = {false, false};  // pointage : une seule saisie par passage du doigt
    ULONGLONG grabAt = 0;
    bool rotated = false;
    em::Vec3 axis0, n0;
    double lastAngle = 0.0;
    sim::DialAccumulator dial;
    double prevLevel = 0.0, lastSentLevel = -1.0;
    ULONGLONG lastSend = 0;
};
std::vector<ZoneState> g_zone;

// Repere du cockpit : les positions des zones sont relatives a ce repere (recale par la touche de recentrage de MSFS).
em::CockpitAnchor g_anchor;
ULONGLONG g_recenterAt = 0;  // recentrage demande : instant ou relever la pose de la tete (0 = aucun)
bool g_prevRecenterKey = false;

// Boite de la zone (espace du jeu). Curseur : le long de la course apprise ; sinon centree sur le point appris.
em::ZoneBox BoxOf(const ZoneState& z) {
    if (z.ctrl.kind == "slider" && z.defined2) return em::SegmentBox(g_anchor.ToWorld(z.pos), g_anchor.ToWorld(z.pos2), static_cast<float>(z.geom.sx / 1000.0), static_cast<float>(z.geom.sy / 1000.0));
    em::ZoneBox b;
    b.center = g_anchor.ToWorld(z.pos);
    b.q = g_anchor.ToWorldRot(em::QuatFromEulerDeg(z.geom.yaw, z.geom.pitch, z.geom.roll));
    b.size = {static_cast<float>(z.geom.sx / 1000.0), static_cast<float>(z.geom.sy / 1000.0), static_cast<float>(z.geom.sz / 1000.0)};
    return b;
}

void DefaultGeom(ZoneState& z) {
    sim::DefaultSizeMm(z.ctrl.kind, &z.geom.sx, &z.geom.sy, &z.geom.sz);
}

bool Complete(const ZoneState& z) { return z.defined && (z.ctrl.kind != "slider" || z.defined2); }

bool g_loaded = false;
FILETIME g_loadedStamp{};
ULONGLONG g_lastCheck = 0;
std::string g_capturedId;  // derniere zone dont l'apprentissage vient de se terminer (lue une fois par Zones_TakeCaptured)
std::string g_teachId;  // zone visee par l'apprentissage en cours (vide = premiere incomplete)
int g_teachStage = 1;   // curseur : 1 = debut de course, 2 = fin de course
bool g_prevTeach = false, g_prevClear = false;
ULONGLONG g_teachAt = 0, g_teachTick = 0;  // enregistrement differe (0 = aucun)
ULONGLONG g_teachPinchUntil = 0;           // attente d'un pincement : jusqu'a cet instant (0 = pas en attente)
ULONGLONG g_teachBegan = 0;                // debut de l'attente (les 600 premieres ms ne comptent pas : on vient de lacher le menu)
bool g_prevPress[2] = {false, false};      // pincement de chaque main a l'image precedente

// Historique du bout de l'index de chaque main : la position memorisee est celle d'AVANT le pincement (qui deplace le doigt de quelques centimetres).
struct TipSample {
    ULONGLONG t = 0;
    bool valid = false;
    em::Vec3 tip, dir;  // bout de l'index et sa direction (articulation distale -> bout)
};
constexpr int kHist = 96;
TipSample g_hist[2][kHist];
int g_histHead[2] = {0, 0};

TipSample SampleAt(int hand, ULONGLONG t) {  // dernier echantillon valide a l'instant t ou avant
    for (int i = 0; i < kHist; ++i) {
        const TipSample& s = g_hist[hand][(g_histHead[hand] - 1 - i + kHist * 2) % kHist];
        if (s.valid && s.t <= t) return s;
    }
    return TipSample{};
}
em::Vec3 g_teachTip;                       // bout d'index qui sera enregistre (pour le repere visuel)
bool g_teachTipValid = false;
int g_buttonLogsLeft = 200;

// Interrupteurs a N positions : derniere position envoyee et sens (aller-retour), par zone. Touches depuis le fil du pont.
// Position (0..1) de chaque molette, niveau 1 (bague) et 2 (centre), pour l'indicateur : lue dans le simulateur quand on sait la lire, sinon estimee
// a partir des crans envoyes. Touchee depuis le fil du pont.
std::mutex g_lvMutex;
std::map<std::string, std::array<double, 2>> g_levels;
double GetLevel(const std::string& id, int lvl) {
    std::lock_guard<std::mutex> l(g_lvMutex);
    auto it = g_levels.find(id);
    return it == g_levels.end() ? 0.5 : it->second[lvl == 2 ? 1 : 0];
}
void SetLevel(const std::string& id, int lvl, double v) {
    std::lock_guard<std::mutex> l(g_lvMutex);
    auto it = g_levels.find(id);
    if (it == g_levels.end()) it = g_levels.emplace(id, std::array<double, 2>{0.5, 0.5}).first;
    it->second[lvl == 2 ? 1 : 0] = std::max(0.0, std::min(1.0, v));
}

std::mutex g_swMutex;
std::map<std::string, std::pair<int, int>> g_switchState;

// Alloue et jamais detruit : pas de COM dans un destructeur statique (dechargement de la DLL sous verrou du chargeur).
hands::win::JoyPoll& JoyInstance() {
    static hands::win::JoyPoll* p = new hands::win::JoyPoll;
    return *p;
}

// zones.ini : le dossier de la DLL n'est inscriptible que dans logs (et data depuis la version du 8 novembre de install.ps1) ; on prend data
// s'il existe, sinon logs (installations anterieures).
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

em::Vec3 V(double x, double y, double z) { return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)}; }

void LoadZones() {
    g_zone.clear();
    std::ifstream f(ZonesPath());
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        for (const sim::ZoneRecord& r : sim::ParseZones(ss.str())) {
            ZoneState z;
            z.id = r.id;
            z.ctrl = r.control;
            z.geom = r.geom;
            if (z.geom.sx <= 0) DefaultGeom(z);
            if (r.hasPos) {
                z.defined = true;
                z.pos = V(r.x, r.y, r.z);
            }
            if (r.hasPos2) {
                z.defined2 = true;
                z.pos2 = V(r.x2, r.y2, r.z2);
            }
            for (auto& t : z.tracker) t.Disarm();  // le doigt est peut-etre deja dans la zone : pas de declenchement au chargement
            g_zone.push_back(std::move(z));
        }
    }
    if (g_zone.empty()) {  // premier lancement : une zone d'essai (frein de parc), a apprendre
        ZoneState z;
        z.id = "frein_parc";
        z.ctrl.kind = "push";
        z.ctrl.slots["press"] = sim::ParseSeq("event|press|1|number|||0|PARKING_BRAKES");
        DefaultGeom(z);
        g_zone.push_back(std::move(z));
    }
}

void SaveZones() {
    std::vector<sim::ZoneRecord> recs;
    for (const ZoneState& z : g_zone) {
        sim::ZoneRecord r;
        r.id = z.id;
        r.control = z.ctrl;
        r.geom = z.geom;
        if (z.defined) { r.hasPos = true; r.x = z.pos.x; r.y = z.pos.y; r.z = z.pos.z; }
        if (z.defined2) { r.hasPos2 = true; r.x2 = z.pos2.x; r.y2 = z.pos2.y; r.z2 = z.pos2.z; }
        recs.push_back(std::move(r));
    }
    std::ofstream f(ZonesPath(), std::ios::trunc | std::ios::binary);
    if (!f) {
        Logger::Get().Log(LogLevel::Warn, "zones : ecriture de zones.ini impossible (%s) ; reinstallez la couche (install.ps1 cree data)", ZonesPath().c_str());
        return;
    }
    f << sim::SerializeZones(recs);
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


bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Main dont le bout d'index sert a memoriser : la droite si elle est suivie, sinon la gauche. -1 si aucune.
int TeachHand(const HandsFrame& frame) {
    for (int k : {1, 0})
        if (frame.hand[k].valid && frame.hand[k].jointValid[kIndexTip]) return k;
    return -1;
}

// --- Actions ------------------------------------------------------------------------------------------------------------------------

// Contact simple (poussoir, bascule) : envoie la sequence `press`.
void FirePress(const ZoneState& z) { Bridge_RunSeq(sim::Slot(z.ctrl, "press"), z.id); }

// Interrupteur a N positions : position suivante (lue dans le simulateur si `read` est defini), puis sequence de cette position.
void FireSwitch(const ZoneState& z) {
    const sim::ZoneControl c = z.ctrl;
    const std::string id = z.id;
    Bridge_Post([c, id] {
        int cur = 0, dir = 1;
        {
            std::lock_guard<std::mutex> l(g_swMutex);
            auto it = g_switchState.find(id);
            if (it != g_switchState.end()) { cur = it->second.first; dir = it->second.second; }
        }
        if (!c.read.empty()) {  // « var|NOM|unite » ou « input|NOM »
            std::vector<std::string> f;
            std::stringstream rs(c.read);
            for (std::string t; std::getline(rs, t, '|');) f.push_back(t);
            std::string req;
            if (f.size() >= 2 && f[0] == "var") req = "getvar	" + f[1] + "	" + (f.size() >= 3 ? f[2] : std::string("number"));
            else if (f.size() >= 2 && f[0] == "input") req = "getinput	" + f[1];
            double v = 0;
            std::vector<double> table;
            std::stringstream ss(c.readValues);
            for (std::string t; std::getline(ss, t, ',');) table.push_back(std::atof(t.c_str()));
            if (!req.empty() && Bridge_ReadNumber(req, &v)) cur = sim::PositionOf(v, table, c.count);
        }
        int pos = 0, nd = 1;
        sim::NextPosition(c.count, c.switchMode, cur, dir, &pos, &nd);
        {
            std::lock_guard<std::mutex> l(g_swMutex);
            g_switchState[id] = {pos, nd};
        }
        Bridge_RunSeqNow(sim::Slot(c, "pos" + std::to_string(pos + 1)), id + " position " + std::to_string(pos + 1));
    });
}

// --- Gestes de saisie (rotatif, curseur) -------------------------------------------------------------------------------------------

em::Vec3 PerpTo(em::Vec3 v, em::Vec3 axis) { return em::Normalized(v - axis * em::Dot(v, axis)); }

double DistToSegment(em::Vec3 p, em::Vec3 a, em::Vec3 b, double* level) {
    const em::Vec3 ab = b - a;
    const double l2 = em::Dot(ab, ab);
    double t = l2 > 1e-9 ? em::Dot(p - a, ab) / l2 : 0.0;
    t = std::max(0.0, std::min(1.0, t));
    if (level) *level = t;
    return em::Length(p - (a + ab * static_cast<float>(t)));
}

std::string LevelSlot(const ZoneState& z, const char* base) { return z.grabLevel == 2 ? std::string(base) + "2" : std::string(base); }

void EndGrab(ZoneState& z, ULONGLONG now) {
    if (z.grabHand < 0) return;
    // Rotatif : pincer et relacher sans bouger = appui sur le bouton (de la bague ou du centre, selon le niveau saisi).
    const sim::ActionSeq& press = sim::Slot(z.ctrl, LevelSlot(z, "press"));
    if (z.ctrl.kind == "dial" && !z.rotated && now - z.grabAt < 800 && !press.empty()) {
        Bridge_RunSeq(press, z.id + " appui");
        z.lastFire = now;
        Play(Snd::Trigger);
    }
    z.grabHand = -1;
}

// Bouton central d'une molette a deux niveaux : meme centre et meme orientation, section reduite.
em::ZoneBox InnerBoxOf(const ZoneState& z) {
    em::ZoneBox b = BoxOf(z);
    b.size.x *= static_cast<float>(z.ctrl.innerRatio);
    b.size.y *= static_cast<float>(z.ctrl.innerRatio);
    return b;
}

// Ou lire la position d'une molette ? Variable L: / Input Event de l'action « cran + » si c'est un ajout borne ; sinon `read`, `min`, `max` de zones.ini (bague seulement).
bool LevelSource(const ZoneState& z, int lvl, std::string* req, double* lo, double* hi) {
    const sim::ActionSeq& inc = sim::Slot(z.ctrl, lvl == 2 ? "inc2" : "inc");
    if (!inc.empty() && inc[0].mode == "add" && inc[0].hasMin && inc[0].hasMax && inc[0].max > inc[0].min) {
        if (inc[0].type == "var") *req = "getvar\t" + inc[0].target + "\t" + (inc[0].unit.empty() ? std::string("number") : inc[0].unit);
        else if (inc[0].type == "input") *req = "getinput\t" + inc[0].target;
        else return false;
        *lo = inc[0].min;
        *hi = inc[0].max;
        return true;
    }
    if (lvl == 1 && !z.ctrl.read.empty() && z.ctrl.max > z.ctrl.min) {
        std::vector<std::string> f;
        std::stringstream rs(z.ctrl.read);
        for (std::string t; std::getline(rs, t, '|');) f.push_back(t);
        if (f.size() >= 2 && f[0] == "var") *req = "getvar\t" + f[1] + "\t" + (f.size() >= 3 ? f[2] : std::string("number"));
        else if (f.size() >= 2 && f[0] == "input") *req = "getinput\t" + f[1];
        else return false;
        *lo = z.ctrl.min;
        *hi = z.ctrl.max;
        return true;
    }
    return false;
}

void RefreshLevel(ZoneState& z, int lvl, ULONGLONG now) {
    std::string req;
    double lo = 0, hi = 1;
    if (!LevelSource(z, lvl, &req, &lo, &hi) || now - z.lastRead < 150) return;
    z.lastRead = now;
    const std::string id = z.id;
    Bridge_Post([req, lo, hi, id, lvl] {
        double v = 0;
        if (Bridge_ReadNumber(req, &v)) SetLevel(id, lvl, (v - lo) / (hi - lo));
    });
}

void SendSteps(ZoneState& z, int steps, ULONGLONG now) {
    z.rotated = true;
    const std::string slot = LevelSlot(z, steps > 0 ? "inc" : "dec");
    for (int i = 0; i < std::min(6, std::abs(steps)); ++i) Bridge_RunSeq(sim::Slot(z.ctrl, slot), z.id + (z.grabLevel == 2 ? " centre" : "") + (steps > 0 ? " +" : " -"));
    z.lastFire = now;
    z.hudUntil = now + 1800;
    std::string req;
    double lo, hi;
    if (LevelSource(z, z.grabLevel, &req, &lo, &hi)) RefreshLevel(z, z.grabLevel, now);
    else SetLevel(z.id, z.grabLevel, GetLevel(z.id, z.grabLevel) + static_cast<double>(steps) / std::max(2, z.ctrl.notches));  // position estimee
    Play(Snd::Tick);
}

void UpdateDial(ZoneState& z, const HandsFrame& frame, ULONGLONG now) {
    z.inside = false;
    z.insideInner = false;
    for (int h = 0; h < 2; ++h) {
        const HandPoseData& hd = frame.hand[h];
        const bool ok = hd.valid && hd.jointValid[kIndexTip] && hd.jointValid[kPalm] && hd.jointValid[kWrist] && hd.jointValid[kMiddleProximal];
        const em::ZoneBox box = BoxOf(z);
        const float margin = static_cast<float>(CurrentConfig()->zoneFingerMm / 2000.0);
        const bool isNear = ok && em::BoxContains(box, hd.joint[kIndexTip].p, margin, 1.2f);
        const bool inInner = isNear && z.ctrl.dual && em::BoxContains(InnerBoxOf(z), hd.joint[kIndexTip].p, margin);
        if (isNear) z.inside = true;
        if (inInner) z.insideInner = true;
        const bool pinch = ok && hd.pressed;
        const bool slide = z.ctrl.gesture == "slide";  // glisse : simple pointage, sans pincement ; le doigt doit rester `kEngageMs` dans la boite
        if (!isNear) z.nearSince[h] = 0;
        else if (z.nearSince[h] == 0) z.nearSince[h] = now;
        const bool engage = slide ? (isNear && now - z.nearSince[h] >= kEngageMs && !z.engagedLatch[h]) : (isNear && pinch && !z.prevPinch[h]);
        if (!isNear) z.engagedLatch[h] = false;
        if (z.grabHand < 0 && engage) {  // saisie ; le centre a la priorite
            z.engagedLatch[h] = true;
            z.grabHand = h;
            z.grabLevel = inInner ? 2 : 1;
            z.hudUntil = now + 1800;
            z.lastRead = 0;
            RefreshLevel(z, z.grabLevel, now);
            z.grabAt = now;
            z.rotated = false;
            z.dial = sim::DialAccumulator{};
            z.axis0 = em::Normalized(hd.joint[kMiddleProximal].p - hd.joint[kWrist].p);
            z.n0 = PerpTo(em::Rotate(hd.joint[kPalm].q, em::Vec3{0, 1, 0}), z.axis0);
            z.lastAngle = 0.0;
            if (z.ctrl.gesture == "slide") z.lastAngle = em::ToLocal(box, hd.joint[kIndexTip].p).x * 1000.0;  // glisse : position (mm) le long de la boite
        } else if (z.grabHand == h) {
            if (slide ? !em::BoxContains(box, hd.joint[kIndexTip].p, margin, 1.3f) : (!pinch || !em::BoxContains(box, hd.joint[kIndexTip].p, margin, 2.0f))) {
                EndGrab(z, now);
            } else if (slide) {
                const double x = em::ToLocal(box, hd.joint[kIndexTip].p).x * 1000.0;
                const double d = x - z.lastAngle;
                z.lastAngle = x;
                const int steps = z.dial.Feed(z.ctrl.invert ? -d : d, z.ctrl.stepDeg);
                if (steps != 0) SendSteps(z, steps, now);
            } else {
                const em::Vec3 n1 = PerpTo(em::Rotate(hd.joint[kPalm].q, em::Vec3{0, 1, 0}), z.axis0);
                const double ang = std::atan2(static_cast<double>(em::Dot(z.axis0, em::Cross(z.n0, n1))), static_cast<double>(em::Dot(z.n0, n1))) * 57.29577951;
                double d = ang - z.lastAngle;
                while (d > 180.0) d -= 360.0;
                while (d < -180.0) d += 360.0;
                z.lastAngle = ang;
                const int steps = z.dial.Feed(z.ctrl.invert ? -d : d, z.ctrl.stepDeg);
                if (steps != 0) SendSteps(z, steps, now);
            }
        }
        z.prevPinch[h] = pinch;
    }
}

void UpdateSlider(ZoneState& z, const HandsFrame& frame, ULONGLONG now) {
    z.inside = false;
    for (int h = 0; h < 2; ++h) {
        const HandPoseData& hd = frame.hand[h];
        const bool ok = hd.valid && hd.jointValid[kIndexTip];
        double level = 0.0;
        if (ok) DistToSegment(hd.joint[kIndexTip].p, g_anchor.ToWorld(z.pos), g_anchor.ToWorld(z.pos2), &level);
        const em::ZoneBox box = BoxOf(z);
        const float margin = static_cast<float>(CurrentConfig()->zoneFingerMm / 2000.0);
        const bool isNear = ok && em::BoxContains(box, hd.joint[kIndexTip].p, margin, 1.2f);
        if (isNear) z.inside = true;
        const bool pinch = ok && hd.pressed;  // le curseur se commande en pointant : le pincement n'est pas utilise
        if (!isNear) z.nearSince[h] = 0;
        else if (z.nearSince[h] == 0) z.nearSince[h] = now;
        const bool engage = isNear && now - z.nearSince[h] >= kEngageMs && !z.engagedLatch[h];
        if (!isNear) z.engagedLatch[h] = false;
        (void)pinch;
        if (z.grabHand == h) {
            z.hudLevel = level;
            z.hudUntil = now + 1200;
        }
        if (z.grabHand < 0 && engage) {
            z.engagedLatch[h] = true;
            z.grabHand = h;
            z.grabAt = now;
            z.prevLevel = level;
            z.lastSentLevel = -1.0;
        } else if (z.grabHand == h) {
            if (!em::BoxContains(box, hd.joint[kIndexTip].p, margin, 1.3f)) {
                EndGrab(z, now);
            } else if (z.ctrl.sliderMode == "value") {
                if (std::fabs(level - z.lastSentLevel) >= 0.02 && now - z.lastSend >= 120) {
                    sim::ActionSeq seq = sim::Slot(z.ctrl, "set");
                    const double v = sim::LevelToValue(level, z.ctrl.min, z.ctrl.max);
                    for (sim::ZoneAction& a : seq)
                        if (a.type != "delay") a.value = v;
                    if (Bridge_RunSeq(seq, z.id + " valeur")) {
                        z.lastSentLevel = level;
                        z.lastSend = now;
                        z.lastFire = now;
                    }
                }
            } else {
                const int d = sim::NotchDelta(z.prevLevel, level, z.ctrl.notches);
                if (d != 0) {
                    const char* slot = d > 0 ? "inc" : "dec";
                    for (int i = 0; i < std::min(6, std::abs(d)); ++i) Bridge_RunSeq(sim::Slot(z.ctrl, slot), z.id + (d > 0 ? " +" : " -"));
                    // La reference avance du nombre de crans effectivement traites.
                    z.prevLevel += static_cast<double>(d) / z.ctrl.notches;
                    z.lastFire = now;
                    Play(Snd::Tick);
                }
            }
        }
        z.prevPinch[h] = pinch;
    }
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
                Logger::Get().Log(LogLevel::Info, "zones : activees (fichier : %s) ; Ctrl+Alt+F9 = apprendre la prochaine zone incomplete au bout de l'index, Ctrl+Alt+F10 = effacer les positions",
                                  ZonesPath().c_str());
            g_loaded = true;
        }
    }

    // Recentrage de la vue dans MSFS (meme touche) : le cockpit se replace autour de la tete, les zones suivent.
    {
        const int vk = hands::util::ParseVirtualKey(cfg->zoneRecenterKey);
        const bool down = vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !g_prevRecenterKey) g_recenterAt = now + static_cast<ULONGLONG>(cfg->zoneRecenterDelayMs);
        g_prevRecenterKey = down;
        if (g_recenterAt != 0 && now >= g_recenterAt) {
            if (frame.headValid) {
                g_anchor = em::CockpitAnchor::FromHead(frame.head);
                g_recenterAt = 0;
                for (ZoneState& z : g_zone) {
                    for (auto& t : z.tracker) t.Disarm();
                    z.grabHand = -1;
                }
                Logger::Get().Log(LogLevel::Info, "zones : cockpit recentre (touche %s) : repere a la tete en (%.2f, %.2f, %.2f)", cfg->zoneRecenterKey.c_str(),
                                  g_anchor.pos.x, g_anchor.pos.y, g_anchor.pos.z);
                Play(Snd::Clear);
            } else if (now >= g_recenterAt + 3000) {
                g_recenterAt = 0;
                Logger::Get().Log(LogLevel::Warn, "zones : recentrage ignore, pose de la tete indisponible");
            }
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
    // Historique du bout de l'index (jamais interrompu).
    for (int h = 0; h < 2; ++h) {
        TipSample& sm = g_hist[h][g_histHead[h]];
        const HandPoseData& hd = frame.hand[h];
        sm.t = now;
        sm.valid = hd.valid && hd.jointValid[kIndexTip] && hd.jointValid[9];
        if (sm.valid) {
            sm.tip = hd.joint[kIndexTip].p;
            const em::Vec3 d = hd.joint[kIndexTip].p - hd.joint[9].p;
            const float l = em::Length(d);
            sm.dir = l > 1e-4f ? d * (1.0f / l) : em::Vec3{0, 0, -1};
        }
        g_histHead[h] = (g_histHead[h] + 1) % kHist;
    }
    const bool pinchMode = cfg->zoneTeachMode != "timer";

    // Demande d'apprentissage (bouton de manette / HOTAS, ou Ctrl+Alt+F9) : par pincement (defaut) ou compte a rebours.
    if ((teachKey && !g_prevTeach) || teachBtn) {
        g_teachStage = 1;
        for (const ZoneState& c : g_zone)
            if (g_teachId.empty() && !Complete(c)) { g_teachStage = c.defined ? 2 : 1; break; }
        if (pinchMode) {
            g_teachPinchUntil = now + 60000;
            g_teachBegan = now;
            g_teachAt = 0;
            Logger::Get().Log(LogLevel::Info, "zones : apprentissage : pointez la commande du bout de l'index puis PINCEZ (60 s)");
        } else {
            g_teachAt = now + static_cast<ULONGLONG>(cfg->zoneTeachDelayMs);
            g_teachTick = now;
            Logger::Get().Log(LogLevel::Info, "zones : apprentissage dans %.1f s, posez le bout de l'index sur la commande", cfg->zoneTeachDelayMs / 1000.0);
        }
        Play(Snd::Start);
    }
    g_teachTipValid = false;
    if (g_teachAt != 0 || g_teachPinchUntil != 0) {
        const int th = TeachHand(frame);
        if (th >= 0) {
            g_teachTip = frame.hand[th].joint[kIndexTip].p;
            g_teachTipValid = true;
        }
    }

    // Capture d'une position : choisit la zone visee, enregistre, enchaine sur la fin de course d'un curseur.
    auto capture = [&](const em::Vec3& p, const em::Vec3& dir, int h) {
        ZoneState* target = nullptr;
        for (ZoneState& c : g_zone)
            if (!g_teachId.empty() ? c.id == g_teachId : !Complete(c)) { target = &c; break; }
        if (!target) {
            g_teachId.clear();
            g_teachPinchUntil = 0;
            Logger::Get().Log(LogLevel::Warn, "zones : toutes les zones ont deja leur position ; effacez-les (bouton d'effacement) ou ajoutez-en");
            Play(Snd::Fail);
            return;
        }
        ZoneState& z = *target;
        const bool second = z.ctrl.kind == "slider" && (g_teachStage == 2 || (z.defined && !z.defined2 && g_teachId.empty()));
        if (second) {
            z.pos2 = g_anchor.ToRel(p);
            z.defined2 = true;
        } else {
            z.pos = g_anchor.ToRel(p);
            z.defined = true;
            z.defined2 = z.ctrl.kind == "slider" ? false : z.defined2;
            if (z.geom.sx <= 0) DefaultGeom(z);
            // Orientation de la boite d'apres la direction du doigt : la face de la commande est opposee au doigt qui l'enfonce (commande a plat :
            // doigt vers le bas -> boite a plat face vers le haut ; commande verticale : face vers le pilote, tournee de la direction du doigt, par pas de 15 degres).
            if (z.ctrl.kind != "slider" && z.geom.yaw == 0 && z.geom.pitch == 0 && z.geom.roll == 0) {
                const em::Vec3 n = em::Rotate(em::Conjugate(g_anchor.q), dir * -1.0f);  // normale de la face, dans le repere du cockpit
                if (n.y > 0.7f) z.geom.pitch = -90.0;
                else if (n.y < -0.7f) z.geom.pitch = 90.0;
                else {
                    const double yaw = std::atan2(static_cast<double>(n.x), static_cast<double>(n.z)) * 57.29577951;
                    z.geom.yaw = std::max(-60.0, std::min(60.0, std::round(yaw / 15.0) * 15.0));
                }
            }
        }
        z.lastCapture = GetTickCount64();
        for (auto& t : z.tracker) t.Disarm();  // le doigt est deja dans la zone : il doit en sortir avant de declencher
        SaveZones();
        g_loadedStamp = FileStamp();
        Logger::Get().Log(LogLevel::Info, "zones : '%s' %s apprise avec la main %s en (%.3f, %.3f, %.3f), boite %.0fx%.0fx%.0f mm, rotation (%.0f, %.0f, %.0f)", z.id.c_str(),
                          second ? "(fin de course)" : (z.ctrl.kind == "slider" ? "(debut de course)" : ""), h == 1 ? "droite" : "gauche", p.x, p.y, p.z,
                          z.geom.sx, z.geom.sy, z.geom.sz, z.geom.yaw, z.geom.pitch, z.geom.roll);
        Play(Snd::Captured);
        if (z.ctrl.kind == "slider" && !second) {  // curseur : enchaine sur la fin de course
            g_teachId = z.id;
            g_teachStage = 2;
            if (cfg->zoneTeachMode != "timer") {
                g_teachPinchUntil = GetTickCount64() + 60000;
                g_teachBegan = GetTickCount64();
                Logger::Get().Log(LogLevel::Info, "zones : pointez maintenant la FIN de course de '%s' et pincez", z.id.c_str());
            } else {
                g_teachAt = GetTickCount64() + 3000;
                g_teachTick = GetTickCount64();
                Logger::Get().Log(LogLevel::Info, "zones : posez maintenant l'index sur la FIN de course de '%s' (3 s)", z.id.c_str());
            }
        } else {
            g_teachId.clear();
            g_teachPinchUntil = 0;
            g_capturedId = z.id;  // apprentissage termine : le menu en VR peut se rouvrir sur cette zone
        }
    };

    // --- Attente d'un pincement : la position memorisee est celle du doigt 300 ms avant le pincement ---------------------------------
    if (g_teachPinchUntil != 0) {
        if (now - g_teachTick >= 2000) {
            g_teachTick = now;
            Play(Snd::Tick);
        }
        for (int h : {1, 0}) {
            const bool edge = frame.hand[h].valid && frame.hand[h].pressed && !g_prevPress[h];
            if (!edge || now - g_teachBegan < 600) continue;
            const TipSample sm = SampleAt(h, now > 300 ? now - 300 : 0);
            if (!sm.valid) continue;
            capture(sm.tip, sm.dir, h);
            break;
        }
        if (g_teachPinchUntil != 0 && now > g_teachPinchUntil) {
            g_teachPinchUntil = 0;
            g_teachId.clear();
            Logger::Get().Log(LogLevel::Warn, "zones : apprentissage abandonne (aucun pincement pendant 60 s)");
            Play(Snd::Fail);
        }
    }

    // --- Compte a rebours (teach_mode = timer) ------------------------------------------------------------------------------------
    if (g_teachAt != 0) {
        const int h = TeachHand(frame);
        if (now < g_teachAt && now - g_teachTick >= 1000) {
            g_teachTick = now;
            Play(Snd::Tick);
        }
        if (now >= g_teachAt) {
            if (h < 0) {
                // Main pas (encore) suivie : on reessaie a chaque image pendant 5 s avant d'abandonner.
                if (now >= g_teachAt + 5000) {
                    g_teachAt = 0;
                    g_teachId.clear();
                    Logger::Get().Log(LogLevel::Warn, "zones : apprentissage abandonne, aucun bout d'index suivi pendant 5 s (la main est-elle visible des cameras ?)");
                    Play(Snd::Fail);
                }
            } else {
                g_teachAt = 0;
                const TipSample sm = SampleAt(h, now);
                capture(frame.hand[h].joint[kIndexTip].p, sm.valid ? sm.dir : em::Vec3{0, 0, -1}, h);
            }
        }
    }
    for (int h = 0; h < 2; ++h) g_prevPress[h] = frame.hand[h].valid && frame.hand[h].pressed;
    if (clearNow) {
        for (auto& z : g_zone) {
            z.defined = z.defined2 = false;  // les actions sont conservees, seules les positions sont effacees
            z.grabHand = -1;
        }
        SaveZones();
        g_loadedStamp = FileStamp();
        g_teachId.clear();
        Logger::Get().Log(LogLevel::Info, "zones : positions de toutes les zones effacees");
        Play(Snd::Clear);
    }
    g_prevTeach = teachKey;
    g_prevClear = clearKey;

    for (ZoneState& z : g_zone) {
        z.inside = false;
        if (!Complete(z)) continue;
        if (z.ctrl.kind == "dial") { UpdateDial(z, frame, now); continue; }
        if (z.ctrl.kind == "slider") { UpdateSlider(z, frame, now); continue; }
        for (int h = 0; h < 2; ++h) {
            const HandPoseData& hd = frame.hand[h];
            const bool ok = hd.valid && hd.jointValid[kIndexTip];
            const em::Vec3 tip = ok ? hd.joint[kIndexTip].p : em::Vec3{};
            const em::ZoneBox box = BoxOf(z);
            const float margin = static_cast<float>(cfg->zoneFingerMm / 2000.0);
            const bool in = ok && em::BoxContains(box, tip, margin), inWide = ok && em::BoxContains(box, tip, margin, 1.5f);
            if (in) z.inside = true;
            if (ok && em::Length(tip - box.center) < 0.10f && now - z.lastNear > 500) {  // trace de mise au point : a quelle distance passe le doigt ?
                z.lastNear = now;
                Logger::Get().Log(LogLevel::Info, "zones : '%s' main %s a %.0f mm", z.id.c_str(), h == 1 ? "droite" : "gauche", em::Length(tip - box.center) * 1000.0f);
            }
            // Les deux mains peuvent toucher la meme zone : un seul declenchement (200 ms de garde entre les deux mains).
            if (z.tracker[h].UpdateInside(ok, in, inWide, static_cast<double>(now), cfg->zoneDwellMs, cfg->zoneRearmMs) && now - z.lastFire >= 200) {
                z.lastFire = now;
                Logger::Get().Log(LogLevel::Info, "zones : '%s' touchee par la main %s", z.id.c_str(), h == 1 ? "droite" : "gauche");
                Play(Snd::Trigger);
                if (z.ctrl.kind == "switch") FireSwitch(z);
                else FirePress(z);
            }
        }
    }
}

bool Zones_AnyGrab() {
    for (const ZoneState& z : g_zone)
        if (z.grabHand >= 0) return true;
    return false;
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
    if ((g_teachAt != 0 || g_teachPinchUntil != 0) && g_teachTipValid) add(g_teachTip, ((now / 250) % 2 == 0) ? 0.012f : 0.020f, 1.0f, 1.0f, 1.0f);
    // Boites en fil de fer : couleur selon le type de commande, plus epaisses et claires quand le doigt est dedans / saisie / declenchee.
    const auto cfg = CurrentConfig();
    if (cfg->zoneShowBoxes == "hidden") return;
    const float nearM = static_cast<float>(cfg->zoneNearCm / 100.0);
    for (const ZoneState& z : g_zone) {
        if (!z.defined) continue;
        const em::ZoneBox box = BoxOf(z);
        if (cfg->zoneShowBoxes == "near") {
            bool close = z.lastFire != 0 && now - z.lastFire < 800;
            for (int h = 0; h < 2 && !close; ++h)
                if (frame->hand[h].valid && frame->hand[h].jointValid[kIndexTip] && em::Length(frame->hand[h].joint[kIndexTip].p - box.center) < nearM + box.size.z) close = true;
            if (!close && g_teachId != z.id) continue;
        }
        if (frame->boxCount >= kMaxBoxes) break;
        BoxData& bx = frame->box[frame->boxCount++];
        bx.pose = {box.q, box.center};
        bx.size = box.size;
        const std::string& k = z.ctrl.kind;
        if (k == "switch") { bx.r = 1.0f; bx.g = 0.85f; bx.b = 0.15f; }          // jaune : interrupteur
        else if (k == "dial") { bx.r = 1.0f; bx.g = 0.55f; bx.b = 0.10f; }      // orange : rotatif
        else if (k == "slider") { bx.r = 0.30f; bx.g = 0.95f; bx.b = 0.35f; }   // vert : curseur
        else { bx.r = 0.30f; bx.g = 0.60f; bx.b = 1.0f; }                        // bleu : poussoir, bascule
        bx.edge = 0.0016f;
        bx.faceOnly = cfg->zoneBoxFaces == "face";
        if (z.lastCapture != 0 && now - z.lastCapture < 800) { bx.r = bx.g = bx.b = 1.0f; bx.edge = 0.004f; }              // memorisee : blanc
        else if (z.grabHand >= 0) { bx.r = 0.10f; bx.g = 0.95f; bx.b = 1.0f; bx.edge = 0.0035f; }                           // saisie : cyan
        else if (z.lastFire != 0 && now - z.lastFire < 800) { bx.r = 0.15f; bx.g = 1.0f; bx.b = 0.25f; bx.edge = 0.0045f; } // declenchee : vert vif
        else if (z.inside && !z.insideInner) { bx.r = 1.0f; bx.g = 1.0f; bx.b = 0.35f; bx.edge = 0.0035f; }                // doigt dedans : jaune clair
        // Indicateur de position (comme Archan XR) : un petit slider horizontal au-dessus de la molette, avec sa poignee, tant qu'on l'actionne.
        if (k == "dial" && (z.grabHand >= 0 || now < z.hudUntil) && frame->boxCount + 8 <= kMaxBoxes) {
            const float W = std::max(0.08f, box.size.x * 1.6f);
            const float level = static_cast<float>(GetLevel(z.id, z.grabLevel));
            auto wire = [&](float ox, float oy, float sx, float sy, float sz, float r, float g, float b, float edge) {
                BoxData& w = frame->box[frame->boxCount++];
                w.pose = {box.q, box.center + em::Rotate(box.q, em::Vec3{ox, oy, box.size.z * 0.5f})};
                w.size = {sx, sy, sz};
                w.r = r; w.g = g; w.b = b;
                w.edge = edge;
                w.faceOnly = false;
            };
            const float oy = box.size.y * 0.5f + 0.016f;
            wire(0, oy, W, 0.007f, 0.004f, 0.95f, 0.95f, 0.95f, 0.0012f);                                        // gouttiere
            for (int i = 0; i <= 4; ++i) wire(-W * 0.5f + W * 0.25f * static_cast<float>(i), oy, 0.0012f, 0.016f, 0.004f, 0.8f, 0.8f, 0.8f, 0.0010f);  // graduations
            wire(-W * 0.5f + W * level, oy, 0.007f, 0.020f, 0.010f, 0.10f, 0.95f, 1.0f, 0.0030f);                // poignee : position
        }
        if (k == "slider" && z.defined2 && (z.grabHand >= 0 || now < z.hudUntil) && frame->boxCount < kMaxBoxes) {
            BoxData& h = frame->box[frame->boxCount++];
            const em::Vec3 a = g_anchor.ToWorld(z.pos), b2 = g_anchor.ToWorld(z.pos2);
            h.pose = {box.q, a + (b2 - a) * static_cast<float>(z.hudLevel)};
            h.size = {static_cast<float>(z.geom.sx / 1000.0) * 1.1f, static_cast<float>(z.geom.sy / 1000.0) * 1.1f, 0.012f};
            h.r = 0.10f; h.g = 0.95f; h.b = 1.0f; h.edge = 0.0030f; h.faceOnly = false;
        }
        if (k == "dial" && z.ctrl.dual && frame->boxCount < kMaxBoxes) {  // molette a deux niveaux : boite du bouton central, imbriquee
            const em::ZoneBox in = InnerBoxOf(z);
            BoxData& ib = frame->box[frame->boxCount++];
            ib = bx;
            ib.pose = {in.q, in.center};
            ib.size = in.size;
            if (z.grabHand >= 0 && z.grabLevel == 2) { ib.r = 0.10f; ib.g = 0.95f; ib.b = 1.0f; ib.edge = 0.0035f; }
            else if (z.insideInner) { ib.r = 1.0f; ib.g = 1.0f; ib.b = 0.35f; ib.edge = 0.0035f; }
            else if (!(z.lastFire != 0 && now - z.lastFire < 800) && !(z.lastCapture != 0 && now - z.lastCapture < 800)) { ib.r = 1.0f; ib.g = 0.85f; ib.b = 0.2f; ib.edge = 0.0016f; }
            if (z.grabHand >= 0 && z.grabLevel == 1) { bx.r = 0.10f; bx.g = 0.95f; bx.b = 1.0f; }
        }
    }
}

// --- Pilotage depuis le menu en VR ------------------------------------------------------------------------------------------------
namespace {

struct LibEntry {
    const wchar_t* label;
    const char* kind;
    const char* press;
    const char* inc;
    const char* dec;
    const char* press2;  // molette a deux niveaux : bouton central
    const char* inc2;
    const char* dec2;
};
const LibEntry kLibrary[] = {
#include "sim/CommandLibrary.inc"
};
constexpr size_t kLibraryCount = sizeof(kLibrary) / sizeof(kLibrary[0]);

std::mutex g_liveMutex;
std::vector<std::string> g_liveInputs;
std::atomic<bool> g_showAdvanced{false};  // montrer aussi les Input Events « avances » (UNKNOWN_*, entrees des manettes AZP_RAF_CONTROLS_*)

// Input Event deja couvert par une commande de la bibliotheque (meme nom dans une de ses sequences).
bool InLibrary(const std::string& name) {
    for (const LibEntry& e : kLibrary)
        for (const char* s : {e.press, e.inc, e.dec, e.press2, e.inc2, e.dec2})
            if (s[0] && std::strstr(s, name.c_str())) return true;
    return false;
}
bool IsAdvancedInput(const std::string& n) { return n.rfind("UNKNOWN_", 0) == 0 || n.rfind("AZP_RAF_CONTROLS_", 0) == 0; }

// Input Events proposes dans le menu : ceux de la bibliotheque sont deja la, les « avances » ne se montrent qu'a la demande (verrou `g_liveMutex` pris).
std::vector<size_t> VisibleLive() {
    std::vector<size_t> v;
    for (size_t i = 0; i < g_liveInputs.size(); ++i) {
        const std::string& n = g_liveInputs[i];
        if (InLibrary(n)) continue;
        if (IsAdvancedInput(n) && !g_showAdvanced) continue;
        v.push_back(i);
    }
    return v;
}
std::atomic<bool> g_liveLoaded{false}, g_liveLoading{false};

void EnsureLoaded() {
    if (g_loaded) return;
    LoadZones();
    g_loadedStamp = FileStamp();
    g_loaded = true;
}

std::wstring WideOf(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Apprentissage lance depuis le menu : on pointe la commande puis on pince (60 s), ou compte a rebours de 3 s si `teach_mode = timer`.
void StartTeach(const std::string& id) {
    g_teachId = id;
    g_teachStage = 1;
    for (const ZoneState& z : g_zone)
        if (z.id == id && z.ctrl.kind == "slider" && z.defined && !z.defined2) g_teachStage = 2;
    const ULONGLONG now = GetTickCount64();
    g_teachTick = now;
    if (CurrentConfig()->zoneTeachMode != "timer") {
        g_teachPinchUntil = now + 60000;
        g_teachBegan = now;
        g_teachAt = 0;
        Logger::Get().Log(LogLevel::Info, "zones : apprentissage de '%s' : pointez la commande du bout de l'index puis PINCEZ (60 s)%s", id.c_str(),
                          g_teachStage == 2 ? " (fin de course)" : "");
    } else {
        g_teachAt = now + 3000;
        Logger::Get().Log(LogLevel::Info, "zones : apprentissage de '%s' dans 3 s, posez le bout de l'index sur la commande%s", id.c_str(),
                          g_teachStage == 2 ? " (fin de course)" : "");
    }
    Play(Snd::Start);
}

}  // namespace

std::vector<ZoneInfo> Zones_List() {
    EnsureLoaded();
    std::vector<ZoneInfo> out;
    for (const ZoneState& z : g_zone) out.push_back({z.id, WideOf(z.id + " — " + sim::Summary(z.ctrl)), Complete(z)});
    return out;
}

size_t Zones_CommandCount() {
    std::lock_guard<std::mutex> l(g_liveMutex);
    return kLibraryCount + VisibleLive().size();
}

size_t Zones_AdvancedCount() {
    std::lock_guard<std::mutex> l(g_liveMutex);
    size_t n = 0;
    for (const std::string& s : g_liveInputs)
        if (IsAdvancedInput(s) && !InLibrary(s)) ++n;
    return n;
}

bool Zones_AdvancedShown() { return g_showAdvanced.load(); }
void Zones_ToggleAdvanced() { g_showAdvanced = !g_showAdvanced.load(); }

std::wstring Zones_CommandLabel(size_t index) {
    if (index < kLibraryCount) return kLibrary[index].label;
    std::lock_guard<std::mutex> l(g_liveMutex);
    index -= kLibraryCount;
    const std::vector<size_t> vis = VisibleLive();
    if (index >= vis.size()) return std::wstring();
    const std::string& n = g_liveInputs[vis[index]];
    return (IsAdvancedInput(n) ? L"(avancé) " : L"Input : ") + WideOf(n);
}

bool Zones_InputsLoaded() { return g_liveLoaded; }

void Zones_RefreshInputs() {
    if (g_liveLoading.exchange(true)) return;
    std::thread([] {
        const std::string r = Bridge_Call("inputs", 12000);
        std::vector<std::string> names;
        if (r.rfind("ok", 0) == 0) {
            size_t p = r.find('\t');
            while (p != std::string::npos) {
                const size_t q = r.find('\t', p + 1);
                const std::string n = r.substr(p + 1, q == std::string::npos ? std::string::npos : q - p - 1);
                if (!n.empty()) names.push_back(n);
                p = q;
            }
            std::sort(names.begin(), names.end());
        }
        Logger::Get().Log(r.rfind("ok", 0) == 0 ? LogLevel::Info : LogLevel::Warn, "zones : %zu Input Events lus dans l'avion (%s)", names.size(),
                          r.empty() ? "pont injoignable" : r.substr(0, 40).c_str());
        {
            std::lock_guard<std::mutex> l(g_liveMutex);
            g_liveInputs = std::move(names);
        }
        g_liveLoaded = true;
        g_liveLoading = false;
    }).detach();
}

void Zones_AddCommand(size_t index) {
    EnsureLoaded();
    ZoneState z;
    std::string base;
    if (index < kLibraryCount) {
        const LibEntry& e = kLibrary[index];
        z.ctrl.kind = e.kind;
        if (e.press[0]) z.ctrl.slots["press"] = sim::ParseSeq(e.press);
        if (e.inc[0]) z.ctrl.slots["inc"] = sim::ParseSeq(e.inc);
        if (e.dec[0]) z.ctrl.slots["dec"] = sim::ParseSeq(e.dec);
        if (e.press2[0]) z.ctrl.slots["press2"] = sim::ParseSeq(e.press2);
        if (e.inc2[0]) z.ctrl.slots["inc2"] = sim::ParseSeq(e.inc2);
        if (e.dec2[0]) z.ctrl.slots["dec2"] = sim::ParseSeq(e.dec2);
        z.ctrl.dual = e.inc2[0] != 0;
        for (const char* s : {"press", "inc", "dec"})
            if (z.ctrl.slots.count(s) && !z.ctrl.slots[s].empty()) { base = z.ctrl.slots[s][0].target; break; }
    } else {
        std::lock_guard<std::mutex> l(g_liveMutex);
        const std::vector<size_t> vis = VisibleLive();
        const size_t i = index - kLibraryCount;
        if (i >= vis.size()) return;
        z.ctrl.kind = "toggle";
        sim::ZoneAction a;
        a.type = "input";
        a.target = g_liveInputs[vis[i]];
        a.mode = "toggle";
        z.ctrl.slots["press"] = {a};
        base = a.target;
    }
    base = sim::SanitizeZoneId(base);
    if (base.size() > 40) base.resize(40);
    if (base.empty()) base = "zone";
    std::string id = base;
    for (int n = 2;; ++n) {
        bool used = false;
        for (const ZoneState& o : g_zone) used = used || o.id == id;
        if (!used) break;
        id = base + "_" + std::to_string(n);
    }
    z.id = id;
    DefaultGeom(z);
    g_zone.push_back(std::move(z));
    SaveZones();
    g_loadedStamp = FileStamp();
    StartTeach(id);
}

bool Zones_TakeCaptured(std::string* id) {
    if (g_capturedId.empty()) return false;
    *id = g_capturedId;
    g_capturedId.clear();
    return true;
}

void Zones_Reteach(const std::string& id) {
    EnsureLoaded();
    for (ZoneState& z : g_zone)
        if (z.id == id) {
            if (z.ctrl.kind == "slider") z.defined = z.defined2 = false;  // un curseur reapprend ses deux points
            StartTeach(id);
            return;
        }
}

// --- Boites : taille et forme depuis le menu en VR ------------------------------------------------------------------------------------

namespace {
struct Shape {
    const wchar_t* name;
    double x, y, z;
};
const Shape kShapes[] = {{L"petit bouton", 16, 16, 30}, {L"bouton", 30, 30, 40}, {L"interrupteur", 30, 40, 40}, {L"molette", 50, 50, 50},
                         {L"grand panneau", 90, 60, 50}, {L"ecran", 160, 110, 60}};
ZoneState* FindZone(const std::string& id) {
    EnsureLoaded();
    for (ZoneState& z : g_zone)
        if (z.id == id) return &z;
    return nullptr;
}
void SaveNow() {
    SaveZones();
    g_loadedStamp = FileStamp();
}
}  // namespace

std::wstring Zones_GeomText(const std::string& id) {
    const ZoneState* z = FindZone(id);
    if (!z) return {};
    wchar_t b[96];
    std::swprintf(b, 96, L"%.0f x %.0f x %.0f mm", z->geom.sx, z->geom.sy, z->geom.sz);
    return b;
}

bool Zones_Scale(const std::string& id, double factor) {
    ZoneState* z = FindZone(id);
    if (!z) return false;
    auto clamp = [](double v) { return std::max(8.0, std::min(300.0, v)); };
    z->geom.sx = clamp(z->geom.sx * factor);
    z->geom.sy = clamp(z->geom.sy * factor);
    z->geom.sz = clamp(z->geom.sz * factor);
    for (auto& t : z->tracker) t.Disarm();
    SaveNow();
    return true;
}

bool Zones_NextShape(const std::string& id) {
    ZoneState* z = FindZone(id);
    if (!z) return false;
    constexpr int n = sizeof(kShapes) / sizeof(kShapes[0]);
    int best = 0;
    double bestD = 1e18;
    for (int i = 0; i < n; ++i) {
        const double d = std::fabs(kShapes[i].x - z->geom.sx) + std::fabs(kShapes[i].y - z->geom.sy) + std::fabs(kShapes[i].z - z->geom.sz);
        if (d < bestD) { bestD = d; best = i; }
    }
    const Shape& s = kShapes[(best + 1) % n];
    z->geom.sx = s.x;
    z->geom.sy = s.y;
    z->geom.sz = s.z;
    for (auto& t : z->tracker) t.Disarm();
    SaveNow();
    return true;
}

// Boite a plat (face vers le haut : banquettes, consoles) ou verticale (face vers le pilote).
bool Zones_ToggleFlat(const std::string& id) {
    ZoneState* z = FindZone(id);
    if (!z) return false;
    z->geom.pitch = (z->geom.pitch == 0.0) ? -90.0 : 0.0;
    for (auto& t : z->tracker) t.Disarm();
    SaveNow();
    return true;
}

bool Zones_IsFlat(const std::string& id) {
    const ZoneState* z = FindZone(id);
    return z && std::fabs(z->geom.pitch + 90.0) < 1.0;
}

bool Zones_Delete(const std::string& id) {
    EnsureLoaded();
    const size_t before = g_zone.size();
    g_zone.erase(std::remove_if(g_zone.begin(), g_zone.end(), [&](const ZoneState& z) { return z.id == id; }), g_zone.end());
    if (g_zone.size() == before) return false;
    SaveNow();
    return true;
}

}  // namespace layer
