// hooks_probe.cpp — hooks de l'étape 0 : ils JOURNALISENT, ils ne modifient rien.
//
// Patron commun à chaque hook :
//     1. (facultatif) journaliser les arguments, dans un bloc protégé Safe(...) ;
//     2. appeler la VRAIE fonction (runtime) — jamais à l'intérieur d'un try ;
//     3. journaliser le résultat, dans un bloc protégé ;
//     4. renvoyer exactement ce que le runtime a renvoyé.
// Ainsi une exception dans NOTRE code de journalisation ne peut jamais empêcher
// l'appel réel ni changer son résultat (fail-open).
//
// NOTE C++ : `XRAPI_ATTR XrResult XRAPI_CALL` est la « convention d'appel » exigée
// par OpenXR (sur Windows 64 bits il n'y en a qu'une, mais on la respecte).
// Les `[&]{ ... }` sont des lambdas : comme `() => { ... }` en C#, `[&]`
// indiquant qu'elles capturent par référence les variables locales utilisées.
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "state.h"

namespace layer {

using hands::log::Logger;
using hands::log::LogLevel;

namespace {

// --- Aides ------------------------------------------------------------------

template <class F>
void Safe(const char* what, F&& f) noexcept {
    try {
        f();
    } catch (const std::exception& e) {
        try { Logger::Get().Log(LogLevel::Error, "exception dans le hook %s : %s", what, e.what()); } catch (...) {}
    } catch (...) {
        try { Logger::Get().Log(LogLevel::Error, "exception inconnue dans le hook %s", what); } catch (...) {}
    }
}

// Clé de table pour un handle OpenXR (pointeur opaque en 64 bits).
template <class H>
uint64_t Key(H h) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h)); }

std::mutex g_namesMutex;  // protège les deux tables ci-dessous (appels hors boucle de rendu)
std::unordered_map<uint64_t, std::string> g_actionSetNames;
std::unordered_map<uint64_t, std::string> g_actionNames;  // "jeu/action"
std::mutex g_profileMutex;
std::unordered_map<uint64_t, std::string> g_lastProfile;  // dernier profil vu par chemin utilisateur

std::string NameOfSet(XrActionSet s) {
    std::lock_guard<std::mutex> lock(g_namesMutex);
    auto it = g_actionSetNames.find(Key(s));
    return it == g_actionSetNames.end() ? "?" : it->second;
}
std::string NameOfAction(XrAction a) {
    std::lock_guard<std::mutex> lock(g_namesMutex);
    auto it = g_actionNames.find(Key(a));
    return it == g_actionNames.end() ? "?" : it->second;
}

std::string ResultName(XrResult r) {
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    const XrInstance inst = g_instance.load();
    if (g_dispatch.ResultToString && inst != XR_NULL_HANDLE &&
        XR_SUCCEEDED(g_dispatch.ResultToString(inst, r, buf)))
        return std::string(buf) + " (" + std::to_string(static_cast<int>(r)) + ")";
    return std::to_string(static_cast<int>(r));
}

const char* ActionTypeName(XrActionType t) {
    switch (t) {
        case XR_ACTION_TYPE_BOOLEAN_INPUT: return "bool";
        case XR_ACTION_TYPE_FLOAT_INPUT: return "float";
        case XR_ACTION_TYPE_VECTOR2F_INPUT: return "vec2";
        case XR_ACTION_TYPE_POSE_INPUT: return "pose";
        case XR_ACTION_TYPE_VIBRATION_OUTPUT: return "vibration";
        default: return "?";
    }
}

const char* SessionStateName(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "?";
    }
}

// Nom lisible d'une structure chaînée (`next`) dans xrCreateSession : permet de
// savoir quelle API graphique le jeu utilise (utile pour la surimpression, étape 3).
// Valeurs numériques tirées de la spécification (extensions KHR) :
const char* GraphicsBindingName(int type) {
    switch (type) {
        case 1000023000: return "OpenGL (Win32)";
        case 1000025000: return "Vulkan";
        case 1000027000: return "Direct3D 11";
        case 1000028000: return "Direct3D 12";
        default: return nullptr;
    }
}

// Log d'un chemin « /user/hand/left » à partir de son XrPath (entier opaque).
std::string PathText(XrPath p) { return PathToStringSafe(g_instance.load(), p); }

}  // namespace

std::string PathToStringSafe(XrInstance instance, XrPath path) {
    if (path == XR_NULL_PATH) return "(aucun)";
    if (!g_dispatch.PathToString || instance == XR_NULL_HANDLE) return "?";
    uint32_t len = 0;
    if (XR_FAILED(g_dispatch.PathToString(instance, path, 0, &len, nullptr)) || len == 0) return "?";
    std::string s(len, '\0');
    if (XR_FAILED(g_dispatch.PathToString(instance, path, len, &len, s.data()))) return "?";
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

namespace {

// --- Sonde du suivi des mains (appelée une fois par session) ----------------

void ProbeHandTracking(XrInstance instance, XrSession session, XrSystemId systemId) {
    Logger& log = Logger::Get();
    const Dispatch& d = g_dispatch;
    const auto cfg = CurrentConfig();
    if (!g_handExtensionEnabled) {
        log.Log(LogLevel::Warn, "sonde mains : XR_EXT_hand_tracking n'est PAS activee -> impossible de sonder");
        return;
    }
    XrSystemHandTrackingPropertiesEXT hand{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    props.next = &hand;  // on « chaîne » la structure d'extension (patron `next` d'OpenXR)
    const XrResult r = d.GetSystemProperties(instance, systemId, &props);
    if (XR_FAILED(r)) {
        log.Log(LogLevel::Warn, "sonde mains : xrGetSystemProperties a echoue : %s", ResultName(r).c_str());
        return;
    }
    log.Log(LogLevel::Info, "systeme : '%s' (vendeur 0x%04X) ; suivi position=%s orientation=%s",
            props.systemName, props.vendorId, props.trackingProperties.positionTracking ? "oui" : "non",
            props.trackingProperties.orientationTracking ? "oui" : "non");
    log.Log(LogLevel::Info, "systeme : image max %ux%u, couches max %u", props.graphicsProperties.maxSwapchainImageWidth,
            props.graphicsProperties.maxSwapchainImageHeight, props.graphicsProperties.maxLayerCount);
    log.Log(LogLevel::Info, "*** supportsHandTracking = %s ***", hand.supportsHandTracking ? "OUI" : "NON");

    if (!hand.supportsHandTracking || !cfg->probeHandTracker) return;
    if (!d.CreateHandTrackerEXT || !d.DestroyHandTrackerEXT) {
        log.Log(LogLevel::Warn, "sonde mains : fonctions xrCreate/DestroyHandTrackerEXT absentes");
        return;
    }
    const XrHandEXT hands[2] = {XR_HAND_LEFT_EXT, XR_HAND_RIGHT_EXT};
    const char* names[2] = {"gauche", "droite"};
    for (int i = 0; i < 2; ++i) {
        XrHandTrackerCreateInfoEXT ci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        ci.hand = hands[i];
        ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        XrHandTrackerEXT tracker = XR_NULL_HANDLE;
        const XrResult cr = d.CreateHandTrackerEXT(session, &ci, &tracker);
        log.Log(LogLevel::Info, "sonde mains : creation du hand tracker %s -> %s", names[i], ResultName(cr).c_str());
        if (XR_SUCCEEDED(cr) && tracker) d.DestroyHandTrackerEXT(tracker);
    }
}

// --- Hooks ---------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrDestroyInstance(XrInstance instance) {
    Safe("xrDestroyInstance", [&] { Logger::Get().Log(LogLevel::Info, "xrDestroyInstance"); });
    const auto destroy = g_dispatch.DestroyInstance;
    g_active.store(false, std::memory_order_release);  // les appels suivants passent tels quels
    const XrResult r = destroy(instance);
    g_instance.store(XR_NULL_HANDLE, std::memory_order_release);
    Logger::Get().Flush(std::chrono::milliseconds(300));  // destruction : hors boucle de rendu
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo,
                                                    XrSession* session) {
    Safe("xrCreateSession(avant)", [&] {
        if (!createInfo) return;
        Logger& log = Logger::Get();
        log.Log(LogLevel::Info, "xrCreateSession : systemId=%llu flags=0x%llX", static_cast<unsigned long long>(createInfo->systemId),
                static_cast<unsigned long long>(createInfo->createFlags));
        for (const auto* n = static_cast<const XrBaseInStructure*>(createInfo->next); n; n = n->next) {
            const int t = static_cast<int>(n->type);
            if (const char* g = GraphicsBindingName(t))
                log.Log(LogLevel::Info, "  API graphique du jeu : %s (type de structure %d)", g, t);
            else
                log.Log(LogLevel::Info, "  structure chainee : type %d", t);
        }
    });
    const XrResult r = g_dispatch.CreateSession(instance, createInfo, session);
    Safe("xrCreateSession(apres)", [&] {
        Logger::Get().Log(LogLevel::Info, "xrCreateSession -> %s", ResultName(r).c_str());
        if (XR_SUCCEEDED(r) && createInfo && session && *session) ProbeHandTracking(instance, *session, createInfo->systemId);
    });
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo* createInfo,
                                                      XrActionSet* actionSet) {
    const XrResult r = g_dispatch.CreateActionSet(instance, createInfo, actionSet);
    Safe("xrCreateActionSet", [&] {
        if (!createInfo) return;
        Logger::Get().Log(LogLevel::Info, "xrCreateActionSet '%s' (affiche: '%s', priorite %u) -> %s",
                          createInfo->actionSetName, createInfo->localizedActionSetName, createInfo->priority,
                          ResultName(r).c_str());
        if (XR_SUCCEEDED(r) && actionSet) {
            std::lock_guard<std::mutex> lock(g_namesMutex);
            g_actionSetNames[Key(*actionSet)] = createInfo->actionSetName;
        }
    });
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo* createInfo,
                                                   XrAction* action) {
    const XrResult r = g_dispatch.CreateAction(actionSet, createInfo, action);
    Safe("xrCreateAction", [&] {
        if (!createInfo) return;
        const std::string set = NameOfSet(actionSet);
        std::string subs;
        for (uint32_t i = 0; i < createInfo->countSubactionPaths; ++i) {
            if (i) subs += ", ";
            subs += PathText(createInfo->subactionPaths[i]);
        }
        Logger::Get().Log(LogLevel::Info, "xrCreateAction %s/%s type=%s affiche='%s' sous-actions=[%s] -> %s", set.c_str(),
                          createInfo->actionName, ActionTypeName(createInfo->actionType), createInfo->localizedActionName,
                          subs.c_str(), XR_SUCCEEDED(r) ? "ok" : ResultName(r).c_str());
        if (XR_SUCCEEDED(r) && action) {
            std::lock_guard<std::mutex> lock(g_namesMutex);
            g_actionNames[Key(*action)] = set + "/" + createInfo->actionName;
        }
    });
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* suggested) {
    Safe("xrSuggestInteractionProfileBindings", [&] {
        if (!suggested) return;
        Logger& log = Logger::Get();
        log.Log(LogLevel::Info, ">>> PROFIL SUGGERE PAR LE JEU : %s (%u bindings)",
                PathToStringSafe(instance, suggested->interactionProfile).c_str(), suggested->countSuggestedBindings);
        if (CurrentConfig()->logBindings) {
            for (uint32_t i = 0; i < suggested->countSuggestedBindings; ++i) {
                const auto& b = suggested->suggestedBindings[i];
                log.Log(LogLevel::Info, "    %-40s <- %s", NameOfAction(b.action).c_str(),
                        PathToStringSafe(instance, b.binding).c_str());
            }
        }
    });
    const XrResult r = g_dispatch.SuggestInteractionProfileBindings(instance, suggested);
    Safe("xrSuggestInteractionProfileBindings(apres)", [&] {
        Logger::Get().Log(LogLevel::Info, "xrSuggestInteractionProfileBindings -> %s", ResultName(r).c_str());
    });
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrAttachSessionActionSets(XrSession session,
                                                              const XrSessionActionSetsAttachInfo* attachInfo) {
    Safe("xrAttachSessionActionSets", [&] {
        if (!attachInfo) return;
        for (uint32_t i = 0; i < attachInfo->countActionSets; ++i)
            Logger::Get().Log(LogLevel::Info, "xrAttachSessionActionSets : jeu d'actions '%s'",
                              NameOfSet(attachInfo->actionSets[i]).c_str());
    });
    return g_dispatch.AttachSessionActionSets(session, attachInfo);
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* createInfo,
                                                        XrSpace* space) {
    const XrResult r = g_dispatch.CreateActionSpace(session, createInfo, space);
    Safe("xrCreateActionSpace", [&] {
        if (!createInfo) return;
        // poseInActionSpace = décalage que le jeu applique à la pose de l'action. C'est un
        // indice sur le « point de contact » vu par MSFS (inconnue à lever).
        const auto& p = createInfo->poseInActionSpace;
        Logger::Get().Log(LogLevel::Info,
                          "xrCreateActionSpace action=%s sous-chemin=%s decalage pos=(%.3f %.3f %.3f) rot=(%.3f %.3f %.3f %.3f) -> %s",
                          NameOfAction(createInfo->action).c_str(), PathText(createInfo->subactionPath).c_str(),
                          p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z,
                          p.orientation.w, XR_SUCCEEDED(r) ? "ok" : ResultName(r).c_str());
    });
    return r;
}

// Journalise le profil actif sur /user/hand/left et /user/hand/right (si changé).
void LogCurrentProfiles(XrSession session) {
    const XrInstance inst = g_instance.load();
    const char* tops[2] = {"/user/hand/left", "/user/hand/right"};
    for (const char* top : tops) {
        XrPath topPath = XR_NULL_PATH;
        if (XR_FAILED(g_dispatch.StringToPath(inst, top, &topPath))) continue;
        XrInteractionProfileState st{XR_TYPE_INTERACTION_PROFILE_STATE};
        const XrResult r = g_dispatch.GetCurrentInteractionProfile(session, topPath, &st);
        Logger::Get().Log(LogLevel::Info, "profil actif sur %s : %s%s", top,
                          XR_SUCCEEDED(r) ? PathToStringSafe(inst, st.interactionProfile).c_str() : "(erreur)",
                          XR_SUCCEEDED(r) && st.interactionProfile == XR_NULL_PATH ? " -> AUCUN contrôleur actif" : "");
    }
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData) {
    const XrResult r = g_dispatch.PollEvent(instance, eventData);
    // Chemin rapide : appelé à chaque image. On ne fait quelque chose que si un
    // événement intéressant est présent.
    if (r == XR_SUCCESS && eventData) {
        Safe("xrPollEvent", [&] {
            if (eventData->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto* e = reinterpret_cast<const XrEventDataSessionStateChanged*>(eventData);
                Logger::Get().Log(LogLevel::Info, "evenement : etat de session -> %s", SessionStateName(e->state));
            } else if (eventData->type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
                const auto* e = reinterpret_cast<const XrEventDataInteractionProfileChanged*>(eventData);
                Logger::Get().Log(LogLevel::Info, "evenement : le profil d'interaction a change");
                LogCurrentProfiles(e->session);
            }
        });
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath,
                                                                   XrInteractionProfileState* state) {
    const XrResult r = g_dispatch.GetCurrentInteractionProfile(session, topLevelUserPath, state);
    Safe("xrGetCurrentInteractionProfile", [&] {
        if (XR_FAILED(r) || !state) return;
        const std::string profile = PathText(state->interactionProfile);
        {
            std::lock_guard<std::mutex> lock(g_profileMutex);
            auto& last = g_lastProfile[static_cast<uint64_t>(topLevelUserPath)];
            if (last == profile) return;  // inchangé : on ne répète pas
            last = profile;
        }
        Logger::Get().Log(LogLevel::Info, "xrGetCurrentInteractionProfile(%s) = %s", PathText(topLevelUserPath).c_str(),
                          profile.c_str());
    });
    return r;
}

struct HookEntry {
    const char* name;
    PFN_xrVoidFunction fn;
};

#define HOOK(n) {#n, reinterpret_cast<PFN_xrVoidFunction>(Hook_##n)}
const HookEntry kHooks[] = {
    HOOK(xrDestroyInstance),
    HOOK(xrCreateSession),
    HOOK(xrCreateActionSet),
    HOOK(xrCreateAction),
    HOOK(xrSuggestInteractionProfileBindings),
    HOOK(xrAttachSessionActionSets),
    HOOK(xrCreateActionSpace),
    HOOK(xrPollEvent),
    HOOK(xrGetCurrentInteractionProfile),
};
#undef HOOK

}  // namespace

PFN_xrVoidFunction FindHook(const char* name) {
    for (const HookEntry& h : kHooks)
        if (std::strcmp(h.name, name) == 0) return h.fn;
    return nullptr;
}

}  // namespace layer
