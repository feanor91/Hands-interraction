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
#include <filesystem>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

#include "depth_reader.h"
#include "emulation/Pinch.h"
#include "emulate.h"
#include "hand_renderer.h"
#include "hands_overlay.h"
#include "vr_menu.h"
#include "zones.h"
#include "mouse_click.h"

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

// Pincement lu directement sur les articulations, pour le mode « mains seules » : le calcul du pincement de l'emulation de manette ne tourne pas quand
// [emulation] enabled = false, or le geste d'ouverture du menu (deux mains pincees) et la saisie des rotatifs en ont besoin. Meme regle que le clic :
// distance du bout du pouce au bout de l'index ou du majeur (le plus proche), seuils click_on_mm / click_off_mm avec hysteresis.
bool PinchFromJoints(int hand, const XrHandJointLocationEXT* joints) {
    static hands::emulation::PinchDetector det[2];
    const auto cfg = CurrentConfig();
    auto ok = [&](int j) { return (joints[j].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0; };
    auto dist = [&](int a, int b) {
        const XrVector3f& pa = joints[a].pose.position;
        const XrVector3f& pb = joints[b].pose.position;
        const float dx = pa.x - pb.x, dy = pa.y - pb.y, dz = pa.z - pb.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    if (!ok(5) || !ok(10)) {
        det[hand].Reset();
        return false;
    }
    float d = dist(5, 10);
    // Le majeur ne compte que s'il est tendu : main qui pointe, le majeur replie touche le pouce en permanence et simulerait un pincement
    // (apprentissage d'une zone declenche tout seul). Tendu = bout plus loin du poignet que l'articulation du milieu du doigt.
    if (ok(15) && ok(13) && ok(1) && dist(1, 15) > dist(1, 13) * 1.05f) d = std::min(d, dist(5, 15));
    return det[hand].Update(d, static_cast<float>(cfg->clickOnMm / 1000.0), static_cast<float>(cfg->clickOffMm / 1000.0));
}

void ToHandsFrame(const XrCompositionLayerProjection* game, XrTime time, HandsFrame* frame, bool* anyHand) {
    *anyHand = false;
    {
        XrPosef hp;
        frame->gameSpace = reinterpret_cast<void*>(game->space);
        frame->headValid = Emu_HeadPose(game->space, time, &hp);
        if (frame->headValid) frame->head = {{hp.orientation.x, hp.orientation.y, hp.orientation.z, hp.orientation.w}, {hp.position.x, hp.position.y, hp.position.z}};
    }
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
        hd.pressed = CurrentConfig()->emulationEnabled ? pressed : PinchFromJoints(h, joints);
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
    uint64_t acquireCount = 0;            // nombre d'images prises par le jeu : 0 = swapchain créée mais jamais utilisée
};
std::unordered_map<uint64_t, SwapchainInfo> g_gameSwapchains;  // XrSwapchain -> infos (uniquement celles du jeu)
std::vector<uint64_t> g_depthChains;  // swapchains de profondeur du jeu, dans l'ordre de création (œil gauche puis droit)
std::vector<uint64_t> g_colorChains;  // swapchains couleur du jeu, dans l ordre de creation (oeil gauche puis droit)
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
            else if (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) g_colorChains.push_back(reinterpret_cast<uint64_t>(*sc));
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
        if (it != g_gameSwapchains.end()) {
            it->second.acquired = static_cast<int>(*index);
            ++it->second.acquireCount;
        }
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
std::unique_ptr<DepthReader> g_colorReader;  // lecteur des captures couleur (meme classe, autre format)
bool g_colorReaderFailed = false;

XRAPI_ATTR XrResult XRAPI_CALL Hook_xrDestroySwapchain(XrSwapchain sc) {
    Safe("xrDestroySwapchain", [&] {
        if (g_colorReader) g_colorReader->WaitIdle();
        if (g_depthReader) g_depthReader->WaitIdle();  // une copie en cours pourrait lire une image sur le point d'être libérée
        std::lock_guard<std::mutex> lock(g_swapMutex);
        const uint64_t h = reinterpret_cast<uint64_t>(sc);
        g_gameSwapchains.erase(h);
        g_depthChains.erase(std::remove(g_depthChains.begin(), g_depthChains.end(), h), g_depthChains.end());
        g_colorChains.erase(std::remove(g_colorChains.begin(), g_colorChains.end(), h), g_colorChains.end());
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
    if (!depth[0] && !depth[1]) {
        // Rien à lire : on journalise l'état des swapchains (rarement) pour savoir si le jeu s'en sert.
        if (g_probeCount++ % 20 == 0) {
            std::lock_guard<std::mutex> lock(g_swapMutex);
            for (const auto& kv : g_gameSwapchains) {
                const SwapchainInfo& s = kv.second;
                Logger::Get().Log(LogLevel::Info, "sonde de profondeur : rien a lire. swapchain %s : %zu images, prises par le jeu %llu fois, derniere terminee = %d",
                                  (s.usage & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? "PROFONDEUR" : "couleur", s.images.size(),
                                  static_cast<unsigned long long>(s.acquireCount), s.lastReleased);
            }
        }
        return;
    }

    HandsFrame frame;
    bool any = false;
    ToHandsFrame(game, info->displayTime, &frame, &any);

    std::vector<DepthRequestEye> req(2);
    for (int e = 0; e < 2; ++e) {
        g_probeItems[e].clear();
        req[e].depth = depth[e];
        req[e].state = StateFromName(cfg->depthProbeState);
        req[e].format = DXGI_FORMAT_R32_TYPELESS;
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
    if (!g_depthReader->Submit(req))
        Logger::Get().Log(LogLevel::Warn, "sonde de profondeur : soumission refusee (%s)", g_depthReader->LastError());
}

// --- Captures du rendu du jeu autour de la main --------------------------------------------------
// But : voir ce que MSFS dessine (flèche au bout du doigt, rayon, point blanc d'impact) pour juger si on peut en tirer la distance à la
// commande. On copie un rectangle de l'image couleur de l'œil gauche, centré sur le bout de l'index, et on l'écrit en BMP.
int g_capturesDone = 0;
ULONGLONG g_lastCaptureTick = 0;
bool g_prevPressed[2] = {false, false};
struct PendingCapture {
    bool active = false;
    std::string tag;
    int cx = 0, cy = 0;
    uint32_t w = 0, h = 0;
    int x0 = 0, y0 = 0;                          // coin du rectangle copie, dans l'image complete
    int ox = -1, oy = -1, fx = -1, fy = -1;      // depart du rayon et point a 25 cm sur le rayon (pixels de l'image complete)
    int ix = -1, iy = -1;                        // bout de l'index
} g_pendingCapture;

bool WriteBmp(const std::filesystem::path& file, const std::vector<float>& words, uint32_t w, uint32_t h) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") != 0 || !f) return false;
    const uint32_t imgSize = w * h * 4, fileSize = 54 + imgSize, offset = 54, dibSize = 40;
    uint8_t header[54] = {'B', 'M'};
    std::memcpy(header + 2, &fileSize, 4);
    std::memcpy(header + 10, &offset, 4);
    std::memcpy(header + 14, &dibSize, 4);
    const int32_t sw = static_cast<int32_t>(w), sh = -static_cast<int32_t>(h);  // hauteur négative : lignes de haut en bas
    std::memcpy(header + 18, &sw, 4);
    std::memcpy(header + 22, &sh, 4);
    const uint16_t planes = 1, bpp = 32;
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &imgSize, 4);
    std::fwrite(header, 1, 54, f);
    std::vector<uint8_t> row(static_cast<size_t>(w) * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(&words[static_cast<size_t>(y) * w]);  // RGBA
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2];  // B
            row[x * 4 + 1] = src[x * 4 + 1];  // G
            row[x * 4 + 2] = src[x * 4 + 0];  // R
            row[x * 4 + 3] = 255;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

// Croix de repere dans une image RGBA (mots de 4 octets), en coordonnees de l image copiee. Ignore ce qui sort de l image.
void DrawCross(std::vector<float>& words, uint32_t w, uint32_t h, int cx, int cy, int arm, uint8_t r, uint8_t g, uint8_t b) {
    auto put = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= static_cast<int>(w) || y >= static_cast<int>(h)) return;
        uint8_t* p = reinterpret_cast<uint8_t*>(&words[static_cast<size_t>(y) * w + static_cast<size_t>(x)]);
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = 255;
    };
    for (int k = -arm; k <= arm; ++k)
        for (int t = -1; t <= 1; ++t) {
            put(cx + k, cy + t);
            put(cx + t, cy + k);
        }
}

D3D12_RESOURCE_STATES ColorStateFromName(const std::string& n) {
    if (n == "common") return D3D12_RESOURCE_STATE_COMMON;
    if (n == "shader") return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (n == "copy_source") return D3D12_RESOURCE_STATE_COPY_SOURCE;
    return D3D12_RESOURCE_STATE_RENDER_TARGET;
}

void CaptureFrame(const XrFrameEndInfo* info) {
    const auto cfg = CurrentConfig();
    if (cfg->captureMax <= 0 || g_capturesDone >= cfg->captureMax || g_colorReaderFailed || !info || !info->layers || !g_device || !g_queue) return;
    const XrCompositionLayerProjection* game = FindGameProjection(info);
    if (!game) return;
    if (!g_colorReader) {
        auto reader = std::make_unique<DepthReader>();
        const char* err = nullptr;
        if (!reader->Init(g_device, g_queue, &err)) {
            Logger::Get().Log(LogLevel::Error, "captures desactivees : %s", err ? err : "?");
            g_colorReaderFailed = true;
            return;
        }
        g_colorReader = std::move(reader);
    }

    if (g_pendingCapture.active) {  // une copie est en cours : on attend sa fin sans jamais bloquer le rendu
        std::vector<DepthResultEye> res;
        if (!g_colorReader->TryCollect(&res)) return;
        g_pendingCapture.active = false;
        if (!res.empty() && !res[0].boxes.empty() && res[0].boxes[0].size() == static_cast<size_t>(g_pendingCapture.w) * g_pendingCapture.h) {
            char name[96];
            std::snprintf(name, sizeof(name), "cap_%02d_%s.bmp", g_capturesDone + 1, g_pendingCapture.tag.c_str());
            const std::filesystem::path file = LayerBaseDir() / "logs" / "captures" / name;
            // Reperes : magenta = depart du rayon que NOUS donnons au jeu ; vert = point a 25 cm sur ce rayon ; jaune = bout de l'index.
            if (g_pendingCapture.ox >= 0) DrawCross(res[0].boxes[0], g_pendingCapture.w, g_pendingCapture.h, g_pendingCapture.ox - g_pendingCapture.x0, g_pendingCapture.oy - g_pendingCapture.y0, 14, 255, 0, 255);
            if (g_pendingCapture.fx >= 0) DrawCross(res[0].boxes[0], g_pendingCapture.w, g_pendingCapture.h, g_pendingCapture.fx - g_pendingCapture.x0, g_pendingCapture.fy - g_pendingCapture.y0, 10, 0, 255, 0);
            if (g_pendingCapture.ix >= 0) DrawCross(res[0].boxes[0], g_pendingCapture.w, g_pendingCapture.h, g_pendingCapture.ix - g_pendingCapture.x0, g_pendingCapture.iy - g_pendingCapture.y0, 8, 255, 255, 0);
            const bool ok = WriteBmp(file, res[0].boxes[0], g_pendingCapture.w, g_pendingCapture.h);
            Logger::Get().Log(LogLevel::Info, "capture %d (%s) : %s, rect (%d,%d) %ux%u ; depart du rayon (%d,%d) ; point a 25 cm (%d,%d) ; bout de l'index (%d,%d)", g_capturesDone + 1,
                              g_pendingCapture.tag.c_str(), ok ? name : "ECHEC d'ecriture", g_pendingCapture.x0, g_pendingCapture.y0, g_pendingCapture.w, g_pendingCapture.h,
                              g_pendingCapture.ox, g_pendingCapture.oy, g_pendingCapture.fx, g_pendingCapture.fy, g_pendingCapture.ix, g_pendingCapture.iy);
            ++g_capturesDone;
        }
        return;
    }

    HandsFrame frame;
    bool any = false;
    ToHandsFrame(game, info->displayTime, &frame, &any);
    if (!any) {
        g_prevPressed[0] = g_prevPressed[1] = false;
        return;
    }
    int hand = -1;
    bool clickEdge = false;
    for (int h = 0; h < 2; ++h) {
        const bool pressed = frame.hand[h].valid && frame.hand[h].pressed;
        if (pressed && !g_prevPressed[h]) {
            clickEdge = true;
            hand = h;
        }
        g_prevPressed[h] = pressed;
    }
    const ULONGLONG now = GetTickCount64();
    const bool periodic = now - g_lastCaptureTick >= static_cast<ULONGLONG>(cfg->captureIntervalMs);
    if (!clickEdge && !periodic) return;
    if (hand < 0) hand = frame.hand[1].valid ? 1 : 0;
    if (!frame.hand[hand].valid || !frame.hand[hand].jointValid[10]) return;

    ID3D12Resource* color = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_swapMutex);
        if (!g_colorChains.empty()) {
            const auto it = g_gameSwapchains.find(g_colorChains[0]);  // première swapchain couleur = œil gauche
            if (it != g_gameSwapchains.end() && it->second.lastReleased >= 0 && static_cast<size_t>(it->second.lastReleased) < it->second.images.size())
                color = it->second.images[it->second.lastReleased];
        }
    }
    if (!color) return;
    const D3D12_RESOURCE_DESC d = color->GetDesc();

    // Projection, dans l'image de l'œil gauche, du depart du rayon que nous donnons au jeu (et du bout de l'index)
    const XrPosef& vp = game->views[0].pose;
    const em::Pose eye{{vp.orientation.x, vp.orientation.y, vp.orientation.z, vp.orientation.w}, {vp.position.x, vp.position.y, vp.position.z}};
    const XrFovf& fov = game->views[0].fov;
    const float tl = std::tan(fov.angleLeft), tr = std::tan(fov.angleRight), tu = std::tan(fov.angleUp), td = std::tan(fov.angleDown);
    auto project = [&](const em::Vec3& p, int* ox, int* oy) {
        const em::Vec3 v = em::Rotate(em::Conjugate(eye.q), p - eye.p);
        const float z = -v.z;
        if (z < 0.05f) return false;
        *ox = static_cast<int>((v.x / z - tl) / (tr - tl) * static_cast<float>(d.Width));
        *oy = static_cast<int>((tu - v.y / z) / (tu - td) * static_cast<float>(d.Height));
        return true;
    };
    PendingCapture pc;
    pc.tag = clickEdge ? "clic" : "survol";
    int px = 0, py = 0;
    if (!project(frame.hand[hand].joint[10].p, &pc.ix, &pc.iy)) return;
    XrPosef aim;
    if (Emu_LastPointerPose(hand, &aim)) {
        const em::Vec3 o{aim.position.x, aim.position.y, aim.position.z};
        const em::Quat q{aim.orientation.x, aim.orientation.y, aim.orientation.z, aim.orientation.w};
        project(o, &pc.ox, &pc.oy);
        project(o + em::Rotate(q, em::Vec3{0.0f, 0.0f, -0.25f}), &pc.fx, &pc.fy);
    }
    px = pc.ox >= 0 ? pc.ox : pc.ix;
    py = pc.oy >= 0 ? pc.oy : pc.iy;
    const uint32_t cw = std::min<uint32_t>(960, static_cast<uint32_t>(d.Width)), ch = std::min<uint32_t>(720, d.Height);
    const int x0 = std::max(0, std::min(px - static_cast<int>(cw / 2), static_cast<int>(d.Width - cw)));
    const int y0 = std::max(0, std::min(py - static_cast<int>(ch / 2), static_cast<int>(d.Height - ch)));

    std::vector<DepthRequestEye> req(1);
    req[0].depth = color;
    req[0].state = ColorStateFromName(cfg->colorState);
    req[0].format = DXGI_FORMAT_R8G8B8A8_UNORM;
    req[0].boxes.push_back({static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), cw, ch});
    if (!g_colorReader->Submit(req)) return;
    g_lastCaptureTick = now;
    pc.active = true;
    pc.cx = px;
    pc.cy = py;
    pc.w = cw;
    pc.h = ch;
    pc.x0 = x0;
    pc.y0 = y0;
    g_pendingCapture = pc;
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
    XrCompositionLayerQuad menuQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};  // panneau du menu des reglages (si ouvert)
    bool addMenu = false;

    Safe("xrEndFrame(mains seules)", [&] { if (info) Emu_UpdateHandPresence(info->displayTime); });
    Safe("xrEndFrame(diagnostic)", [&] { LogLayerChain(info); });
    Safe("xrEndFrame(sonde profondeur)", [&] { DepthProbeFrame(info); });
    Safe("xrEndFrame(captures)", [&] { CaptureFrame(info); });
    Safe("xrEndFrame(menu)", [&] {
        if (!info || !info->layers || !g_device || !g_queue) return;
        const XrCompositionLayerProjection* game = FindGameProjection(info);
        if (!game) return;
        HandsFrame frame;
        bool any = false;
        ToHandsFrame(game, info->displayTime, &frame, &any);
        Zones_Frame(frame);
        Mouse_Frame(frame);
        addMenu = VrMenu_Frame(session, g_device, g_queue, game, info->displayTime, frame, &menuQuad);
    });
    Safe("xrEndFrame(preparation)", [&] {
        const auto cfg = CurrentConfig();
        if (g_ov.disabled || !info || info->layerCount == 0 || !info->layers) return;
        const XrCompositionLayerProjection* game = FindGameProjection(info);
        if (!game) return;

        HandsFrame frame;
        bool any = false;
        ToHandsFrame(game, info->displayTime, &frame, &any);
        if (!cfg->handsEnabled) {  // mains cachees : on ne dessine plus que les boites des zones et le curseur du menu
            frame.hand[0].valid = frame.hand[1].valid = false;
            any = false;
        }
        Zones_FillMarkers(&frame);
        Mouse_FillMarkers(&frame);
        VrMenu_FillBoxes(&frame);
        if (!any && frame.markerCount == 0 && frame.boxCount == 0) { ++g_ov.framesSkipped; return; }  // aucune main visible : pas de couche, pas de coût GPU

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
            style.mode = cfg->handsStyle == "glove" ? kHandGlove : (cfg->handsStyle == "bare" ? kHandBare : (cfg->handsStyle == "ghost" ? kHandGhost : kHandSkeleton));
            style.alpha = static_cast<float>(cfg->handsGhostAlpha);
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
    if (!addLayer && !addMenu) return g_dispatch.EndFrame(session, info);

    std::vector<const XrCompositionLayerBaseHeader*> layers(info->layers, info->layers + info->layerCount);
    // Ordre des couches = ordre de dessin : le menu d'abord, puis les mains par-dessus (on voit toujours ses doigts, le curseur et les boites).
    if (addMenu) layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuQuad));
    if (addLayer) layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ourLayer));
    XrFrameEndInfo modified = *info;
    modified.layerCount = static_cast<uint32_t>(layers.size());
    modified.layers = layers.data();
    const XrResult r = g_dispatch.EndFrame(session, &modified);
    if (XR_FAILED(r)) {
        // Notre couche est peut-être en cause : on la désactive et on ne retente pas dans la même image.
        Safe("xrEndFrame(echec)", [&] {
            if (addMenu) VrMenu_NoteEndFrameFailure();
            Logger::Get().Log(LogLevel::Error, "surimpression : xrEndFrame a refuse la couche des mains (code %d)", static_cast<int>(r));
            if (addLayer && ++g_ov.failures >= 3) Disable("xrEndFrame refuse la couche ajoutee");
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
        VrMenu_Stop();
        if (g_depthReader) g_depthReader->WaitIdle();
        g_depthReader.reset();
        if (g_colorReader) g_colorReader->WaitIdle();
        g_colorReader.reset();
        g_colorReaderFailed = false;
        g_pendingCapture.active = false;
        g_capturesDone = 0;
        g_lastCaptureTick = 0;
        g_depthReaderFailed = false;
        g_lastProbeTick = 0;
        g_probeCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_swapMutex);
            g_gameSwapchains.clear();
            g_depthChains.clear();
            g_colorChains.clear();
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
