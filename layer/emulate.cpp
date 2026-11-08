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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "emulate.h"
#include "hands_overlay.h"
#include "emulation/HandPresence.h"
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

// Comme ClickJointFor ; en mode "any" (n'importe quel contact du pouce avec l'index ou le majeur), l'articulation la plus proche du pouce.
int ClickJointNearest(const std::string& mode, const XrHandJointLocationEXT* j) {
    if (mode != "any") return ClickJointFor(mode);
    const float tx = j[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position.x, ty = j[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position.y, tz = j[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position.z;
    int best = XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT;
    float bestD = 1e9f;
    for (int k : {static_cast<int>(XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT), static_cast<int>(XR_HAND_JOINT_INDEX_TIP_EXT), static_cast<int>(XR_HAND_JOINT_MIDDLE_TIP_EXT)}) {
        const XrVector3f& q = j[k].pose.position;
        const float d = (q.x - tx) * (q.x - tx) + (q.y - ty) * (q.y - ty) + (q.z - tz) * (q.z - tz);
        if (d < bestD) { bestD = d; best = k; }
    }
    return best;
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
    em::OneEuro rotFilter, posFilter, worldRotFilter, worldPosFilter;
    em::Pose relOut;       // dernière valeur lissée
    XrTime relTime = -1;   // instant de la dernière mise à jour du filtre
    RelSample history[kHistory];
    int histCount = 0, histHead = 0;  // anneau : le plus récent est à (histHead - 1)
    bool latched = false;
    em::Pose latchedRel;
    bool preArmed = true;
    bool prelatched = false;  // pointeur fige des que le pouce s approche de la cible (avant meme le clic)
    ULONGLONG prelatchTick = 0;
    bool stickyOn = false;
    em::Pose sticky;  // pose figee du pointeur (zone morte)

    // Pincement
    em::PinchDetector pinch;
    float clickDist = 1.0f;  // distance pouce - cible du clic (metres) ; 1 m = inutilisable. Sert a choisir la main engagee.
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
XrSpace g_viewSpace = XR_NULL_HANDLE;  // espace VIEW (la tete), utilise par le mode "shoulder"
HandState g_hand[2];
XrPath g_handPath[2] = {XR_NULL_PATH, XR_NULL_PATH};
bool g_started = false;
std::atomic<bool> g_live{false};  // lecture rapide hors verrou

struct SpaceInfo { ActionKind kind; XrPath subaction; };
std::unordered_map<uint64_t, ActionKind> g_actionKinds;  // action -> sorte
std::unordered_map<uint64_t, SpaceInfo> g_spaces;        // espace d'action -> infos
std::unordered_map<uint64_t, uint8_t> g_reported;        // (action,main) -> dernière valeur envoyée

// Mode « mains seules » : manettes virtuelles issues des mains masquées tant que la main est suivie.
em::HandPresence g_presence[2];                    // suivi filtré (maintien), protégé par g_mutex
std::atomic<bool> g_hideHand[2] = {false, false};  // lu sans verrou par les hooks
std::unordered_set<uint64_t> g_handBound;          // actions liées à un chemin /user/hand/... (protégé par g_mutex)
bool g_profileEventPending = false;                // évènement de changement de profil à synthétiser (protégé par g_mutex)
bool g_lastProfileFake = false;                    // valeur de hide_controllers_profile à la dernière mise à jour

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

// Doit-on masquer l'entrée de cette action pour cette sous-action ? (g_mutex déjà pris)
bool ShouldMask(XrAction action, XrPath subaction) {
    if (g_handBound.find(Key(action)) == g_handBound.end()) return false;
    if (subaction == XR_NULL_PATH) return g_hideHand[0].load() || g_hideHand[1].load();
    const int h = HandIndex(subaction);
    return h >= 0 && g_hideHand[h].load();
}

constexpr XrSpaceLocationFlags kPoseValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
constexpr XrSpaceLocationFlags kPoseTracked = XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;

bool JointUsable(const XrHandJointLocationEXT& j, bool needOrientation, bool requireTracked) {
    XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (needOrientation) need |= XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (requireTracked) need |= XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    return (j.locationFlags & need) == need;
}

// Pose de la tete (espace VIEW) dans `base` a l'instant `time`, mise en cache par (base, instant). Faux si indisponible.
// Torse estime : son lacet suit celui de la tete avec une constante de temps (torso_follow_s), pour que les epaules ne tournent pas
// avec la tete quand on regarde un bouton sur le cote.
float g_torsoYaw = 0.0f;
bool g_torsoInit = false;
XrTime g_torsoTime = -1;

void UpdateTorso(const em::Pose& head, XrTime time) {
    const float yawHead = em::HeadYaw(head.q, g_torsoYaw);
    const float tau = static_cast<float>(CurrentConfig()->torsoFollowS);
    if (!g_torsoInit || tau < 0.05f) {
        g_torsoYaw = yawHead;
        g_torsoInit = true;
        g_torsoTime = time;
        return;
    }
    float dt = static_cast<float>(time - g_torsoTime) * 1e-9f;
    if (dt <= 0.0f) return;  // instant deja traite (ou en arriere)
    if (dt > 1.0f) dt = 1.0f;
    g_torsoTime = time;
    g_torsoYaw = em::WrapAngle(g_torsoYaw + em::WrapAngle(yawHead - g_torsoYaw) * (1.0f - std::exp(-dt / tau)));
}

bool HeadPose(XrSpace base, XrTime time, em::Pose* out) {
    static XrSpace cBase = XR_NULL_HANDLE;
    static XrTime cTime = -1;
    static bool cOk = false;
    static em::Pose cPose;
    if (g_viewSpace == XR_NULL_HANDLE || !g_dispatch.LocateSpace) return false;
    if (cBase != base || cTime != time) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        cOk = XR_SUCCEEDED(g_dispatch.LocateSpace(g_viewSpace, base, time, &loc)) && (loc.locationFlags & kPoseValid) == kPoseValid;
        cBase = base;
        cTime = time;
        if (cOk) {
            cPose = ToPose(loc.pose);
            UpdateTorso(cPose, time);
        }
    }
    *out = cPose;
    return cOk;
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
    hs.worldRotFilter.Reset();
    hs.worldPosFilter.Reset();
    hs.relTime = -1;
    hs.histCount = hs.histHead = 0;
    hs.latched = false;
    hs.prelatched = false;
    hs.stickyOn = false;
    hs.pressed = false;
    hs.clickDist = 1.0f;
    hs.pinch.Reset();
}

bool Fresh(int h);
void UpdateEngaged(const hands::config::Config& cfg);  // defini plus bas

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
    const bool contactMode = cfg->pointerMode == "contact";
    const bool shoulderMode = cfg->pointerMode == "shoulder";
    const XrHandJointLocationEXT& wrist = joints[XR_HAND_JOINT_WRIST_EXT];
    const XrHandJointLocationEXT& clickTarget = joints[ClickJointNearest(cfg->clickMode, joints)];  // ce que le pouce touche pour cliquer
    // Doigt qui porte le pointeur : l'index (défaut) ou le majeur, qui bouge moins quand l'index sert au clic.
    const bool middleFinger = cfg->pointerFinger == "middle";
    const XrHandJointLocationEXT& ptrTip = joints[middleFinger ? XR_HAND_JOINT_MIDDLE_TIP_EXT : XR_HAND_JOINT_INDEX_TIP_EXT];
    const XrHandJointLocationEXT& knuckle = joints[middleFinger ? XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT : XR_HAND_JOINT_INDEX_PROXIMAL_EXT];
    // Mode "shoulder" : ancre RIGIDE sur la main = base du doigt choisi, avancee le long de l'os metacarpien. Ne depend pas du geste
    // de clic (le bout de l'index bouge de 2-3 cm en pincant : partir de lui deplacait le rayon au moment du clic).
    const XrHandJointLocationEXT& metaJoint = joints[middleFinger ? XR_HAND_JOINT_MIDDLE_METACARPAL_EXT : XR_HAND_JOINT_INDEX_METACARPAL_EXT];
    const XrHandJointLocationEXT& proxJoint = joints[middleFinger ? XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT : XR_HAND_JOINT_INDEX_PROXIMAL_EXT];
    // Mode "palm" : articulation du doigt vers laquelle on vise depuis la paume (proximal, intermédiaire, distal, bout).
    const int aimOffset = cfg->palmAim == "proximal" ? 0 : (cfg->palmAim == "distal" ? 2 : (cfg->palmAim == "tip" ? 3 : 1));
    const XrHandJointLocationEXT& aimJoint = joints[(middleFinger ? XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT : XR_HAND_JOINT_INDEX_PROXIMAL_EXT) + aimOffset];
    // Mode "palm" : le rayon part de la paume et vise le doigt choisi dans sa pose naturelle (direction lissée lentement) ;
    // mode "hand" : rigide à la paume (axe de la main, origine à la base du doigt) ; mode "finger" : suit le bout du doigt.
    // Le geste de clic est indépendant (click_mode).
    const bool usable = XR_SUCCEEDED(r) && loc.isActive && JointUsable(palm, true, cfg->requireTracked) &&
                        JointUsable(ptrTip, true, cfg->requireTracked) && (!handMode || JointUsable(knuckle, false, cfg->requireTracked)) &&
                        (!palmMode || JointUsable(aimJoint, false, cfg->requireTracked)) &&
                        (!contactMode || (JointUsable(wrist, false, cfg->requireTracked) && JointUsable(clickTarget, false, cfg->clickRequireTracked) &&
                                          JointUsable(thumb, false, cfg->clickRequireTracked))) &&
                        (!shoulderMode || (JointUsable(metaJoint, false, cfg->requireTracked) && JointUsable(proxJoint, false, cfg->requireTracked) &&
                                            JointUsable(knuckle, false, cfg->requireTracked)));
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
    if (contactMode) {
        // Le rayon passe par le POINT DE CONTACT du clic : la surface de la cible (index, majeur...) du côté du pouce. Au clic, le
        // rayon est donc aligné avec l'endroit où le pouce touche le doigt. Direction : du poignet vers ce point (origine reculée, donc
        // peu sensible aux petits écarts) ; l'origine visible est ramenée vers la paume, sur la même droite.
        const em::Vec3 t = ToPose(clickTarget.pose).p, th = ToPose(thumb.pose).p, wr = ToPose(wrist.pose).p;
        const float rad = std::max(0.004f, std::min(clickTarget.radius, 0.012f));
        const em::Vec3 c = t + em::Normalized(th - t) * rad;
        const em::Vec3 d = em::Normalized(c - wr);
        const float len = std::max(0.04f, std::min(em::Length(c - palmPose.p), 0.16f));
        pointerWorld = {em::LookAlong(d, em::Rotate(palmPose.q, em::Vec3{0.0f, 1.0f, 0.0f})), c - d * len};
    }
    if (shoulderMode) {
        // Comme l'interface du casque : la direction va de l'EPAULE estimee vers un point de la main, et le rayon part de ce point (la ou se
        // rejoignent le pouce et l'index). Ce point est RIGIDE sur la main (base du doigt + `anchor_forward_mm` le long de l'os) : plier ou
        // pincer ne le deplace pas. La droite epaule -> main est longue (40 a 60 cm) : le rayon suit la position de la main par rapport au
        // corps et reste insensible au bruit des doigts. Sans position de tete : repli sur la direction poignet -> ancre.
        const em::Vec3 meta = ToPose(metaJoint.pose).p, prox = ToPose(proxJoint.pose).p, wr = ToPose(wrist.pose).p;
        const em::Vec3 a = cfg->anchorSource == "bone" ? prox + em::Normalized(prox - meta) * static_cast<float>(cfg->anchorForwardMm / 1000.0)
                                                       : ToPose(ptrTip.pose).p;  // "fingertip" : le bout du doigt du pointeur
        const em::Vec3 up{0.0f, 1.0f, 0.0f};
        em::Vec3 d = em::Normalized(a - wr);
        em::Pose head;
        if (HeadPose(base, time, &head)) {
            const em::Vec3 shoulder = em::EstimateShoulderYaw(head.p, g_torsoYaw, h == 1, static_cast<float>(cfg->shoulderSideMm / 1000.0),
                                                              static_cast<float>(cfg->shoulderDropMm / 1000.0), static_cast<float>(cfg->shoulderBackMm / 1000.0));
            d = em::Normalized(a - shoulder);
            // Doigt dans une autre direction que le bras (main verticale, bouton sur le cote) : le rayon bascule vers la direction du doigt.
            if (cfg->fingerBlend) {
                const em::Vec3 f = em::Normalized(ToPose(ptrTip.pose).p - ToPose(knuckle.pose).p);
                const float cosA = std::max(-1.0f, std::min(1.0f, em::Dot(f, d)));
                const float angleDeg = std::acos(cosA) * 57.29578f;
                const float s0 = static_cast<float>(cfg->fingerBlendStartDeg), s1 = static_cast<float>(cfg->fingerBlendEndDeg);
                float w = (angleDeg - s0) / (s1 - s0);
                w = std::max(0.0f, std::min(1.0f, w));
                w = w * w * (3.0f - 2.0f * w);  // transition douce
                if (w > 0.0f && em::Length(f) > 0.5f) {
                    const em::Vec3 mixed = em::Normalized(d * (1.0f - w) + f * w);
                    if (em::Length(mixed) > 0.5f) d = mixed;
                }
            }
        }
        pointerWorld = {em::LookAlong(d, up), a};
    }
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
        if (shoulderMode && cfg->shoulderSmoothHz > 0.0) {
            // Mode shoulder : la direction epaule -> doigt est filtree DANS LE MONDE (One Euro : tres stable a l'arret pour viser un petit
            // bouton, reactive quand la main bouge vite). Le tremblement de la main ne fait plus bouger le rayon.
            em::Pose w = pointerWorld;
            w.q = hs.worldRotFilter.Filter(w.q, dt, static_cast<float>(cfg->shoulderSmoothHz), static_cast<float>(cfg->shoulderSmoothBeta));
            w.p = hs.worldPosFilter.Filter(w.p, dt, 4.0f, 40.0f);
            hs.relOut = em::Multiply(em::Inverse(palmPose), w);
            if (cfg->pointerPitchDeg != 0.0 || cfg->pointerYawDeg != 0.0) hs.relOut.q = rawRel.q;  // reglage fin : pas de filtre
        } else if (cfg->smoothing && !shoulderMode) {  // mode shoulder : la droite epaule -> contact est deja stable, pas de lissage retardant
            // Mode palm : suivi lent de la direction (le doigt qui bouge pour cliquer ne déplace pas le rayon).
            const bool slowDirection = palmMode || contactMode;
            const float rotHz = slowDirection ? static_cast<float>(cfg->palmFollowHz) : static_cast<float>(cfg->smoothRotHz);
            const float rotBeta = static_cast<float>(cfg->smoothRotBeta) * (slowDirection ? 0.4f : 1.0f);
            hs.relOut.q = hs.rotFilter.Filter(rawRel.q, dt, rotHz, rotBeta);
            hs.relOut.p = hs.posFilter.Filter(rawRel.p, dt, 6.0f, 30.0f);
        } else {
            hs.relOut = rawRel;
        }
        hs.relTime = time;
        PushHistory(hs, time, hs.relOut);

        const XrHandJointLocationEXT& target = joints[ClickJointNearest(cfg->clickMode, joints)];
        if (JointUsable(thumb, false, cfg->clickRequireTracked) && JointUsable(target, false, cfg->clickRequireTracked)) {
            const bool was = hs.pressed;
            const float dist = em::Length(ToPose(target.pose).p - ToPose(thumb.pose).p);
            const float prevDist = hs.clickDist;
            hs.clickDist = dist;
            hs.pressed = hs.pinch.Update(dist, static_cast<float>(cfg->clickOnMm / 1000.0), static_cast<float>(cfg->clickOffMm / 1000.0));
            // Pre-verrouillage : le pointeur se fige des que le pouce se rapproche de la cible (le geste de clic dure ~150 ms et deplace deja
            // la main), a partir de la pose d'avant le geste. Il se libere si le pouce s'eloigne sans cliquer.
            const float armMm = static_cast<float>(cfg->clickOnMm + cfg->prelatchMm) / 1000.0f;
            if (cfg->prelatchMm > 0.0 && !hs.pressed) {
                if (!hs.prelatched && dist < armMm && dist < prevDist - 0.0005f && hs.preArmed) {
                    hs.prelatched = true;
                    hs.prelatchTick = GetTickCount64();
                    hs.latchedRel = HistoryAt(hs, time - static_cast<XrTime>(cfg->latchMs * 1e6));
                } else if (hs.prelatched && (dist > armMm + 0.004f || GetTickCount64() - hs.prelatchTick > 1500)) {
                    hs.prelatched = false;
                    hs.preArmed = false;  // reste desarme tant que le pouce n'est pas reparti loin
                }
                if (dist > armMm + 0.004f) hs.preArmed = true;
            }
            if (hs.pressed && !was) {
                if (!hs.prelatched) hs.latchedRel = HistoryAt(hs, time - static_cast<XrTime>(cfg->latchMs * 1e6));
                hs.prelatched = false;
                hs.latched = true;
            } else if (!hs.pressed && was) {
                hs.latched = false;
            }
            if (hs.pressed != was)
                Logger::Get().Log(LogLevel::Info, "emulation : main %s -> %s (distance %.1f mm)", h == 0 ? "gauche" : "droite",
                                  hs.pressed ? "APPUI (pincement)" : "relache", dist * 1000.0f);
        } else {
            hs.clickDist = 1.0f;  // pouce ou cible inutilisable : cette main ne peut pas s engager
        }
    }

    const bool frozen = hs.latched || hs.prelatched;
    em::Pose cand = em::Multiply(palmPose, frozen ? hs.latchedRel : hs.relOut);
    if (frozen && cfg->freezeDeadzoneDeg > 0.0) {
        // Zone morte : une fois fige, le pointeur ne bouge pas tant que la main reste dans une petite zone (tremblement, geste de clic) ;
        // au-dela il suit la main (glisser une molette) sans a-coup.
        if (!hs.stickyOn) {
            hs.sticky = cand;
            hs.stickyOn = true;
        } else {
            const float dzA = static_cast<float>(cfg->freezeDeadzoneDeg) * 3.14159265f / 180.0f;
            const float ang = em::AngleBetween(cand.q, hs.sticky.q);
            if (ang > dzA) hs.sticky.q = em::Nlerp(cand.q, hs.sticky.q, dzA / ang);
            const em::Vec3 dv = cand.p - hs.sticky.p;
            const float len = em::Length(dv), dzP = static_cast<float>(cfg->freezeDeadzoneMm) / 1000.0f;
            if (len > dzP) hs.sticky.p = cand.p - dv * (dzP / len);
        }
        cand = hs.sticky;
    } else {
        hs.stickyOn = false;
    }
    hs.outTip = cand;
    hs.lastValidTick = GetTickCount64();
    hs.lastValidTime = time;
    UpdateEngaged(*cfg);
    return true;
}

bool Fresh(int h) {
    return g_hand[h].tracker && g_hand[h].lastValidTick != 0 && GetTickCount64() - g_hand[h].lastValidTick < kFreshMs;
}

std::atomic<bool> g_menuOpen{false};  // menu des reglages ouvert dans le casque : le jeu ne recoit plus de clic

int g_engaged = -1;  // main engagee (0 gauche, 1 droite) quand le pointeur partage est actif ; -1 = aucune

// Pointeur partage : MSFS ne gere qu un controleur actif a la fois. On lui donne donc, aux DEUX emplacements (gauche et droite),
// les donnees de la main engagee : celle qui s apprete a cliquer (pouce proche de la cible) ou qui clique deja.
int SourceFor(int slot, const hands::config::Config& cfg) {
    if (!cfg.sharedPointer || g_engaged < 0 || !Fresh(g_engaged)) return slot;
    return g_engaged;
}

void UpdateEngaged(const hands::config::Config& cfg) {
    if (!cfg.sharedPointer) {
        g_engaged = -1;
        return;
    }
    const bool f[2] = {Fresh(0), Fresh(1)};
    int next = g_engaged;
    if (!f[0] && !f[1]) {
        g_engaged = -1;
        return;
    }
    if (next < 0 || !f[next]) {  // aucune main engagee, ou celle-ci est perdue : on prend la meilleure disponible
        next = !f[0] ? 1 : (!f[1] ? 0 : (g_hand[0].clickDist <= g_hand[1].clickDist ? 0 : 1));
    } else if (!g_hand[next].pressed) {  // une main en plein clic garde la main ; sinon on regarde l autre
        const int o = 1 - next;
        const float engage = static_cast<float>(cfg.engageMm / 1000.0), margin = 0.006f;
        if (f[o] && (g_hand[o].pressed || (g_hand[o].clickDist < engage && g_hand[o].clickDist + margin < g_hand[next].clickDist))) next = o;
    }
    if (next != g_engaged) {
        g_engaged = next;
        static int logsLeft = 60;
        if (logsLeft > 0) {
            --logsLeft;
            Logger::Get().Log(LogLevel::Info, "emulation : pointeur partage -> main %s engagee (distances pouce-cible : gauche %.0f mm, droite %.0f mm)",
                              next == 0 ? "gauche" : "droite", g_hand[0].clickDist * 1000.0f, g_hand[1].clickDist * 1000.0f);
        }
    }
}

// --- Hooks ---------------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location) {
    const XrResult r = g_dispatch.LocateSpace(space, baseSpace, time, location);
    if (!g_live.load(std::memory_order_acquire) || XR_FAILED(r) || !location) return r;
    Safe("xrLocateSpace", [&] {
        const auto cfg = CurrentConfig();
        if (!cfg->emulationEnabled) {
            // Mode « mains seules » : la pose de l'action (aim ou grip) d'une main suivie est déclarée non localisable.
            if (!cfg->hideHandControllers) return;
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto hit = g_spaces.find(Key(space));
            if (hit == g_spaces.end()) return;
            const int hslot = HandIndex(hit->second.subaction);
            if (hslot < 0 || !g_hideHand[hslot].load()) return;
            location->locationFlags = 0;
            for (auto* n = static_cast<XrBaseOutStructure*>(location->next); n; n = n->next)
                if (n->type == XR_TYPE_SPACE_VELOCITY) reinterpret_cast<XrSpaceVelocity*>(n)->velocityFlags = 0;
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_spaces.find(Key(space));
        if (it == g_spaces.end()) return;
        const int slot = HandIndex(it->second.subaction);
        if (slot < 0) return;
        bool ok[2] = {false, false};
        ok[slot] = Refresh(slot, baseSpace, time);
        if (cfg->sharedPointer) ok[1 - slot] = Refresh(1 - slot, baseSpace, time);  // l'autre main doit rester à jour pour pouvoir s'engager
        const int h = SourceFor(slot, *cfg);  // main dont les données sont fournies à cet emplacement
        if (!ok[h]) return;  // main non suivie : pose du runtime conservée
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
bool PressedFor(XrPath subaction, const hands::config::Config& cfg, bool* any, int* firstHand) {
    const bool menuOpen = g_menuOpen.load(std::memory_order_acquire);
    bool pressed = false;
    *any = false;
    *firstHand = -1;
    for (int h = 0; h < 2; ++h) {
        if (subaction != XR_NULL_PATH && HandIndex(subaction) != h) continue;
        const int src = SourceFor(h, cfg);  // pointeur partage : l emplacement h recoit les donnees de la main engagee
        if (!Fresh(src)) {
            if (g_hand[src].pressed || g_hand[src].latched) ResetPointer(g_hand[src]);  // pas d appui fantome au retour
            continue;
        }
        *any = true;
        if (*firstHand < 0) *firstHand = src;
        pressed = pressed || g_hand[src].pressed;
    }
    // Les deux mains pincees ensemble = geste d'ouverture du menu : le jeu ne doit pas y voir un clic.
    const bool bothPressed = Fresh(0) && Fresh(1) && g_hand[0].pressed && g_hand[1].pressed;
    return pressed && !menuOpen && !bothPressed;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo,
                                                            XrActionStateBoolean* state) {
    const XrResult r = g_dispatch.GetActionStateBoolean(session, getInfo, state);
    if (!g_live.load(std::memory_order_acquire) || XR_FAILED(r) || !getInfo || !state) return r;
    Safe("xrGetActionStateBoolean", [&] {
        const auto cfg = CurrentConfig();
        if (!cfg->emulationEnabled) {
            if (!cfg->hideHandControllers) return;
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_handBound.find(Key(getInfo->action)) == g_handBound.end()) return;
            // `last` : dernier état « enfoncé » vu par le jeu pour (action, main). Au début du masquage, le jeu voit un relâchement (une seule fois).
            uint8_t& last = g_reported[Key(getInfo->action) * 4 + static_cast<uint64_t>(HandIndex(getInfo->subactionPath) + 1)];
            if (ShouldMask(getInfo->action, getInfo->subactionPath)) {
                state->changedSinceLastSync = (last != 0) ? XR_TRUE : XR_FALSE;
                last = 0;
                state->currentState = XR_FALSE;
                state->isActive = XR_FALSE;
            } else {
                last = (state->currentState != XR_FALSE) ? 1 : 0;
            }
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_actionKinds.find(Key(getInfo->action));
        if (it == g_actionKinds.end() || it->second != ActionKind::TriggerBool) return;
        bool any = false;
        int h = -1;
        const bool pressed = PressedFor(getInfo->subactionPath, *cfg, &any, &h);
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
        const auto cfg = CurrentConfig();
        if (!cfg->emulationEnabled) {
            if (!cfg->hideHandControllers) return;
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_handBound.find(Key(getInfo->action)) == g_handBound.end()) return;
            uint8_t& last = g_reported[Key(getInfo->action) * 4 + static_cast<uint64_t>(HandIndex(getInfo->subactionPath) + 1)];
            if (ShouldMask(getInfo->action, getInfo->subactionPath)) {
                state->changedSinceLastSync = (last != 0) ? XR_TRUE : XR_FALSE;
                last = 0;
                state->currentState = 0.0f;
                state->isActive = XR_FALSE;
            } else {
                last = (state->currentState != 0.0f) ? 1 : 0;
            }
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_actionKinds.find(Key(getInfo->action));
        if (it == g_actionKinds.end() || it->second != ActionKind::TriggerFloat) return;
        bool any = false;
        int h = -1;
        const bool pressed = PressedFor(getInfo->subactionPath, *cfg, &any, &h);
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
        if (!cfg->emulationEnabled) {
            if (!cfg->hideHandControllers) return;
            std::lock_guard<std::mutex> lock(g_mutex);
            if (ShouldMask(getInfo->action, getInfo->subactionPath)) state->isActive = XR_FALSE;
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_actionKinds.find(Key(getInfo->action));
        if (it == g_actionKinds.end()) return;
        const XrPath sub = getInfo->subactionPath;
        for (int h = 0; h < 2; ++h) {
            if (sub != XR_NULL_PATH && HandIndex(sub) != h) continue;
            if (Fresh(SourceFor(h, *cfg))) { state->isActive = XR_TRUE; return; }
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
    if (!CurrentConfig()->emulationEnabled && !CurrentConfig()->handsEnabled && !CurrentConfig()->hideHandControllers && !CurrentConfig()->zonesEnabled) {
        log.Log(LogLevel::Info, "emulation, mains, zones et masquage des manettes : desactives dans hands.ini");
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
    if (d.CreateReferenceSpace && d.DestroySpace) {  // position de la tete, pour le pointeur depuis l'epaule
        XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        rci.poseInReferenceSpace.orientation.w = 1.0f;
        if (XR_FAILED(d.CreateReferenceSpace(session, &rci, &g_viewSpace))) g_viewSpace = XR_NULL_HANDLE;
        log.Log(LogLevel::Info, "emulation : espace de la tete (pointeur depuis l'epaule) : %s", g_viewSpace ? "ok" : "INDISPONIBLE (repli poignet)");
    }
    g_live.store(true, std::memory_order_release);
    const auto cfg = CurrentConfig();
    if (cfg->emulationEnabled)
        log.Log(LogLevel::Info,
                "emulation : ACTIVE (pointeur : %s sur le %s ; clic : %s %.0f/%.0f mm ; verrouillage %.0f ms ; lissage %s ; suivi exige : %s ; "
                "manettes du jeu %s)",
                cfg->pointerMode.c_str(), cfg->pointerFinger == "middle" ? "majeur" : "doigt index", cfg->clickMode.c_str(), cfg->clickOnMm, cfg->clickOffMm, cfg->latchMs,
                cfg->smoothing ? "oui" : "non", cfg->requireTracked ? "oui" : "non", cfg->hideControllerModel ? "masquees" : "visibles");
    else
        log.Log(LogLevel::Info, "emulation : desactivee (hand trackers ouverts : dessin des mains, zones, masquage des manettes virtuelles : %s)",
                cfg->hideHandControllers ? "OUI" : "non");
    return true;
}

// Derniere pose de pointeur calculee pour la main (dans l'espace de base du dernier appel), si la main est suivie.
bool Emu_LastPointerPose(int hand, XrPosef* out) {
    if (!g_live.load(std::memory_order_acquire) || hand < 0 || hand > 1 || !out) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!Fresh(hand)) return false;
    *out = ToXr(g_hand[hand].outTip);
    return true;
}

void Emu_SetMenuOpen(bool open) { g_menuOpen.store(open, std::memory_order_release); }

bool Emu_HeadPose(XrSpace base, XrTime time, XrPosef* out) {
    if (!g_live.load(std::memory_order_acquire) || !out) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    em::Pose p;
    if (!HeadPose(base, time, &p)) return false;
    *out = ToXr(p);
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
    if (clickJoint) *clickJoint = ClickJointNearest(CurrentConfig()->clickMode, out);
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
    if (g_viewSpace != XR_NULL_HANDLE && g_dispatch.DestroySpace) g_dispatch.DestroySpace(g_viewSpace);
    g_viewSpace = XR_NULL_HANDLE;
    g_spaces.clear();
    g_reported.clear();
    g_handBound.clear();
    for (int h = 0; h < 2; ++h) {
        g_presence[h].Reset();
        g_hideHand[h].store(false);
    }
    g_profileEventPending = false;
    g_torsoInit = false;
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

void Emu_OnHandBinding(XrAction action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handBound.insert(Key(action));
}

void Emu_UpdateHandPresence(XrTime displayTime) {
    if (!g_live.load(std::memory_order_acquire)) return;
    const auto cfg = CurrentConfig();
    const bool want = cfg->hideHandControllers && !cfg->emulationEnabled;
    const double nowMs = static_cast<double>(GetTickCount64());
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int h = 0; h < 2; ++h) {
        bool active = false;
        if (want && g_hand[h].tracker && g_viewSpace != XR_NULL_HANDLE && g_dispatch.LocateHandJointsEXT) {
            XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
            info.baseSpace = g_viewSpace;
            info.time = displayTime;
            XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
            XrHandJointLocationsEXT loc{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
            loc.jointCount = XR_HAND_JOINT_COUNT_EXT;
            loc.jointLocations = joints;
            active = XR_SUCCEEDED(g_dispatch.LocateHandJointsEXT(g_hand[h].tracker, &info, &loc)) && loc.isActive;
        }
        bool present = false;
        if (want) present = g_presence[h].Update(active, nowMs, cfg->hideControllersHoldMs);
        else g_presence[h].Reset();
        const bool prev = g_hideHand[h].exchange(present);
        if (present != prev) {
            g_profileEventPending = true;
            Logger::Get().Log(LogLevel::Info, "mains seules : manette virtuelle de la main %s %s", h == 0 ? "gauche" : "droite",
                              present ? "MASQUEE (main suivie)" : "de nouveau visible (main non suivie)");
        }
    }
    if (cfg->hideControllersProfile != g_lastProfileFake) {  // le jeu doit relire les profils quand ce réglage change à chaud
        g_lastProfileFake = cfg->hideControllersProfile;
        g_profileEventPending = true;
    }
}

bool Emu_ShouldHideProfile(XrPath topLevelUserPath) {
    if (!g_live.load(std::memory_order_acquire)) return false;
    const auto cfg = CurrentConfig();
    if (cfg->emulationEnabled || !cfg->hideHandControllers || !cfg->hideControllersProfile) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    const int h = HandIndex(topLevelUserPath);
    return h >= 0 && g_hideHand[h].load();
}

bool Emu_TakePendingProfileEvent(XrSession* session) {
    if (!g_live.load(std::memory_order_acquire)) return false;
    const auto cfg = CurrentConfig();
    if (cfg->emulationEnabled || !cfg->hideHandControllers || !cfg->hideControllersProfile) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_profileEventPending = false;  // inutile : rien n'est masqué au niveau du profil
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_profileEventPending || g_session == XR_NULL_HANDLE) return false;
    g_profileEventPending = false;
    if (session) *session = g_session;
    return true;
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
