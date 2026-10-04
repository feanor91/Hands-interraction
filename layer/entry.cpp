// entry.cpp — points d'entrée de la DLL : DllMain et négociation avec le loader.
//
// Séquence au chargement (côté loader OpenXR, dans le processus du jeu) :
//   1. Le loader lit le registre, trouve notre manifeste JSON, charge la DLL.
//   2. Il appelle xrNegotiateLoaderApiLayerInterface (ci-dessous) : on lui
//      remet nos deux fonctions d'entrée (GetInstanceProcAddr, CreateApiLayerInstance).
//   3. Plus tard, le jeu appelle xrCreateInstance -> le loader appelle notre
//      xrCreateApiLayerInstance (layer.cpp).
#include <windows.h>

#include <cstring>

#include "state.h"

namespace layer {
HMODULE g_module = nullptr;  // utilisé par layer.cpp pour retrouver le dossier de la DLL
XrResult XRAPI_CALL Layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                   const XrApiLayerCreateInfo* layerInfo, XrInstance* instance);
}  // namespace layer

// DllMain est appelée par Windows à chaque (dé)chargement de la DLL, SOUS le
// « loader lock » : on n'y fait RIEN d'autre que mémoriser le module. Surtout
// pas de log, de thread, de LoadLibrary ni d'allocation importante.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        layer::g_module = module;
        DisableThreadLibraryCalls(module);  // inutile de nous notifier des threads
    }
    return TRUE;
}

// NOTE C++ : `extern "C"` désactive la décoration de nom propre à C++ pour que
// le loader trouve exactement « xrNegotiateLoaderApiLayerInterface » (GetProcAddress).
// `__declspec(dllexport)` exporte la fonction (comme `public` pour une DLL).
extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loaderInfo, const char* layerName, XrNegotiateApiLayerRequest* request) {
    // Fail-open : en cas de doute on REFUSE la négociation ; le loader ignore
    // alors simplement notre couche et le jeu continue sans elle.
    if (!loaderInfo || !request) return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_NEGOTIATE_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo))
        return XR_ERROR_INITIALIZATION_FAILED;
    if (request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        request->structVersion != XR_NEGOTIATE_API_LAYER_REQUEST_STRUCT_VERSION ||
        request->structSize != sizeof(XrNegotiateApiLayerRequest))
        return XR_ERROR_INITIALIZATION_FAILED;
    if (layerName && std::strcmp(layerName, HANDS_LAYER_NAME) != 0) return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;

    layer::g_loaderInfo = {loaderInfo->minInterfaceVersion, loaderInfo->maxInterfaceVersion,
                           loaderInfo->minApiVersion, loaderInfo->maxApiVersion};
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_CURRENT_API_VERSION;
    request->getInstanceProcAddr = layer::Layer_xrGetInstanceProcAddr;
    request->createApiLayerInstance = layer::Layer_xrCreateApiLayerInstance;
    return XR_SUCCESS;
}
