// state.h — état global de la couche (partagé entre layer.cpp et hooks_probe.cpp).
//
// NOTE C++ : `extern` déclare une variable globale définie dans UN SEUL .cpp.
// Les variables globales d'une DLL sont initialisées au chargement de la DLL.
// Ici on évite tout constructeur « lourd » global (pas d'appel système au
// chargement : c'est dangereux sous le loader lock Windows).
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <openxr/openxr.h>

#include "config/Config.h"
#include "log/Logger.h"
#include "loader_negotiation.h"

#define HANDS_LAYER_NAME "XR_APILAYER_HANDS_bare"
#define HANDS_LAYER_VERSION "0.1.0-etape0"

namespace layer {

// Pointeurs vers les fonctions de la couche SUIVANTE (finalement le runtime
// VDXR), obtenus avec xrGetInstanceProcAddr de la couche suivante.
// NOTE C++ : `PFN_xrXxx` est un « pointeur de fonction » ; l'équivalent C# est
// un delegate. On l'appelle avec la même syntaxe qu'une fonction : d.PathToString(...).
struct Dispatch {
    PFN_xrDestroyInstance DestroyInstance = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrSuggestInteractionProfileBindings SuggestInteractionProfileBindings = nullptr;
    PFN_xrCreateActionSet CreateActionSet = nullptr;
    PFN_xrCreateAction CreateAction = nullptr;
    PFN_xrAttachSessionActionSets AttachSessionActionSets = nullptr;
    PFN_xrCreateActionSpace CreateActionSpace = nullptr;
    PFN_xrPollEvent PollEvent = nullptr;
    PFN_xrGetCurrentInteractionProfile GetCurrentInteractionProfile = nullptr;
    PFN_xrPathToString PathToString = nullptr;
    PFN_xrStringToPath StringToPath = nullptr;
    PFN_xrResultToString ResultToString = nullptr;
    PFN_xrGetSystemProperties GetSystemProperties = nullptr;
    PFN_xrGetInstanceProperties GetInstanceProperties = nullptr;
    // Extension XR_EXT_hand_tracking : peut être absente (nullptr).
    PFN_xrCreateHandTrackerEXT CreateHandTrackerEXT = nullptr;
    PFN_xrDestroyHandTrackerEXT DestroyHandTrackerEXT = nullptr;
};

extern PFN_xrGetInstanceProcAddr g_nextGetInstanceProcAddr;  // couche suivante
extern Dispatch g_dispatch;                                  // rempli une fois, avant g_active
extern std::atomic<XrInstance> g_instance;                   // instance suivie (une seule)
extern std::atomic<bool> g_active;                           // les hooks sont-ils exposés ?
extern bool g_handExtensionEnabled;                          // XR_EXT_hand_tracking activée ?
extern bool g_handExtensionInjected;                         // ... par nous ?

// Ce que le loader nous a annoncé pendant la négociation (journalisé plus tard,
// quand le fichier de log est ouvert).
struct LoaderNegotiationInfo {
    uint32_t minInterface = 0, maxInterface = 0;
    XrVersion minApi = 0, maxApi = 0;
};
extern LoaderNegotiationInfo g_loaderInfo;

// Configuration courante (lecture sans verrou). Jamais nullptr une fois initialisée.
std::shared_ptr<const hands::config::Config> CurrentConfig();

// Fournis par layer.cpp
XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);

// Fournis par hooks_probe.cpp : renvoie la fonction « hook » pour `name`, ou nullptr.
PFN_xrVoidFunction FindHook(const char* name);
// Journalise la destruction proprement (appelé par le hook xrDestroyInstance).
std::string PathToStringSafe(XrInstance instance, XrPath path);

}  // namespace layer
