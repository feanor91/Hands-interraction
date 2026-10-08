// emulate.cpp — étape 1 : le jeu voit un contrôleur placé au bout de l'index.
//
// Ce que fait ce fichier, pour un développeur C# :
//  * Il garde deux « hand trackers » (gauche, droite) ouverts pendant la session.
//  * Quand le jeu demande la pose d'une action aim/grip (xrLocateSpace), on appelle d'abord le
//    VRAI runtime, puis, si la main est suivie, on remplace la pose par celle du bout de l'index.
//  * Cette pose est exprimée RELATIVEMENT À LA PAUME (rigide, insensible au pincement), lissée
//    (One Euro), et FIGÉE à sa valeur d'avant le pincement tant que l'on pince : sinon le bout
//    de l'index descend vers le pouce et le rayon quitte la cible avant le clic.
//  * Quand le jeu lit la gâchette (xrGetActionStateBoolean/Float), on répond « appuyé » si le
//    pouce et l'index se touchent (avec hystérésis, voir core/emulation/Pinch.h).
//  * Si quoi que ce soit manque (main perdue, fonction absente, exception) : rien n'est modifié.
#include <windows.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "emulate.h"
#include "hands_overlay.h"
#include "emulation/Pinch.h"
#include "emulation/PoseMath.h"

namespace layer {

using hands::log::Logger;
using hands::log::LogLevel;
namespace em = hands::emulation;

namespace {

template <class F>
void Safe(const char* what, F&& f) noexcept {
    try {
        f();
    } catch (const std::exception& e) {
        try { Logger::Get().Log(LogLevel::Error, "exception dans l'emulation (%s) : %s", what, e.what()); } catch (...) {}
    } catch (...) {
        try { Logger::Get().Log(LogLevel::Error, "exception inconnue dans l'emulation (%s)", what); } catch (...) {}
    }
}

template <class H>
uint64_t Key(H h) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h)); }

enum class ActionKind { None, AimPose, GripPose, TriggerBool, TriggerFloat };

// Articulation que le pouce doit toucher pour cliquer (indices OpenXR : 8 = index intermédiaire,
// 10 = bout de l'index, 15 = bout du majeur).
int ClickJointFor(const std::string& mode) {
    if (mode == "pinch") return XR_HAND_JOINT_INDEX_TIP_EXT;
    if (mode == "middle") return XR_HAND_JOINT_MIDDLE_TIP_EXT;
    return XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT;  // "side"
}

// --- Conversions OpenXR <-> maths portables ---------------------------------------------------
em::Pose ToPose(const XrPosef& p) {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}};
}
XrPosef ToXr(const em::Pose& p) {
    XrPosef x;
    x.orientation = {p.q.x, p.q.y, p.q.z, p.q.w};
    x.position = {p.p.x, p.p.y, p.p.z};
    return x;
}

struct RelSample {
    XrTime t = 0;
    em::Pose rel;  // pointe de l'index exprimée dans le repère de la paume (déjà lissée)
};

constexpr int kHistory = 64;  // ~700 ms à 90 Hz

struct HandState {
    XrHandTrackerEXT tracker = XR_NULL_HANDLE;

    // Pointeur relatif à la paume
    em::OneEuro rotFilter, posFilter;
    em::Pose relOut;       // dernière valeur lissée
    XrTime relTime = -1;   // instant de la dernière mise à jour du filtre
    RelSample history[kHistory];
    int histCount = 0, histHead = 0;  // anneau : le plus récent est à (histHead - 1)
    bool latched = false;
    em::Pose latchedRel;

    // Pincement
    em::PinchDetector pinch;
    bool pressed = false;

    // Cache de la dernière localisation (par espace de base et instant)
    XrSpace cacheBase = XR_NULL_HANDLE;
    XrTime cacheTime = -1;
    bool cacheValid = false;
    em::Pose outTip;  // pose finale du pointeur dans l'espace de base mis en cache

    // Suivi / diagnostic
    ULONGLONG lastValidTick = 0;
    XrTime lastValidTime = 0;
    bool wasValid = false;
    int logsLeft = 3;      // premières poses substituées
    int flagLogsLeft = 3;  // premiers indicateurs de suivi bruts
    int lossLogsLeft = 40;
};

std::mutex g_mutex;  // protège tout ce qui suit ; sections courtes
XrSession g_session = XR_NULL_HANDLE;
HandState g_hand[2];
XrPath g_handPath[2] = {XR_NULL_PATH, XR_NULL_PATH};
bool g_started = false;
std::atomic<bool> g_live{false};  // lecture rapide hors verrou

struct SpaceInfo { ActionKind kind; XrPath subaction; };
std::unordered_map<uint64_t, ActionKind> g_actionKinds;  // action -> sorte
std::unordered_map<uint64_t, SpaceInfo> g_spaces;        // espace d'action -> infos
std::unordered_map<uint64_t, uint8_t> g_reported;        // (action,main) -> dernière valeur envoyée

constexpr ULONGLONG kFreshMs = 300;  // au-delà, la main est considérée perdue : pass-through

ActionKind Classify(const std::string& fullName) {
    const size_t slash = fullName.find('/');
    const std::string n = slash == std::string::npos ? fullName : fullName.substr(slash + 1);
    if (n == "aim_pose") return ActionKind::AimPose;
    if (n == "grip_pose") return ActionKind::GripPose;
    if (n == "trigger_click" || n == "select_click") return ActionKind::TriggerBool;
    if (n == "trigger_value") return ActionKind::TriggerFloat;
    return ActionKind::None;
}

int HandIndex(XrPath p) {
    if (p == XR_NULL_PATH) return -1;
    if (p == g_handPath[0]) return 0;
    if (p == g_handPath[1]) return 1;
    return -1;
}

constexpr XrSpaceLocationFlags kPoseValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
constexpr XrSpaceLocationFlags kPoseTracked = XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;

bool JointUsable(const XrHandJointLocationEXT& j, bool needOrientation, bool requireTracked) {
    XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (needOrientation) need |= XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (requireTracked) need |= XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    return (j.locationFlags & need) == need;
}

void PushHistory(HandState& hs, XrTime t, const em::Pose& rel) {
    hs.history[hs.histHead] = {t, rel};
    hs.histHead = (hs.histHead + 1) % kHistory;
    if (hs.histCount < kHistory) ++hs.histCount;
}

// Valeur relative la plus récente dont l'instant est <= `target` (sinon la plus ancienne connue).
em::Pose HistoryAt(const HandState& hs, XrTime target) {
    if (hs.histCount == 0) return hs.relOut;
    em::Pose best = hs.history[(hs.histHead - hs.histCount + kHistory) % kHistory].rel;  // la plus ancienne
    for (int i = 0; i < hs.histCount; ++i) {  // du plus ancien au plus récent
        const RelSample& s = hs.history[(hs.histHead - hs.histCount + i + kHistory) % kHistory];
        if (s.t <= target) best = s.rel;
        else break;
    }
    return best;
}

void ResetPointer(HandState& hs) {
    hs.rotFilter.Reset();
    hs.posFilter.Reset();
    hs.relTime = -1;
    hs.histCount = hs.histHead = 0;
    hs.latched = false;
    hs.pressed = false;
    hs.pinch.Reset();
}

// Localise les articulations de la main `h` dans `base` à l'instant `time` (verrou déjà pris).
// Met à jour pointeur relatif, pincement, verrouillage. Retourne vrai si la main est exploitable.
bool Refresh(int h, XrSpace base, XrTime time) {
    HandState& hs = g_hand[h];
    if (!hs.tracker || !g_dispatch.LocateHandJointsEXT) return false;
    if (hs.cacheBase == base && hs.cacheTime == time) return hs.cacheValid;

    const auto cfg = CurrentConfig();
    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT] = {};
    XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
    info.baseSpace = base;
    info.time = time;
    XrHandJointLocationsEXT loc{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
    loc.jointCount = XR_HAND_JOINT_COUNT_EXT;
    loc.jointLocations = joints;
    const XrResult r = g_dispatch.LocateHandJointsEXT(hs.tracker, &info, &loc);

    const XrHandJointLocationEXT& palm = joints[XR_HAND_JOINT_PALM_EXT];
    const XrHandJointLocationEXT& idx = joints[XR_HAND_JOINT_INDEX_TIP_EXT];
    const XrHandJointLocationEXT& thumb = joints[XR_HAND_JOINT_THUMB_TIP_EXT];

    if (hs.flagLogsLeft > 0 && XR_SUCCEEDED(r)) {
        --hs.flagLogsLeft;
        Logger::Get().Log(LogLevel::Info,
                          "emulation : main %s indicateurs bruts : isActive=%d paume=0x%llX index=0x%llX pouce=0x%llX",
                          h == 0 ? "gauche" : "droite", loc.isActive ? 1 : 0,
                          static_cast<unsigned long long>(palm.locationFlags), static_cast<unsigned long long>(idx.locationFlags),
                          static_cast<unsigned long long>(thumb.locationFlags));
    }

    const bool handMode = cfg->pointerMode == "hand";
    const bool palmMode = cfg->pointerMode == "palm";
    // Doigt qui porte le pointeur : l'index (défaut) ou le majeur, qui bouge moins quand l'index sert au clic.
    const bool middleFinger = cfg->pointerFinger == "middle";
    const XrHandJointLocationEXT& ptrTip = joints[middleFinger ? XR_HAND_JOINT_MIDDLE_TIP_EXT : XR_HAND_JOINT_INDEX_TIP_EXT];
    const XrHandJointLocationEXT& knuckle = joints[middleFinger ? XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT : XR_HAND_JOINT_INDEX_PROXIMAL_EXT];
    // Mode "palm" : articulation du doigt vers laquelle on vise depuis la paume (proximal, intermédiaire, distal, bout).
    const int aimOffset = cfg->palmAim == "proximal" ? 0 : (cfg->palmAim == "distal" ? 2 : (cfg->palmAim == "tip" ? 3 : 1));
    const XrHandJointLocationEXT& aimJoint = joints[(middleFinger ? XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT : XR_HAND_JOINT_INDEX_PROXIMAL_EXT) + aimOffset];
    // Mode "palm" : le rayon part de la paume et vise le doigt choisi dans sa pose naturelle (direction lissée lentement) ;
    // mode "hand" : rigide à la paume (axe de la main, origine à la base du doigt) ; mode "finger" : suit le bout du doigt.
    // Le geste de clic est indépendant (click_mode).
    const bool usable = XR_SUCCEEDED(r) && loc.isActive && JointUsable(palm, true, cfg->requireTracked) &&
                        JointUsable(ptrTip, true, cfg->requireTracked) && (!handMode || JointUsable(knuckle, false, cfg->requireTracked)) &&
                        (!palmMode || JointUsable(aimJoint, false, cfg->requireTracked));
    if (usable != hs.wasValid && hs.lossLogsLeft > 0) {
        --hs.lossLogsLeft;
        Logger::Get().Log(LogLevel::Info, "emulation : main %s : suivi %s", h == 0 ? "gauche" : "droite",
                          usable ? "retrouve" : "PERDU (pass-through)");
    }
    hs.wasValid = usable;
    hs.cacheBase = base;
    hs.cacheTime = time;
    hs.cacheValid = usable;
    if (!usable) return false;

    const em::Pose palmPose = ToPose(palm.pose);
    em::Pose pointerWorld = ToPose(ptrTip.pose);
    if (handMode) pointerWorld = {palmPose.q, ToPose(knuckle.pose).p};
    if (palmMode) {
        // Origine = centre de la paume ; avant = de la paume vers le doigt visé ; haut = haut de la paume (sans effet sur le rayon).
        const em::Vec3 from = palmPose.p, to = ToPose(aimJoint.pose).p;
        pointerWorld = {em::LookAlong(to - from, em::Rotate(palmPose.q, em::Vec3{0.0f, 1.0f, 0.0f})), from};
    }
    em::Pose rawRel = em::Multiply(em::Inverse(palmPose), pointerWorld);
    if (cfg->pointerPitchDeg != 0.0 || cfg->pointerYawDeg != 0.0) {  // réglage fin de la direction (repère de la paume)
        const float kDeg = 3.14159265f / 180.0f;
        const float hp = static_cast<float>(cfg->pointerPitchDeg) * kDeg * 0.5f, hy = static_cast<float>(cfg->pointerYawDeg) * kDeg * 0.5f;
        const em::Quat qx{std::sin(hp), 0, 0, std::cos(hp)}, qy{0, std::sin(hy), 0, std::cos(hy)};
        rawRel.q = em::Normalize(em::Multiply(rawRel.q, em::Multiply(qy, qx)));
    }

    if (time > hs.relTime) {  // nouvel instant : on fait avancer le filtre, le pincement et le verrouillage
        float dt = hs.relTime < 0 ? 0.0f : static_cast<float>(time - hs.relTime) * 1e-9f;
        if (dt > 0.5f) {  // trou long (main perdue) : on repart de zéro
            ResetPointer(hs);
            dt = 0.0f;
        }
        if (cfg->smoothing) {
            // Mode palm : suivi lent de la direction (le doigt qui bouge pour cliquer ne déplace pas le rayon).
            const float rotHz = palmMode ? static_cast<float>(cfg->palmFollowHz) : static_cast<float>(cfg->smoothRotHz);
            const float rotBeta = static_cast<float>(cfg->smoothRotBeta) * (palmMode ? 0.4f : 1.0f);
            hs.relOut.q = hs.rotFilter.Filter(rawRel.q, dt, rotHz, rotBeta);
            hs.relOut.p = hs.posFilter.Filter(rawRel.p, dt, 6.0f, 30.0f);
        } else {
            hs.relOut = rawRel;
        }
        hs.relTime = time;
        PushHistory(hs, time, hs.relOut);

        const XrHandJointLocationEXT& target = joints[ClickJointFor(cfg->clickMode)];
        if (JointUsable(thumb, false, cfg->clickRequireTracked) && JointUsable(target, false, cfg->clickRequireTracked)) {
            const bool was = hs.pressed;
            const float dist = em::Length(ToPose(target.pose).p - ToPose(thumb.pose).p);
            hs.pressed = hs.pinch.Update(dist, static_cast<float>(cfg->clickOnMm / 1000.0), static_cast<float>(cfg->clickOffMm / 1000.0));
            if (hs.pressed && !was) {
                hs.latched = true;
                hs.latchedRel = HistoryAt(hs, time - static_cast<XrTime>(cfg->latchMs * 1e6));
            } else if (!hs.pressed && was) {
                hs.latched = false;
            }
            if (hs.pressed != was)
                Logger::Get().Log(LogLevel::Info, "emulation : main %s -> %s (distance %.1f mm)", h == 0 ? "gauche" : "droite",
                                  hs.pressed ? "APPUI (pincement)" : "relache", dist * 1000.0f);
        }
    }

    hs.outTip = em::Multiply(palmPose, hs.latched ? hs.latchedRel : hs.relOut);
    hs.lastValidTick = GetTickCount64();
    hs.lastValidTime = time;
    return true;
}

bool Fresh(int h) {
    return g_hand[h].tracker && g_hand[h].lastValidTick != 0 && GetTickCount64() - g_hand[h].lastValidTick < kFreshMs;
}

// --- Hooks ---------------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location) {
    const XrResult r = g_dispatch.LocateSpace(space, baseSpace, time, location);
    if (!g_live.load(std::memory_order_acquire) || XR_FAILED(r) || !location) return r;
    Safe("xrLocateSpace", [&] {
        const auto cfg = CurrentConfig();
        if (!cfg->emulationEnabled) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_spaces.find(Key(space));
        if (it == g_spaces.end()) return;
        const int h = HandIndex(it->second.subaction);
        if (h < 0 || !Refresh(h, baseSpace, time)) return;  // main non suivie : pose du runtime conservée
        HandState& hs = g_hand[h];
        if (hs.logsLeft > 0) {
            --hs.logsLeft;
            Logger::Get().Log(LogLevel::Info,
                              "emulation : %s main %s : pose runtime pos=(%.3f %.3f %.3f) -> pointeur pos=(%.3f %.3f %.3f)",
                              it->second.kind == ActionKind::AimPose ? "aim" : "grip", h == 0 ? "gauche" : "droite",
                              location->pose.position.x, location->pose.position.y, location->pose.position.z,
                              hs.outTip.p.x, hs.outTip.p.y, hs.outTip.p.z);
        }
        em::Pose finalPose = hs.outTip;
        // Masquer le modèle de manette : le jeu le dessine à la pose « grip » ; on l'envoie très loin sous le cockpit
        // (100 m), pose toujours valide et active pour ne pas troubler le jeu. Le rayon (aim) reste sur le doigt.
        if (it->second.kind == ActionKind::GripPose && cfg->hideControllerModel) finalPose.p.y -= 100.0f;
        location->pose = ToXr(finalPose);
        location->locationFlags |= kPoseValid | kPoseTracked;
        // Les vitesses du runtime ne correspondent plus à la pose substituée : on les invalide.
        for (auto* n = static_cast<XrBaseOutStructure*>(location->next); n; n = n->next)
            if (n->type == XR_TYPE_SPACE_VELOCITY) reinterpret_cast<XrSpaceVelocity*>(n)->velocityFlags = 0;
    });
    return r;
}

// Main(s) concernée(s) par une lecture d'action : la sous-action, ou les deux si elle est absente.
// Retourne l'état « appuyé » combiné ; `any` indique qu'au moins une main est suivie.
bool PressedFor(XrPath subaction, bool* any, int* firstHand) {
    bool pressed = false;
    *any = false;
    *firstHand = -1;
    for (int h = 0; h < 2; ++h) {
        if (subaction != XR_NULL_PATH && HandIndex(subaction) != h) continue;
        if (!Fresh(h)) {
            if (g_hand[h].pressed || g_hand[h].latched) ResetPointer(g_hand[h]);  // pas d'appui fantôme au retour
            continue;
        }
        *any = true;
        if (*firstHand < 0) *firstHand = h;
        pressed = pressed || g_hand[h].pressed;
    }
    return pressed;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo,
                                                            XrActionStateBoolean* state) {
    const XrResult r = g_dispatch.GetActionStateBoolean(session, getInfo, state);
    if (!g_live.load(std::memory_order_acquire) || XR_FAILED(r) || !getInfo || !state) return r;
    Safe("xrGetActionStateBoolean", [&] {
        if (!CurrentConfig()->emulationEnabled) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_actionKinds.find(Key(getInfo->action));
        if (it == g_actionKinds.end() || it->second != ActionKind::TriggerBool) return;
        bool any = false;
        int h = -1;
        const bool pressed = PressedFor(getInfo->subactionPath, &any, &h);
        if (!any) return;  // aucune main suivie : état du runtime conservé
        uint8_t& last = g_reported[Key(getInfo->action) * 4 + static_cast<uint64_t>(h + 1)];
        state->changedSinceLastSync = (last != static_cast<uint8_t>(pressed)) ? XR_TRUE : XR_FALSE;
        last = pressed ? 1 : 0;
        state->currentState = pressed ? XR_TRUE : XR_FALSE;
        state->isActive = XR_TRUE;
        if (state->changedSinceLastSync && h >= 0) state->lastChangeTime = g_hand[h].lastValidTime;
    });
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* getInfo,
                                                          XrActionStateFloat* state) {
    const XrResult r = g_dispatch.GetActionStateFloat(session, getInfo, state);
    if (!g_live.load(std::memory_order_acquire) || XR_FAILED(r) || !getInfo || !state) return r;
    Safe("xrGetActionStateFloat", [&] {
        if (!CurrentConfig()->emulationEnabled) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_actionKinds.find(Key(getInfo->action));
        if (it == g_actionKinds.end() || it->second != ActionKind::TriggerFloat) return;
        bool any = false;
        int h = -1;
        const bool pressed = PressedFor(getInfo->subactionPath, &any, &h);
        if (!any) return;
        uint8_t& last = g_reported[Key(getInfo->action) * 4 + static_cast<uint64_t>(h + 1)];
        state->changedSinceLastSync = (last != static_cast<uint8_t>(pressed)) ? XR_TRUE : XR_FALSE;
        last = pressed ? 1 : 0;
        state->currentState = pressed ? 1.0f : 0.0f;
        state->isActive = XR_TRUE;
        if (state->changedSinceLastSync && h >= 0) state->lastChangeTime = g_hand[h].lastValidTime;
    });
    return r;
}

// Beaucoup de jeux testent `isActive` avant d'utiliser la pose : on l'affirme quand la main est suivie.
XRAPI_ATTR XrResult XRAPI_CALL Hook_xrGetActionStatePose(XrSession session, const XrActionStateGetInfo* getInfo,
                                                         XrActionStatePose* state) {
    const XrResult r = g_dispatch.GetActionStatePose(session, getInfo, state);
    if (!g_live.load(std::memory_order_acquire) || XR_FAILED(r) || !getInfo || !state) return r;
    Safe("xrGetActionStatePose", [&] {
        const auto cfg = CurrentConfig();
        if (!cfg->emulationEnabled) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_actionKinds.find(Key(getInfo->action));
        if (it == g_actionKinds.end()) return;
        const XrPath sub = getInfo->subactionPath;
        for (int h = 0; h < 2; ++h) {
            if (sub != XR_NULL_PATH && HandIndex(sub) != h) continue;
            if (Fresh(h)) { state->isActive = XR_TRUE; return; }
        }
    });
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrDestroySession(XrSession session) {
    Safe("xrDestroySession", [&] { Emu_Stop(session); });
    return g_dispatch.DestroySession(session);
}

struct HookEntry {
    const char* name;
    PFN_xrVoidFunction fn;
};
#define HOOK(n) {#n, reinterpret_cast<PFN_xrVoidFunction>(Hook_##n)}
const HookEntry kHooks[] = {
    HOOK(xrLocateSpace),
    HOOK(xrGetActionStateBoolean),
    HOOK(xrGetActionStateFloat),
    HOOK(xrGetActionStatePose),
    HOOK(xrDestroySession),
};
#undef HOOK

}  // namespace

// --- API publique ----------------------------------------------------------------------------

bool Emu_Start(XrInstance instance, XrSession session) {
    Logger& log = Logger::Get();
    // Les hand trackers servent à l'émulation (étape 1) ET au dessin des mains (étape 3a).
    if (!CurrentConfig()->emulationEnabled && !CurrentConfig()->handsEnabled) {
        log.Log(LogLevel::Info, "emulation et mains : desactivees dans hands.ini ([emulation] et [hands] enabled = false)");
        return false;
    }
    const Dispatch& d = g_dispatch;
    if (!d.CreateHandTrackerEXT || !d.DestroyHandTrackerEXT || !d.LocateHandJointsEXT || !d.LocateSpace || !d.DestroySession ||
        !d.GetActionStateBoolean || !d.GetActionStateFloat || !d.GetActionStatePose) {
        log.Log(LogLevel::Warn, "emulation : fonctions OpenXR manquantes dans le runtime -> desactivee");
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_started) return false;  // une seule session suivie
    if (XR_FAILED(d.StringToPath(instance, "/user/hand/left", &g_handPath[0])) ||
        XR_FAILED(d.StringToPath(instance, "/user/hand/right", &g_handPath[1]))) {
        log.Log(LogLevel::Warn, "emulation : chemins /user/hand/* introuvables -> desactivee");
        return false;
    }
    const XrHandEXT ids[2] = {XR_HAND_LEFT_EXT, XR_HAND_RIGHT_EXT};
    int ok = 0;
    for (int h = 0; h < 2; ++h) {
        XrHandTrackerCreateInfoEXT ci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        ci.hand = ids[h];
        ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        const XrResult r = d.CreateHandTrackerEXT(session, &ci, &g_hand[h].tracker);
        if (XR_FAILED(r)) g_hand[h].tracker = XR_NULL_HANDLE;
        else ++ok;
        log.Log(LogLevel::Info, "emulation : hand tracker %s -> %s", h == 0 ? "gauche" : "droite", XR_SUCCEEDED(r) ? "ok" : "ECHEC");
    }
    if (ok == 0) return false;
    g_session = session;
    g_started = true;
    g_live.store(true, std::memory_order_release);
    const auto cfg = CurrentConfig();
    if (cfg->emulationEnabled)
        log.Log(LogLevel::Info,
                "emulation : ACTIVE (pointeur : %s sur le %s ; clic : %s %.0f/%.0f mm ; verrouillage %.0f ms ; lissage %s ; suivi exige : %s ; "
                "manettes du jeu %s)",
                cfg->pointerMode.c_str(), cfg->pointerFinger == "middle" ? "majeur" : "doigt index", cfg->clickMode.c_str(), cfg->clickOnMm, cfg->clickOffMm, cfg->latchMs,
                cfg->smoothing ? "oui" : "non", cfg->requireTracked ? "oui" : "non", cfg->hideControllerModel ? "masquees" : "visibles");
    else
        log.Log(LogLevel::Info, "emulation : desactivee (hand trackers ouverts pour le dessin des mains)");
    return true;
}

bool Emu_LocateForRender(int hand, XrSpace base, XrTime time, XrHandJointLocationEXT* out, bool* pressed, int* clickJoint) {
    if (!g_live.load(std::memory_order_acquire) || hand < 0 || hand > 1 || !out) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    HandState& hs = g_hand[hand];
    if (!hs.tracker || !g_dispatch.LocateHandJointsEXT) return false;
    XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
    info.baseSpace = base;
    info.time = time;
    XrHandJointLocationsEXT loc{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
    loc.jointCount = XR_HAND_JOINT_COUNT_EXT;
    loc.jointLocations = out;
    if (XR_FAILED(g_dispatch.LocateHandJointsEXT(hs.tracker, &info, &loc)) || !loc.isActive) return false;
    if (pressed) *pressed = hs.pressed && Fresh(hand);
    if (clickJoint) *clickJoint = ClickJointFor(CurrentConfig()->clickMode);
    return true;
}

void Emu_Stop(XrSession session) {
    Overlay_Stop();  // les swapchains de la surimpression dépendent de la session : on les détruit d'abord
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_started || (session != XR_NULL_HANDLE && session != g_session)) return;
    g_live.store(false, std::memory_order_release);
    for (int h = 0; h < 2; ++h) {
        if (g_hand[h].tracker && g_dispatch.DestroyHandTrackerEXT) g_dispatch.DestroyHandTrackerEXT(g_hand[h].tracker);
        g_hand[h] = HandState{};
    }
    g_spaces.clear();
    g_reported.clear();
    g_session = XR_NULL_HANDLE;
    g_started = false;
    Logger::Get().Log(LogLevel::Info, "emulation : arretee (hand trackers detruits)");
}

void Emu_OnAction(XrAction action, const std::string& fullName) {
    const ActionKind k = Classify(fullName);
    if (k == ActionKind::None) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_actionKinds[Key(action)] = k;
}

void Emu_OnActionSpace(XrAction action, XrPath subactionPath, XrSpace space) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_actionKinds.find(Key(action));
    if (it == g_actionKinds.end()) return;
    if (it->second == ActionKind::AimPose || it->second == ActionKind::GripPose)
        g_spaces[Key(space)] = SpaceInfo{it->second, subactionPath};
}

PFN_xrVoidFunction FindEmulationHook(const char* name) {
    // Le jeu demande ses fonctions AVANT la creation de session : on fournit donc les hooks des que
    // les vraies fonctions existent ; chaque hook ne fait rien tant que l'emulation n'est pas demarree.
    const Dispatch& d = g_dispatch;
    if (!d.LocateSpace || !d.GetActionStateBoolean || !d.GetActionStateFloat || !d.GetActionStatePose || !d.DestroySession)
        return nullptr;
    for (const HookEntry& h : kHooks)
        if (std::strcmp(h.name, name) == 0) return h.fn;
    return nullptr;
}

}  // namespace layer
