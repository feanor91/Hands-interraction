// hands_overlay.cpp — étape 3a : dessine le squelette des mains par-dessus le jeu.
//
// Principe (pour un développeur C#) : un jeu OpenXR termine chaque image par xrEndFrame(info), où
// `info->layers` est la liste des « couches » à composer (le monde rendu par le jeu = une couche
// « projection » à deux vues, une par œil). On intercepte xrEndFrame, on dessine les mains dans nos
// propres images (deux swapchains, une par œil, créées auprès du runtime), puis on appelle le VRAI
// xrEndFrame avec la liste des couches du jeu + UNE couche de plus : la nôtre.
//
// Fail-open : toute erreur d'initialisation désactive définitivement la surimpression (journalisée) ;
// le jeu reçoit alors son xrEndFrame d'origine, strictement inchangé.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>
#include <d3d12.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr_platform.h>

#include <cmath>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "emulate.h"
#include "hand_renderer.h"
#include "hands_overlay.h"

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
        try { Logger::Get().Log(LogLevel::Error, "exception dans la surimpression (%s) : %s", what, e.what()); } catch (...) {}
    } catch (...) {
        try { Logger::Get().Log(LogLevel::Error, "exception inconnue dans la surimpression (%s)", what); } catch (...) {}
    }
}

ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;

struct EyeChain {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<ID3D12Resource*> images;
    uint32_t width = 0, height = 0;
};

struct OverlayState {
    bool initTried = false;
    bool disabled = false;     // échec définitif : la surimpression ne sera plus tentée
    int failures = 0;
    EyeChain eye[2];
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::unique_ptr<HandRenderer> renderer;
    XrSession session = XR_NULL_HANDLE;
    uint64_t framesDrawn = 0;
    uint64_t framesSkipped = 0;
    ULONGLONG lastStatsTick = 0;
};
OverlayState g_ov;

void DestroyChains() {
    if (g_ov.renderer) g_ov.renderer->WaitIdle();
    for (EyeChain& e : g_ov.eye) {
        if (e.swapchain && g_dispatch.DestroySwapchain) g_dispatch.DestroySwapchain(e.swapchain);
        e = EyeChain{};
    }
}

void Disable(const char* why) {
    Logger::Get().Log(LogLevel::Error, "surimpression des mains DESACTIVEE : %s", why);
    g_ov.disabled = true;
    DestroyChains();
}

DXGI_FORMAT ChooseFormat(XrSession session) {
    uint32_t n = 0;
    if (XR_FAILED(g_dispatch.EnumerateSwapchainFormats(session, 0, &n, nullptr)) || n == 0) return DXGI_FORMAT_UNKNOWN;
    std::vector<int64_t> formats(n);
    if (XR_FAILED(g_dispatch.EnumerateSwapchainFormats(session, n, &n, formats.data()))) return DXGI_FORMAT_UNKNOWN;
    std::string list;
    for (int64_t f : formats) list += std::to_string(f) + " ";
    Logger::Get().Log(LogLevel::Info, "surimpression : formats de swapchain proposes : %s", list.c_str());
    // Préférence : couleur 8 bits par canal, sRGB d'abord (mélange correct).
    const DXGI_FORMAT prefs[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 DXGI_FORMAT_B8G8R8A8_UNORM};
    for (DXGI_FORMAT p : prefs)
        for (int64_t f : formats)
            if (f == static_cast<int64_t>(p)) return p;
    return DXGI_FORMAT_UNKNOWN;
}

bool CreateChain(XrSession session, EyeChain& e, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = static_cast<int64_t>(fmt);
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XrResult r = g_dispatch.CreateSwapchain(session, &ci, &e.swapchain);
    if (XR_FAILED(r)) {
        Logger::Get().Log(LogLevel::Error, "surimpression : xrCreateSwapchain a echoue (code %d)", static_cast<int>(r));
        e.swapchain = XR_NULL_HANDLE;
        return false;
    }
    uint32_t n = 0;
    r = g_dispatch.EnumerateSwapchainImages(e.swapchain, 0, &n, nullptr);
    if (XR_FAILED(r) || n == 0) return false;
    std::vector<XrSwapchainImageD3D12KHR> imgs(n, XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    r = g_dispatch.EnumerateSwapchainImages(e.swapchain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    if (XR_FAILED(r)) return false;
    e.images.clear();
    for (const auto& im : imgs) e.images.push_back(im.texture);
    e.width = w;
    e.height = h;
    return true;
}

// Cherche la couche « projection » du jeu (la vue du monde en deux yeux).
const XrCompositionLayerProjection* FindGameProjection(const XrFrameEndInfo* info) {
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* l = info->layers[i];
        if (l && l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(l);
            if (p->viewCount == 2 && p->views) return p;
        }
    }
    return nullptr;
}

bool EnsureResources(XrSession session, const XrCompositionLayerProjection* game, uint32_t w[2], uint32_t h[2]) {
    if (g_ov.disabled) return false;
    if (!g_ov.renderer) {
        if (g_ov.initTried) return false;
        g_ov.initTried = true;
        if (!g_device || !g_queue) { Disable("liaison Direct3D 12 du jeu introuvable (le jeu n'utilise pas D3D12 ?)"); return false; }
        const Dispatch& d = g_dispatch;
        if (!d.EndFrame || !d.EnumerateSwapchainFormats || !d.CreateSwapchain || !d.DestroySwapchain || !d.EnumerateSwapchainImages ||
            !d.AcquireSwapchainImage || !d.WaitSwapchainImage || !d.ReleaseSwapchainImage) {
            Disable("fonctions de swapchain manquantes dans le runtime");
            return false;
        }
        g_ov.format = ChooseFormat(session);
        if (g_ov.format == DXGI_FORMAT_UNKNOWN) { Disable("aucun format de swapchain compatible"); return false; }
        auto renderer = std::make_unique<HandRenderer>();
        const char* err = nullptr;
        if (!renderer->Init(g_device, g_queue, g_ov.format, &err)) {
            Disable((std::string("initialisation du rendu : ") + (err ? err : "?")).c_str());
            return false;
        }
        g_ov.renderer = std::move(renderer);
        g_ov.session = session;
        Logger::Get().Log(LogLevel::Info, "surimpression des mains : rendu Direct3D 12 pret (format DXGI %d)", static_cast<int>(g_ov.format));
    }
    (void)game;
    // (Re)création des swapchains si la taille a changé.
    for (int i = 0; i < 2; ++i) {
        if (g_ov.eye[i].swapchain && g_ov.eye[i].width == w[i] && g_ov.eye[i].height == h[i]) continue;
        DestroyChains();
        for (int j = 0; j < 2; ++j) {
            if (!CreateChain(session, g_ov.eye[j], w[j], h[j], g_ov.format)) {
                Disable("creation des swapchains impossible");
                return false;
            }
        }
        Logger::Get().Log(LogLevel::Info, "surimpression : swapchains %ux%u (gauche) et %ux%u (droit)", w[0], h[0], w[1], h[1]);
        break;
    }
    return true;
}

void ToHandsFrame(const XrCompositionLayerProjection* game, XrTime time, HandsFrame* frame, bool* anyHand) {
    *anyHand = false;
    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
    for (int h = 0; h < 2; ++h) {
        std::memset(joints, 0, sizeof(joints));
        bool pressed = false;
        int clickJoint = 10;
        HandPoseData& hd = frame->hand[h];
        hd = HandPoseData{};
        if (!Emu_LocateForRender(h, game->space, time, joints, &pressed, &clickJoint)) continue;
        int validCount = 0;
        for (int j = 0; j < kHandJointCount; ++j) {
            const bool ok = (joints[j].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
            hd.jointValid[j] = ok;
            hd.radius[j] = joints[j].radius;
            const XrPosef& p = joints[j].pose;
            hd.joint[j] = {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}};
            if (ok) ++validCount;
        }
        hd.valid = validCount >= 10;  // assez d'articulations pour que le dessin ait un sens
        hd.pressed = pressed;
        hd.clickJoint = clickJoint;
        if (hd.valid) *anyHand = true;
    }
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrEndFrame(XrSession session, const XrFrameEndInfo* info) {
    // 1) Préparation (protégée) : dessine les mains, remplit `ourLayer` si tout s'est bien passé.
    XrCompositionLayerProjection ourLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView ourViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    bool addLayer = false;
    bool swapchainAcquired[2] = {false, false};

    Safe("xrEndFrame(preparation)", [&] {
        const auto cfg = CurrentConfig();
        if (!cfg->handsEnabled || g_ov.disabled || !info || info->layerCount == 0 || !info->layers) return;
        const XrCompositionLayerProjection* game = FindGameProjection(info);
        if (!game) return;

        HandsFrame frame;
        bool any = false;
        ToHandsFrame(game, info->displayTime, &frame, &any);
        if (!any) { ++g_ov.framesSkipped; return; }  // aucune main visible : pas de couche, pas de coût GPU

        uint32_t w[2], h[2];
        for (int i = 0; i < 2; ++i) {
            const XrExtent2Di& ext = game->views[i].subImage.imageRect.extent;
            w[i] = std::max<uint32_t>(64, static_cast<uint32_t>(std::lround(ext.width * cfg->handsRenderScale)));
            h[i] = std::max<uint32_t>(64, static_cast<uint32_t>(std::lround(ext.height * cfg->handsRenderScale)));
        }
        if (!EnsureResources(session, game, w, h)) return;

        // Acquisition des images (une par œil)
        uint32_t index[2] = {0, 0};
        bool ok = true;
        for (int i = 0; i < 2 && ok; ++i) {
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (XR_FAILED(g_dispatch.AcquireSwapchainImage(g_ov.eye[i].swapchain, &ai, &index[i]))) { ok = false; break; }
            swapchainAcquired[i] = true;
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            if (XR_FAILED(g_dispatch.WaitSwapchainImage(g_ov.eye[i].swapchain, &wi))) ok = false;
            if (index[i] >= g_ov.eye[i].images.size()) ok = false;
        }
        if (ok) {
            RenderTarget targets[2];
            for (int i = 0; i < 2; ++i) {
                targets[i].resource = g_ov.eye[i].images[index[i]];
                targets[i].width = g_ov.eye[i].width;
                targets[i].height = g_ov.eye[i].height;
                const XrPosef& p = game->views[i].pose;
                targets[i].view.pose = {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
                                        {p.position.x, p.position.y, p.position.z}};
                targets[i].view.angleLeft = game->views[i].fov.angleLeft;
                targets[i].view.angleRight = game->views[i].fov.angleRight;
                targets[i].view.angleUp = game->views[i].fov.angleUp;
                targets[i].view.angleDown = game->views[i].fov.angleDown;
            }
            RenderStyle style;
            style.jointScale = static_cast<float>(cfg->handsJointScale);
            style.boneThickness = static_cast<float>(cfg->handsBoneMm / 1000.0);
            if (!g_ov.renderer->Render(targets, 2, frame, style)) {
                Logger::Get().Log(LogLevel::Error, "surimpression : echec du rendu : %s", g_ov.renderer->LastError());
                ok = false;
                if (++g_ov.failures >= 3) Disable("trop d'echecs de rendu consecutifs");
            }
        }
        // Libération : TOUJOURS, même après un échec, sinon la swapchain reste bloquée.
        for (int i = 0; i < 2; ++i) {
            if (!swapchainAcquired[i]) continue;
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            g_dispatch.ReleaseSwapchainImage(g_ov.eye[i].swapchain, &ri);
            swapchainAcquired[i] = false;
        }
        if (!ok) return;

        for (int i = 0; i < 2; ++i) {
            ourViews[i].pose = game->views[i].pose;
            ourViews[i].fov = game->views[i].fov;
            ourViews[i].subImage.swapchain = g_ov.eye[i].swapchain;
            ourViews[i].subImage.imageRect = {{0, 0}, {static_cast<int32_t>(g_ov.eye[i].width), static_cast<int32_t>(g_ov.eye[i].height)}};
            ourViews[i].subImage.imageArrayIndex = 0;
        }
        ourLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // alpha prémultiplié
        ourLayer.space = game->space;
        ourLayer.viewCount = 2;
        ourLayer.views = ourViews;
        addLayer = true;
        ++g_ov.framesDrawn;
        g_ov.failures = 0;

        const ULONGLONG now = GetTickCount64();
        if (g_ov.lastStatsTick == 0) g_ov.lastStatsTick = now;
        if (now - g_ov.lastStatsTick > 30000) {  // un point toutes les 30 s
            Logger::Get().Log(LogLevel::Info, "surimpression : %llu images dessinees, %llu sans main visible",
                              static_cast<unsigned long long>(g_ov.framesDrawn), static_cast<unsigned long long>(g_ov.framesSkipped));
            g_ov.lastStatsTick = now;
        }
    });

    // 2) L'appel réel, HORS de tout try. Sans couche ajoutée : l'info d'origine, intacte.
    if (!addLayer) return g_dispatch.EndFrame(session, info);

    std::vector<const XrCompositionLayerBaseHeader*> layers(info->layers, info->layers + info->layerCount);
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ourLayer));
    XrFrameEndInfo modified = *info;
    modified.layerCount = static_cast<uint32_t>(layers.size());
    modified.layers = layers.data();
    const XrResult r = g_dispatch.EndFrame(session, &modified);
    if (XR_FAILED(r)) {
        // Notre couche est peut-être en cause : on la désactive et on ne retente pas dans la même image.
        Safe("xrEndFrame(echec)", [&] {
            Logger::Get().Log(LogLevel::Error, "surimpression : xrEndFrame a refuse la couche des mains (code %d)", static_cast<int>(r));
            if (++g_ov.failures >= 3) Disable("xrEndFrame refuse la couche ajoutee");
        });
    }
    return r;
}

struct HookEntry {
    const char* name;
    PFN_xrVoidFunction fn;
};
const HookEntry kHooks[] = {{"xrEndFrame", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrEndFrame)}};

}  // namespace

void Overlay_OnGraphicsBinding(void* device, void* queue) {
    g_device = static_cast<ID3D12Device*>(device);
    g_queue = static_cast<ID3D12CommandQueue*>(queue);
}

void Overlay_Stop() {
    Safe("Overlay_Stop", [&] {
        if (g_ov.renderer) g_ov.renderer->WaitIdle();
        DestroyChains();
        g_ov.renderer.reset();
        g_ov = OverlayState{};
        g_device = nullptr;
        g_queue = nullptr;
    });
}

PFN_xrVoidFunction FindOverlayHook(const char* name) {
    const Dispatch& d = g_dispatch;
    if (!d.EndFrame) return nullptr;
    for (const HookEntry& h : kHooks)
        if (std::strcmp(h.name, name) == 0) return h.fn;
    return nullptr;
}

}  // namespace layer
