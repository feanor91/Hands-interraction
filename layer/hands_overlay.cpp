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
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

#include "depth_reader.h"
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

// --- Diagnostics (étape « contact ») : que fournit le jeu en profondeur ? -------------------------
// Aucun effet sur le jeu : on journalise les swapchains créées par le jeu et le contenu de ses couches.
struct SwapchainInfo {
    int64_t format = 0;
    uint64_t usage = 0;
    uint32_t width = 0, height = 0, arraySize = 0;
    std::vector<ID3D12Resource*> images;  // images D3D12 de la swapchain (connues après xrEnumerateSwapchainImages)
    int acquired = -1;                    // dernier indice rendu par xrAcquireSwapchainImage
    int lastReleased = -1;                // indice de l'image que le jeu vient de terminer (la plus récente)
};
std::unordered_map<uint64_t, SwapchainInfo> g_gameSwapchains;  // XrSwapchain -> infos (uniquement celles du jeu)
std::vector<uint64_t> g_depthChains;  // swapchains de profondeur du jeu, dans l'ordre de création (œil gauche puis droit)
std::mutex g_swapMutex;

std::string UsageText(uint64_t u) {
    std::string t;
    if (u & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) t += "couleur ";
    if (u & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) t += "PROFONDEUR ";
    if (u & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) t += "UAV ";
    if (u & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) t += "copie-src ";
    if (u & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) t += "copie-dst ";
    if (u & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) t += "echantillonnable ";
    if (u & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) t += "format-modifiable ";
    return t.empty() ? "?" : t;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* ci, XrSwapchain* sc) {
    const XrResult r = g_dispatch.CreateSwapchain(session, ci, sc);
    Safe("xrCreateSwapchain", [&] {
        if (!ci || !sc || XR_FAILED(r)) return;
        {
            std::lock_guard<std::mutex> lock(g_swapMutex);
            SwapchainInfo si;
            si.format = ci->format;
            si.usage = static_cast<uint64_t>(ci->usageFlags);
            si.width = ci->width;
            si.height = ci->height;
            si.arraySize = ci->arraySize;
            g_gameSwapchains[reinterpret_cast<uint64_t>(*sc)] = si;
            if (ci->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) g_depthChains.push_back(reinterpret_cast<uint64_t>(*sc));
        }
        Logger::Get().Log(LogLevel::Info, "swapchain du jeu : format DXGI %lld, %ux%u, tableau %u, mips %u, echantillons %u, usage = %s",
                          static_cast<long long>(ci->format), ci->width, ci->height, ci->arraySize, ci->mipCount, ci->sampleCount,
                          UsageText(ci->usageFlags).c_str());
    });
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrEnumerateSwapchainImages(XrSwapchain sc, uint32_t capacity, uint32_t* count,
                                                              XrSwapchainImageBaseHeader* images) {
    const XrResult r = g_dispatch.EnumerateSwapchainImages(sc, capacity, count, images);
    Safe("xrEnumerateSwapchainImages", [&] {
        if (XR_FAILED(r) || !images || !count || *count == 0) return;
        SwapchainInfo si;
        {
            std::lock_guard<std::mutex> lock(g_swapMutex);
            const auto it = g_gameSwapchains.find(reinterpret_cast<uint64_t>(sc));
            if (it == g_gameSwapchains.end()) return;
            si = it->second;
            if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) {  // on garde toutes les images (pour la sonde de profondeur)
                it->second.images.clear();
                for (uint32_t i = 0; i < *count; ++i)
                    it->second.images.push_back(reinterpret_cast<const XrSwapchainImageD3D12KHR*>(
                                                    reinterpret_cast<const char*>(images) + i * sizeof(XrSwapchainImageD3D12KHR))
                                                    ->texture);
            }
        }
        if (images->type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) return;
        const auto* d3d = reinterpret_cast<const XrSwapchainImageD3D12KHR*>(images);
        if (!d3d->texture) return;
        const D3D12_RESOURCE_DESC d = d3d->texture->GetDesc();
        Logger::Get().Log(LogLevel::Info, "  images (%u) de la swapchain %s : ressource D3D12 format %d, %llux%u, tableau %u, drapeaux 0x%X",
                          *count, (si.usage & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? "PROFONDEUR" : "couleur",
                          static_cast<int>(d.Format), static_cast<unsigned long long>(d.Width), d.Height,
                          static_cast<unsigned>(d.DepthOrArraySize), static_cast<unsigned>(d.Flags));
    });
    return r;
}

// --- Suivi des images de profondeur du jeu : quelle image vient d'être terminée ? ---------------
XRAPI_ATTR XrResult XRAPI_CALL Hook_xrAcquireSwapchainImage(XrSwapchain sc, const XrSwapchainImageAcquireInfo* info, uint32_t* index) {
    const XrResult r = g_dispatch.AcquireSwapchainImage(sc, info, index);
    if (XR_SUCCEEDED(r) && index) {
        std::lock_guard<std::mutex> lock(g_swapMutex);
        const auto it = g_gameSwapchains.find(reinterpret_cast<uint64_t>(sc));
        if (it != g_gameSwapchains.end()) it->second.acquired = static_cast<int>(*index);
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrReleaseSwapchainImage(XrSwapchain sc, const XrSwapchainImageReleaseInfo* info) {
    const XrResult r = g_dispatch.ReleaseSwapchainImage(sc, info);
    if (XR_SUCCEEDED(r)) {
        std::lock_guard<std::mutex> lock(g_swapMutex);
        const auto it = g_gameSwapchains.find(reinterpret_cast<uint64_t>(sc));
        if (it != g_gameSwapchains.end()) it->second.lastReleased = it->second.acquired;
    }
    return r;
}

std::unique_ptr<DepthReader> g_depthReader;  // créé à la première sonde ; détruit avec la session
bool g_depthReaderFailed = false;

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrDestroySwapchain(XrSwapchain sc) {
    Safe("xrDestroySwapchain", [&] {
        if (g_depthReader) g_depthReader->WaitIdle();  // une copie en cours pourrait lire une image sur le point d'être libérée
        std::lock_guard<std::mutex> lock(g_swapMutex);
        const uint64_t h = reinterpret_cast<uint64_t>(sc);
        g_gameSwapchains.erase(h);
        g_depthChains.erase(std::remove(g_depthChains.begin(), g_depthChains.end(), h), g_depthChains.end());
    });
    return g_dispatch.DestroySwapchain(sc);
}

// --- Sonde de profondeur (lecture seule) ----------------------------------------------------------
// Toutes les `probe_interval_ms`, copie vers le CPU : une grille 8x8 par œil et, autour de chaque bout d'index et de
// majeur visible, une petite zone 5x5. Le tout est journalisé avec la distance réelle du doigt à l'œil : on pourra
// ainsi déduire comment le jeu encode la profondeur (plan proche/lointain, profondeur inversée, ...).
struct ProbeItem {
    int hand = 0, joint = 0, px = 0, py = 0;
    float z = 0.0f;  // distance du bout de doigt à l'œil, le long de l'axe de vue (mètres)
};
std::vector<ProbeItem> g_probeItems[2];
ULONGLONG g_lastProbeTick = 0;
int g_probeCount = 0;
constexpr int kGrid = 8;

D3D12_RESOURCE_STATES StateFromName(const std::string& n) {
    if (n == "depth_read") return D3D12_RESOURCE_STATE_DEPTH_READ;
    if (n == "common") return D3D12_RESOURCE_STATE_COMMON;
    if (n == "shader") return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (n == "copy_source") return D3D12_RESOURCE_STATE_COPY_SOURCE;
    return D3D12_RESOURCE_STATE_DEPTH_WRITE;
}

void ProbeLogResults(const std::vector<DepthResultEye>& res) {
    Logger& log = Logger::Get();
    ++g_probeCount;
    for (size_t eye = 0; eye < res.size() && eye < 2; ++eye) {
        const DepthResultEye& r = res[eye];
        if (r.boxes.size() < static_cast<size_t>(kGrid * kGrid)) continue;
        float mn = 1e30f, mx = -1e30f;
        int zeros = 0, total = 0;
        for (int i = 0; i < kGrid * kGrid; ++i)
            if (!r.boxes[i].empty()) {
                const float v = r.boxes[i][0];
                mn = std::min(mn, v);
                mx = std::max(mx, v);
                zeros += v == 0.0f;
                ++total;
            }
        log.Log(LogLevel::Info, "profondeur oeil %zu (%ux%u) : grille %dx%d min=%.6g max=%.6g zeros=%d/%d", eye, r.texWidth, r.texHeight,
                kGrid, kGrid, mn, mx, zeros, total);
        if (g_probeCount <= 4 || g_probeCount % 20 == 0) {  // la grille complète : les premières fois, puis de temps en temps
            std::string row;
            for (int j = 0; j < kGrid; ++j) {
                row.clear();
                for (int i = 0; i < kGrid; ++i) {
                    char b[24];
                    const auto& v = r.boxes[static_cast<size_t>(j * kGrid + i)];
                    std::snprintf(b, sizeof(b), "%.5g ", v.empty() ? -1.0f : v[0]);
                    row += b;
                }
                log.Log(LogLevel::Info, "  grille ligne %d : %s", j, row.c_str());
            }
        }
        for (size_t k = 0; k < g_probeItems[eye].size(); ++k) {
            const ProbeItem& it = g_probeItems[eye][k];
            const size_t box = static_cast<size_t>(kGrid * kGrid) + k;
            if (box >= r.boxes.size() || r.boxes[box].empty()) continue;
            const auto& v = r.boxes[box];
            float pmn = 1e30f, pmx = -1e30f;
            for (float x : v) {
                pmn = std::min(pmn, x);
                pmx = std::max(pmx, x);
            }
            log.Log(LogLevel::Info, "  doigt main %d articulation %d : z=%.4f m pixel (%d,%d) centre=%.6g min=%.6g max=%.6g", it.hand, it.joint,
                    it.z, it.px, it.py, v[v.size() / 2], pmn, pmx);
        }
    }
}

void DepthProbeFrame(const XrFrameEndInfo* info) {
    const auto cfg = CurrentConfig();
    if (!cfg->depthProbe || g_depthReaderFailed || !info || !info->layers || !g_device || !g_queue) return;
    const XrCompositionLayerProjection* game = FindGameProjection(info);
    if (!game) return;

    if (!g_depthReader) {
        auto reader = std::make_unique<DepthReader>();
        const char* err = nullptr;
        if (!reader->Init(g_device, g_queue, &err)) {
            Logger::Get().Log(LogLevel::Error, "sonde de profondeur desactivee : %s", err ? err : "?");
            g_depthReaderFailed = true;
            return;
        }
        g_depthReader = std::move(reader);
        Logger::Get().Log(LogLevel::Info, "sonde de profondeur : prete (une zone toutes les %d ms, etat suppose : %s)", cfg->depthProbeIntervalMs,
                          cfg->depthProbeState.c_str());
    }

    if (g_depthReader->Pending()) {  // la requête précédente n'est peut-être pas terminée : on ne bloque jamais
        std::vector<DepthResultEye> res;
        if (g_depthReader->TryCollect(&res)) ProbeLogResults(res);
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastProbeTick < static_cast<ULONGLONG>(cfg->depthProbeIntervalMs)) return;
    g_lastProbeTick = now;

    // Images de profondeur les plus récentes (une par œil, dans l'ordre de création des swapchains de profondeur)
    ID3D12Resource* depth[2] = {nullptr, nullptr};
    {
        std::lock_guard<std::mutex> lock(g_swapMutex);
        for (size_t e = 0; e < 2 && e < g_depthChains.size(); ++e) {
            const auto it = g_gameSwapchains.find(g_depthChains[e]);
            if (it == g_gameSwapchains.end()) continue;
            const SwapchainInfo& sc = it->second;
            if (sc.lastReleased >= 0 && static_cast<size_t>(sc.lastReleased) < sc.images.size()) depth[e] = sc.images[sc.lastReleased];
        }
    }
    if (!depth[0] && !depth[1]) return;

    HandsFrame frame;
    bool any = false;
    ToHandsFrame(game, info->displayTime, &frame, &any);

    std::vector<DepthRequestEye> req(2);
    for (int e = 0; e < 2; ++e) {
        g_probeItems[e].clear();
        req[e].depth = depth[e];
        if (!depth[e]) continue;
        const D3D12_RESOURCE_DESC d = depth[e]->GetDesc();
        for (int j = 0; j < kGrid; ++j)
            for (int i = 0; i < kGrid; ++i)
                req[e].boxes.push_back({static_cast<uint32_t>((i + 0.5) / kGrid * d.Width), static_cast<uint32_t>((j + 0.5) / kGrid * d.Height), 1, 1});
        // Bouts de doigts projetés dans l'image de cet œil
        const XrPosef& vp = game->views[e].pose;
        const em::Pose eye{{vp.orientation.x, vp.orientation.y, vp.orientation.z, vp.orientation.w}, {vp.position.x, vp.position.y, vp.position.z}};
        const XrFovf& fov = game->views[e].fov;
        const float tl = std::tan(fov.angleLeft), tr = std::tan(fov.angleRight), tu = std::tan(fov.angleUp), td = std::tan(fov.angleDown);
        for (int h = 0; h < 2; ++h) {
            if (!frame.hand[h].valid) continue;
            for (int joint : {10, 15}) {  // bout de l'index, bout du majeur
                if (!frame.hand[h].jointValid[joint]) continue;
                const em::Vec3 v = em::Rotate(em::Conjugate(eye.q), frame.hand[h].joint[joint].p - eye.p);
                const float z = -v.z;
                if (z < 0.05f) continue;
                const float u = (v.x / z - tl) / (tr - tl), w = (tu - v.y / z) / (tu - td);
                const int px = static_cast<int>(u * static_cast<float>(d.Width)), py = static_cast<int>(w * static_cast<float>(d.Height));
                if (px < 2 || py < 2 || px >= static_cast<int>(d.Width) - 2 || py >= static_cast<int>(d.Height) - 2) continue;
                req[e].boxes.push_back({static_cast<uint32_t>(px - 2), static_cast<uint32_t>(py - 2), 5, 5});
                g_probeItems[e].push_back({h, joint, px, py, z});
            }
        }
    }
    if (!g_depthReader->Submit(req, StateFromName(cfg->depthProbeState)))
        Logger::Get().Log(LogLevel::Warn, "sonde de profondeur : soumission refusee (%s)", g_depthReader->LastError());
}

// Journalise, pour les premières images, la liste des couches et ce qui est chaîné aux vues (profondeur ?).
int g_chainLogsLeft = 3;
void LogLayerChain(const XrFrameEndInfo* info) {
    if (g_chainLogsLeft <= 0 || !info || !info->layers) return;
    --g_chainLogsLeft;
    Logger& log = Logger::Get();
    log.Log(LogLevel::Info, "xrEndFrame : %u couche(s), mode de melange %d", info->layerCount, static_cast<int>(info->environmentBlendMode));
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* l = info->layers[i];
        if (!l) continue;
        log.Log(LogLevel::Info, "  couche %u : type %d, drapeaux 0x%llX", i, static_cast<int>(l->type),
                static_cast<unsigned long long>(l->layerFlags));
        if (l->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(l);
        for (uint32_t v = 0; v < p->viewCount && p->views; ++v) {
            const XrCompositionLayerProjectionView& view = p->views[v];
            const XrExtent2Di& e = view.subImage.imageRect.extent;
            log.Log(LogLevel::Info, "    vue %u : image %dx%d, fov (%.2f %.2f %.2f %.2f)", v, e.width, e.height, view.fov.angleLeft,
                    view.fov.angleRight, view.fov.angleUp, view.fov.angleDown);
            bool depth = false;
            for (const auto* n = static_cast<const XrBaseInStructure*>(view.next); n; n = n->next) {
                if (n->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                    const auto* d = reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(n);
                    depth = true;
                    log.Log(LogLevel::Info,
                            "      PROFONDEUR fournie : swapchain %p, rect %dx%d, minDepth %.3f maxDepth %.3f nearZ %.3f farZ %.3f",
                            reinterpret_cast<void*>(d->subImage.swapchain), d->subImage.imageRect.extent.width,
                            d->subImage.imageRect.extent.height, d->minDepth, d->maxDepth, d->nearZ, d->farZ);
                } else {
                    log.Log(LogLevel::Info, "      structure chainee a la vue : type %d", static_cast<int>(n->type));
                }
            }
            if (!depth) log.Log(LogLevel::Info, "      pas d'information de profondeur sur cette vue");
        }
    }
}

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrEndFrame(XrSession session, const XrFrameEndInfo* info) {
    // 1) Préparation (protégée) : dessine les mains, remplit `ourLayer` si tout s'est bien passé.
    XrCompositionLayerProjection ourLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView ourViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    bool addLayer = false;
    bool swapchainAcquired[2] = {false, false};

    Safe("xrEndFrame(diagnostic)", [&] { LogLayerChain(info); });
    Safe("xrEndFrame(sonde profondeur)", [&] { DepthProbeFrame(info); });
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
const HookEntry kHooks[] = {
    {"xrEndFrame", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrEndFrame)},
    {"xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateSwapchain)},
    {"xrEnumerateSwapchainImages", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrEnumerateSwapchainImages)},
    {"xrAcquireSwapchainImage", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrAcquireSwapchainImage)},
    {"xrReleaseSwapchainImage", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrReleaseSwapchainImage)},
    {"xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrDestroySwapchain)},
};

}  // namespace

void Overlay_OnGraphicsBinding(void* device, void* queue) {
    g_device = static_cast<ID3D12Device*>(device);
    g_queue = static_cast<ID3D12CommandQueue*>(queue);
}

void Overlay_Stop() {
    Safe("Overlay_Stop", [&] {
        if (g_depthReader) g_depthReader->WaitIdle();
        g_depthReader.reset();
        g_depthReaderFailed = false;
        g_lastProbeTick = 0;
        g_probeCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_swapMutex);
            g_gameSwapchains.clear();
            g_depthChains.clear();
        }
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
    if (!d.EndFrame || !d.CreateSwapchain || !d.EnumerateSwapchainImages || !d.AcquireSwapchainImage || !d.ReleaseSwapchainImage ||
        !d.DestroySwapchain)
        return nullptr;
    for (const HookEntry& h : kHooks)
        if (std::strcmp(h.name, name) == 0) return h.fn;
    return nullptr;
}

}  // namespace layer
