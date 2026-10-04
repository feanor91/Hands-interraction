// layer.cpp — création/destruction de l'instance, dispatch, initialisation.
//
// Rappel de vocabulaire OpenXR pour un développeur C# :
//  * XrInstance : « connexion » au runtime (VDXR). Un handle opaque (pointeur).
//  * Un handle OpenXR n'est jamais déréférencé par nous : on le transmet.
//  * xrGetInstanceProcAddr(instance, "xrNom") : équivalent de GetProcAddress ;
//    c'est ainsi qu'une application (ou une couche) obtient chaque fonction.
//    Une couche se glisse dans ce mécanisme : quand le jeu demande "xrCreateSession",
//    on lui remet NOTRE fonction à la place, qui appellera ensuite la vraie.
#include <windows.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "state.h"
#include "util/StringUtil.h"

namespace layer {

extern HMODULE g_module;  // défini dans entry.cpp

PFN_xrGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;
Dispatch g_dispatch;
std::atomic<XrInstance> g_instance{XR_NULL_HANDLE};
std::atomic<bool> g_active{false};
bool g_handExtensionEnabled = false;
bool g_handExtensionInjected = false;
LoaderNegotiationInfo g_loaderInfo;

namespace {

using hands::config::ConfigStore;
using hands::log::Logger;
using hands::log::LogLevel;

std::unique_ptr<ConfigStore> g_configStore;  // jamais détruit explicitement (voir ci-dessous)
std::once_flag g_configOnce;
std::once_flag g_servicesOnce;
std::string g_exePath;   // chemin complet de l'exécutable hôte (UTF-8)
std::string g_exeName;   // nom de fichier seul
std::filesystem::path g_baseDir;  // dossier de la DLL (config + logs)

// ---------- Conversions et chemins -----------------------------------------

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ModulePath(HMODULE module) {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return buf;
}

// Charge la configuration (sans thread, sans fichier de log) afin de décider
// si la couche doit s'activer pour ce processus.
void LoadConfigOnce() {
    std::call_once(g_configOnce, [] {
        const std::wstring exe = ModulePath(nullptr);
        g_exePath = WideToUtf8(exe);
        g_exeName = hands::util::FileNameOf(g_exePath);
        g_baseDir = std::filesystem::path(ModulePath(g_module)).parent_path();
        g_configStore = std::make_unique<ConfigStore>(g_baseDir / "hands.ini");
        std::string msg;
        g_configStore->Reload(&msg);  // fichier absent : valeurs par défaut
    });
}

// Ouvre le log et lance la surveillance de la config — uniquement pour les
// processus où la couche est active.
void StartServicesOnce() {
    std::call_once(g_servicesOnce, [] {
        Logger& log = Logger::Get();
        std::string stem = g_exeName;
        for (char& c : stem)
            if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
                c = '_';
        const auto name = "layer_" + stem + ".log";
        bool ok = log.Open(g_baseDir / "logs" / name);
        std::string where = "dossier de la couche";
        if (!ok) {
            // Repli : %TEMP%. Utile si le dossier n'est pas inscriptible par le jeu
            // (application Microsoft Store en bac à sable, droits manquants).
            wchar_t tmp[MAX_PATH + 1] = {};
            const DWORD n = GetTempPathW(MAX_PATH, tmp);
            if (n > 0) {
                ok = log.Open(std::filesystem::path(std::wstring(tmp, n)) / "HandsLayer" / name);
                where = "%TEMP% (le dossier de la couche n'est pas inscriptible)";
            }
        }
        log.SetLevel(CurrentConfig()->logLevel);
        if (ok) log.Log(LogLevel::Info, "journal ouvert dans : %s", where.c_str());

        g_configStore->StartWatching(std::chrono::milliseconds(1000), [](const hands::config::Config& c, const std::string& msg) {
            Logger::Get().SetLevel(c.logLevel);
            Logger::Get().Log(LogLevel::Info, "config rechargee a chaud : %s", msg.c_str());
        });
    });
}

// Fonction de convenance : appelle une fonction de la couche suivante par son nom.
PFN_xrVoidFunction GetNext(XrInstance instance, const char* name) {
    PFN_xrVoidFunction fn = nullptr;
    if (g_nextGetInstanceProcAddr && XR_SUCCEEDED(g_nextGetInstanceProcAddr(instance, name, &fn))) return fn;
    return nullptr;
}

// Liste les extensions que le runtime (couche suivante) propose avant la création.
std::vector<std::string> EnumerateRuntimeExtensions() {
    std::vector<std::string> out;
    auto fn = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(
        GetNext(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties"));
    if (!fn) return out;
    uint32_t count = 0;
    if (XR_FAILED(fn(nullptr, 0, &count, nullptr)) || count == 0) return out;
    std::vector<XrExtensionProperties> props(count);
    for (auto& p : props) { p.type = XR_TYPE_EXTENSION_PROPERTIES; p.next = nullptr; }
    if (XR_FAILED(fn(nullptr, count, &count, props.data()))) return out;
    for (uint32_t i = 0; i < count; ++i) out.emplace_back(props[i].extensionName);
    return out;
}

bool Contains(const std::vector<std::string>& v, const char* s) {
    for (const auto& x : v) if (x == s) return true;
    return false;
}

template <class F>
void Safe(const char* what, F&& f) noexcept {
    try { f(); }
    catch (const std::exception& e) {
        try { Logger::Get().Log(LogLevel::Error, "exception dans %s : %s", what, e.what()); } catch (...) {}
    } catch (...) {
        try { Logger::Get().Log(LogLevel::Error, "exception inconnue dans %s", what); } catch (...) {}
    }
}

}  // namespace

std::shared_ptr<const hands::config::Config> CurrentConfig() {
    if (!g_configStore) return std::make_shared<const hands::config::Config>();
    return g_configStore->Get();
}

// ---------------------------------------------------------------------------
// xrGetInstanceProcAddr : le jeu (ou une couche au-dessus) demande une fonction.
XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!g_nextGetInstanceProcAddr) return XR_ERROR_INITIALIZATION_FAILED;
    // On demande TOUJOURS d'abord la vraie fonction à la couche suivante : si elle
    // n'existe pas (fonction non supportée), on ne la fabrique pas.
    const XrResult r = g_nextGetInstanceProcAddr(instance, name, function);
    if (XR_FAILED(r) || !name || !function) return r;
    // Fail-open : couche inactive ou instance qui n'est pas la nôtre -> pass-through.
    if (!g_active.load(std::memory_order_acquire) || instance != g_instance.load(std::memory_order_acquire)) return r;

    if (std::strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(Layer_xrGetInstanceProcAddr);
        return r;
    }
    if (PFN_xrVoidFunction hook = FindHook(name)) *function = hook;
    return r;
}

// ---------------------------------------------------------------------------
// xrCreateApiLayerInstance : appelée par le loader quand le jeu fait xrCreateInstance.
XrResult XRAPI_CALL Layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                   const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
    // Validation stricte : si la chaîne n'est pas celle attendue, on ne peut pas
    // appeler la couche suivante -> on échoue proprement (le loader gère).
    if (!layerInfo || layerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        layerInfo->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
        layerInfo->structSize != sizeof(XrApiLayerCreateInfo) || !layerInfo->nextInfo ||
        std::strcmp(layerInfo->nextInfo->layerName, HANDS_LAYER_NAME) != 0 ||
        !layerInfo->nextInfo->nextGetInstanceProcAddr || !layerInfo->nextInfo->nextCreateApiLayerInstance)
        return XR_ERROR_INITIALIZATION_FAILED;

    // On mémorise la couche suivante, puis on prépare l'appel qui lui sera fait :
    // la même structure, mais dont `nextInfo` pointe sur le maillon d'après.
    g_nextGetInstanceProcAddr = layerInfo->nextInfo->nextGetInstanceProcAddr;
    const PFN_xrCreateApiLayerInstance nextCreate = layerInfo->nextInfo->nextCreateApiLayerInstance;
    XrApiLayerCreateInfo downInfo = *layerInfo;
    downInfo.nextInfo = layerInfo->nextInfo->next;

    // --- Décision d'activation (aucun effet de bord, aucune exception) --------
    bool active = false;
    Safe("activation", [&] {
        LoadConfigOnce();
        const auto cfg = CurrentConfig();
        active = cfg->discoveryMode || hands::config::MatchesProcess(cfg->processAllowlist, g_exePath);
    });
    if (!active || g_instance.load() != XR_NULL_HANDLE) {
        // Processus non concerné (ou 2e instance dans le même processus) : on laisse
        // TOUT passer sans rien modifier. Pas de log, pas de thread.
        return nextCreate(info, &downInfo, instance);
    }

    Safe("services", [] { StartServicesOnce(); });
    Logger& log = Logger::Get();

    // --- Journal de démarrage ---------------------------------------------------
    std::vector<std::string> runtimeExts;
    Safe("journal de demarrage", [&] {
        const auto cfg = CurrentConfig();
        const bool matched = hands::config::MatchesProcess(cfg->processAllowlist, g_exePath);
        log.Log(LogLevel::Info, "=== %s %s ===", HANDS_LAYER_NAME, HANDS_LAYER_VERSION);
        log.Log(LogLevel::Info, "processus : %s (pid %lu, %u bits)", g_exePath.c_str(),
                static_cast<unsigned long>(GetCurrentProcessId()), static_cast<unsigned>(sizeof(void*) * 8));
        log.Log(LogLevel::Info, "nom d'exe = '%s' ; dans la liste autorisee : %s ; mode decouverte : %s",
                g_exeName.c_str(), matched ? "OUI" : "non", cfg->discoveryMode ? "oui" : "non");
        log.Log(LogLevel::Info, "DLL : %s", WideToUtf8(ModulePath(g_module)).c_str());
        log.Log(LogLevel::Info, "loader : interface %u..%u, api %u.%u.%u..%u.%u.%u", g_loaderInfo.minInterface,
                g_loaderInfo.maxInterface, XR_VERSION_MAJOR(g_loaderInfo.minApi), XR_VERSION_MINOR(g_loaderInfo.minApi),
                static_cast<unsigned>(XR_VERSION_PATCH(g_loaderInfo.minApi)), XR_VERSION_MAJOR(g_loaderInfo.maxApi),
                XR_VERSION_MINOR(g_loaderInfo.maxApi), static_cast<unsigned>(XR_VERSION_PATCH(g_loaderInfo.maxApi)));
        if (info) {
            const auto& a = info->applicationInfo;
            log.Log(LogLevel::Info, "application : '%s' v%u, moteur : '%s' v%u, api demandee %u.%u.%u", a.applicationName,
                    a.applicationVersion, a.engineName, a.engineVersion, XR_VERSION_MAJOR(a.apiVersion),
                    XR_VERSION_MINOR(a.apiVersion), static_cast<unsigned>(XR_VERSION_PATCH(a.apiVersion)));
            for (uint32_t i = 0; i < info->enabledApiLayerCount; ++i)
                log.Log(LogLevel::Info, "couche API demandee par l'app : %s", info->enabledApiLayerNames[i]);
        }
        runtimeExts = EnumerateRuntimeExtensions();
        log.Log(LogLevel::Info, "extensions proposees par le runtime : %u", static_cast<unsigned>(runtimeExts.size()));
        for (const auto& e : runtimeExts) {
            bool requested = false;
            if (info)
                for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
                    if (e == info->enabledExtensionNames[i]) requested = true;
            log.Log(LogLevel::Info, "  %c %s", requested ? '*' : ' ', e.c_str());
        }
        log.Log(LogLevel::Info, "(* = demandee par l'application)");
        log.Log(LogLevel::Info, "XR_EXT_hand_tracking proposee par le runtime : %s",
                Contains(runtimeExts, XR_EXT_HAND_TRACKING_EXTENSION_NAME) ? "OUI" : "NON");
    });

    // --- Création de l'instance, avec injection éventuelle de XR_EXT_hand_tracking ---
    g_handExtensionEnabled = false;
    g_handExtensionInjected = false;
    if (info)
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
            if (std::strcmp(info->enabledExtensionNames[i], XR_EXT_HAND_TRACKING_EXTENSION_NAME) == 0)
                g_handExtensionEnabled = true;

    XrResult result = XR_ERROR_INITIALIZATION_FAILED;
    bool created = false;
    const bool wantInject = info && !g_handExtensionEnabled && CurrentConfig()->enableHandTrackingExtension &&
                            Contains(runtimeExts, XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    if (wantInject) {
        // On ajoute le nom de l'extension à une COPIE de la liste du jeu ; la structure
        // d'origine n'est jamais modifiée. En cas d'échec on retente avec l'original.
        std::vector<const char*> names(info->enabledExtensionNames, info->enabledExtensionNames + info->enabledExtensionCount);
        names.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        XrInstanceCreateInfo modified = *info;
        modified.enabledExtensionCount = static_cast<uint32_t>(names.size());
        modified.enabledExtensionNames = names.data();
        result = nextCreate(&modified, &downInfo, instance);
        if (XR_SUCCEEDED(result)) {
            created = true;
            g_handExtensionEnabled = g_handExtensionInjected = true;
            log.Log(LogLevel::Info, "XR_EXT_hand_tracking ajoutee aux extensions activees (injection)");
        } else {
            log.Log(LogLevel::Warn, "creation avec XR_EXT_hand_tracking refusee (code %d) : nouvel essai a l'identique de l'app",
                    static_cast<int>(result));
        }
    }
    if (!created) {
        result = nextCreate(info, &downInfo, instance);
        if (XR_FAILED(result)) {
            log.Log(LogLevel::Error, "xrCreateInstance a echoue en aval (code %d) : rien n'est modifie", static_cast<int>(result));
            log.Flush(std::chrono::milliseconds(200));
            return result;
        }
    }

    // --- Table de dispatch -------------------------------------------------------
    Safe("dispatch", [&] {
        const XrInstance inst = *instance;
        Dispatch d;
#define LOAD(fn) d.fn = reinterpret_cast<PFN_xr##fn>(GetNext(inst, "xr" #fn))
        LOAD(DestroyInstance); LOAD(CreateSession); LOAD(SuggestInteractionProfileBindings);
        LOAD(CreateActionSet); LOAD(CreateAction); LOAD(AttachSessionActionSets); LOAD(CreateActionSpace);
        LOAD(PollEvent); LOAD(GetCurrentInteractionProfile); LOAD(PathToString); LOAD(StringToPath);
        LOAD(ResultToString); LOAD(GetSystemProperties); LOAD(GetInstanceProperties);
        LOAD(CreateHandTrackerEXT); LOAD(DestroyHandTrackerEXT);
#undef LOAD
        const bool coreOk = d.DestroyInstance && d.CreateSession && d.SuggestInteractionProfileBindings &&
                            d.CreateActionSet && d.CreateAction && d.AttachSessionActionSets && d.CreateActionSpace &&
                            d.PollEvent && d.GetCurrentInteractionProfile && d.PathToString && d.StringToPath &&
                            d.ResultToString && d.GetSystemProperties && d.GetInstanceProperties;
        if (!coreOk) {
            // Fail-open : on n'expose AUCUN hook ; le jeu parle directement au runtime.
            log.Log(LogLevel::Error, "fonctions de base manquantes dans le runtime : couche desactivee (pass-through)");
            return;
        }
        g_dispatch = d;
        g_instance.store(inst, std::memory_order_release);
        g_active.store(true, std::memory_order_release);

        XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(d.GetInstanceProperties(inst, &props)))
            log.Log(LogLevel::Info, "runtime : '%s' version %u.%u.%u", props.runtimeName,
                    XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
                    static_cast<unsigned>(XR_VERSION_PATCH(props.runtimeVersion)));
        log.Log(LogLevel::Info, "instance creee. suivi des mains : extension %s%s ; fonctions hand tracker %s",
                g_handExtensionEnabled ? "ACTIVE" : "non activee", g_handExtensionInjected ? " (par la couche)" : "",
                (d.CreateHandTrackerEXT && d.DestroyHandTrackerEXT) ? "disponibles" : "absentes");
    });
    log.Flush(std::chrono::milliseconds(200));  // hors boucle de rendu : on est au démarrage
    return result;
}

}  // namespace layer
