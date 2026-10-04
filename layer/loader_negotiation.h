// loader_negotiation.h — structures d'échange entre le *loader* OpenXR et une
// API layer.
//
// IMPORTANT : le paquet d'en-têtes que j'ai pu récupérer (libopenxr-dev 1.0.20)
// ne contient pas le fichier officiel `openxr_loader_negotiation.h`. Ces
// définitions sont donc RECOPIÉES À LA MAIN d'après la spécification du loader
// (« OpenXR Loader - API Layer interface »). Elles forment une ABI (disposition
// binaire) stable, mais elles n'ont PAS été comparées à l'original :
// À VÉRIFIER lors d'une mise à jour des en-têtes (remplacer ce fichier par le
// fichier officiel si disponible). Une erreur ici ferait échouer le chargement
// de la couche (pas le jeu : le loader ignore alors la couche).
//
// NOTE C++ : `#pragma once` évite les inclusions multiples (comme un `using`
// qui ne s'appliquerait qu'une fois).
#pragma once

#include <openxr/openxr.h>

#define XR_CURRENT_LOADER_API_LAYER_VERSION 1
#define XR_API_LAYER_MAX_SETTINGS_PATH_SIZE 512

typedef enum XrLoaderInterfaceStructs {
    XR_LOADER_INTERFACE_STRUCT_UNINTIALIZED = 0,
    XR_LOADER_INTERFACE_STRUCT_LOADER_INFO = 1,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST = 2,
    XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST = 3,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO = 4,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO = 5,
} XrLoaderInterfaceStructs;

#define XR_NEGOTIATE_LOADER_INFO_STRUCT_VERSION 1
#define XR_NEGOTIATE_API_LAYER_REQUEST_STRUCT_VERSION 1
#define XR_API_LAYER_NEXT_INFO_STRUCT_VERSION 1
#define XR_API_LAYER_CREATE_INFO_STRUCT_VERSION 1

struct XrApiLayerCreateInfo;

typedef XrResult(XRAPI_PTR* PFN_xrCreateApiLayerInstance)(const XrInstanceCreateInfo* info,
                                                           const struct XrApiLayerCreateInfo* layerInfo,
                                                           XrInstance* instance);

// Ce que le loader nous dit de lui.
typedef struct XrNegotiateLoaderInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t minInterfaceVersion;
    uint32_t maxInterfaceVersion;
    XrVersion minApiVersion;
    XrVersion maxApiVersion;
} XrNegotiateLoaderInfo;

// Ce que nous répondons au loader (on remplit les deux pointeurs de fonctions).
typedef struct XrNegotiateApiLayerRequest {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t layerInterfaceVersion;
    XrVersion layerApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
    PFN_xrCreateApiLayerInstance createApiLayerInstance;
} XrNegotiateApiLayerRequest;

// Maillon de la chaîne de couches : comment appeler la couche SUIVANTE.
typedef struct XrApiLayerNextInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    char layerName[XR_MAX_API_LAYER_NAME_SIZE];
    PFN_xrGetInstanceProcAddr nextGetInstanceProcAddr;
    PFN_xrCreateApiLayerInstance nextCreateApiLayerInstance;
    struct XrApiLayerNextInfo* next;
} XrApiLayerNextInfo;

typedef struct XrApiLayerCreateInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    void* loaderInstance;
    char settings_file_location[XR_API_LAYER_MAX_SETTINGS_PATH_SIZE];
    XrApiLayerNextInfo* nextInfo;
} XrApiLayerCreateInfo;
