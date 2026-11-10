// vr_menu.cpp — voir vr_menu.h.
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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "emulate.h"
#include "zones.h"
#include "menu_render.h"
#include "texture_uploader.h"
#include "vr_menu.h"

namespace layer {

using hands::log::Logger;
using hands::log::LogLevel;
namespace em = hands::emulation;

namespace {

// Panneau au bout du rayon (manettes ou rayon des mains) : grand et loin. Sans rayon (mode « mains seules »), on le touche du doigt : plus petit et à portée de bras.
constexpr float kPanelWidthRay = 0.60f, kPanelDistRay = 0.85f;
constexpr float kPanelWidthTouch = 0.44f, kPanelDistTouch = 0.50f;
constexpr ULONGLONG kPokeDwellMs = 450;  // temps de contact du doigt avant le choix
constexpr size_t kMenuRows = 12;       // nombre de lignes du panneau, fixe
bool g_fine = false;                   // page des réglages fins affichée
enum class Page { Main, Zones, Commands, Zone, Display, Pointer };
Page g_page = Page::Main;
std::string g_zoneId;  // zone ouverte dans la page de detail
int g_cmdPage = 0;
size_t g_lastCmdCount = 0;
constexpr size_t kCmdPerPage = 8;

struct Row {
    std::wstring text;
    bool selectable = true;
    std::function<void()> action;
};

struct MenuState {
    bool disabled = false;
    bool open = false;
    bool armed = true;          // le geste d'ouverture ne se redéclenche qu'après relâchement des deux mains
    ULONGLONG bothSince = 0;    // début du pincement simultané des deux mains
    bool prevPressed[2] = {false, false};
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<ID3D12Resource*> images;
    uint32_t w = 0, h = 0;
    std::unique_ptr<TextureUploader> uploader;
    em::Pose quad;
    int hover = -1;
    bool dirty = true;
    std::wstring status;
    int failures = 0;
    size_t rowCount = 0;
    float panelW = kPanelWidthRay, panelDist = kPanelDistRay;
};
MenuState g;

// Écrit un réglage (relu aussitôt) et mémorise le message d'état affiché dans l'en-tête du menu.
void Set(const char* section, const char* key, const std::string& value, const std::wstring& what) {
    if (Layer_SetConfigValue(section, key, value)) {
        g.status = what;
        Logger::Get().Log(LogLevel::Info, "menu : %s.%s = %s", section, key, value.c_str());
    } else {
        g.status = L"ÉCHEC : hands.ini n'est pas modifiable";
        Logger::Get().Log(LogLevel::Warn, "menu : ecriture de %s.%s impossible", section, key);
    }
    g.dirty = true;
}

template <size_t N>
int IndexOf(const char* const (&list)[N], const std::string& v) {
    for (size_t i = 0; i < N; ++i)
        if (v == list[i]) return static_cast<int>(i);
    return 0;
}

std::string Num(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.0f", v);
    return b;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void CloseMenu() {
    g.open = false;
    g_fine = false;
    g_page = Page::Main;
    Emu_SetMenuOpen(false);
    g.status.clear();
}

// Page « Zones » : zones existantes (les toucher = reapprendre leur position) et ajout d'une zone.
std::vector<Row> BuildZonesRows() {
    std::vector<Row> rows;
    rows.push_back({L"◂  Retour", true, [] { g_page = Page::Main; }});
    rows.push_back({L"＋  Ajouter une zone : choisir sa commande  ▸", true, [] {
                        g_page = Page::Commands;
                        g_cmdPage = 0;
                        if (!Zones_InputsLoaded()) Zones_RefreshInputs();
                    }});
    const std::vector<ZoneInfo> zones = Zones_List();
    for (size_t i = 0; i < zones.size() && rows.size() < kMenuRows - 2; ++i) {
        const ZoneInfo z = zones[i];
        rows.push_back({std::wstring(z.hasPos ? L"✔  " : L"…  ") + z.text + (z.hasPos ? L"" : L"   (à apprendre)"), true, [z] {
                            g_zoneId = z.id;
                            g_page = Page::Zone;
                        }});
    }
    if (zones.empty()) rows.push_back({L"Aucune zone pour l'instant.", false, nullptr});
    rows.push_back({L"Toucher une zone : taille, forme, nouvel apprentissage", false, nullptr});
    while (rows.size() + 1 < kMenuRows) rows.push_back({L"", false, nullptr});
    rows.push_back({L"Fermer le menu", true, [] { CloseMenu(); }});
    return rows;
}

// Page « une zone » : taille et forme de sa boite, nouvel apprentissage, suppression.
std::vector<Row> BuildZoneRows() {
    std::vector<Row> rows;
    const std::string id = g_zoneId;
    rows.push_back({L"◂  Retour aux zones", true, [] { g_page = Page::Zones; }});
    rows.push_back({Wide(id) + L"  —  boîte " + Zones_GeomText(id), false, nullptr});
    rows.push_back({L"Boîte plus grande  (+25 %)", true, [id] { Zones_Scale(id, 1.25); }});
    rows.push_back({L"Boîte plus petite  (−20 %)", true, [id] { Zones_Scale(id, 0.8); }});
    rows.push_back({L"Forme suivante (bouton, interrupteur, molette, écran…)", true, [id] { Zones_NextShape(id); }});
    rows.push_back({L"Réapprendre la position  (compte à rebours 3 s)", true, [id] {
                        CloseMenu();
                        Zones_Reteach(id);
                    }});
    rows.push_back({L"Supprimer cette zone", true, [id] {
                        Zones_Delete(id);
                        g_page = Page::Zones;
                    }});
    while (rows.size() + 1 < kMenuRows) rows.push_back({L"", false, nullptr});
    rows.push_back({L"Fermer le menu", true, [] { CloseMenu(); }});
    return rows;
}

// Page « Commandes » : bibliotheque du Rafale puis Input Events de l'avion, par pages ; choisir = creer la zone et lancer son apprentissage.
std::vector<Row> BuildCommandRows() {
    std::vector<Row> rows;
    const size_t n = Zones_CommandCount();
    const size_t pages = std::max<size_t>(1, (n + kCmdPerPage - 1) / kCmdPerPage);
    if (static_cast<size_t>(g_cmdPage) >= pages) g_cmdPage = static_cast<int>(pages) - 1;
    rows.push_back({L"◂  Retour aux zones", true, [] { g_page = Page::Zones; }});
    for (size_t k = 0; k < kCmdPerPage; ++k) {
        const size_t idx = static_cast<size_t>(g_cmdPage) * kCmdPerPage + k;
        if (idx >= n) {
            rows.push_back({L"", false, nullptr});
            continue;
        }
        rows.push_back({Zones_CommandLabel(idx), true, [idx] {
                            Zones_AddCommand(idx);
                            CloseMenu();
                        }});
    }
    wchar_t b[64];
    swprintf(b, 64, L"◂  Page précédente  (%d / %zu)", g_cmdPage + 1, pages);
    rows.push_back({b, g_cmdPage > 0, [] { --g_cmdPage; }});
    rows.push_back({L"Page suivante  ▸", static_cast<size_t>(g_cmdPage) + 1 < pages, [] { ++g_cmdPage; }});
    if (Zones_InputsLoaded()) {
        swprintf(b, 64, L"%zu commandes (avion lu)", n);
        rows.push_back({b, false, nullptr});
    } else {
        rows.push_back({L"Lire aussi les commandes de l'avion (MSFS lancé)", true, [] { Zones_RefreshInputs(); }});
    }
    return rows;
}

std::vector<Row> BuildPointerRows() {
    const auto cfg = CurrentConfig();
    std::vector<Row> rows;
    rows.push_back({L"◂  Retour", true, [] { g_page = Page::Main; }});

    // Rayon
    static const char* const rayModes[] = {"shoulder", "contact", "palm", "hand", "finger"};
    static const wchar_t* const rayNames[] = {L"Comme le casque (épaule)", L"Par le point de contact (poignet)", L"Depuis la paume", L"Axe de la main", L"Bout du doigt"};
    {
        const int i = IndexOf(rayModes, cfg->pointerMode);
        rows.push_back({std::wstring(L"Rayon : ") + rayNames[i], true, [i] {
                            const int n = (i + 1) % 5;
                            Set("emulation", "pointer_mode", rayModes[n], std::wstring(L"Rayon : ") + rayNames[n]);
                        }});
    }
    // Doigt du pointeur
    {
        const bool middle = cfg->pointerFinger == "middle";
        rows.push_back({std::wstring(L"Doigt du pointeur : ") + (middle ? L"Majeur" : L"Index"), true, [middle] {
                            Set("emulation", "pointer_finger", middle ? "index" : "middle", middle ? L"Pointeur sur l'index" : L"Pointeur sur le majeur");
                            if (!middle) Set("emulation", "click_mode", "any", L"Pointeur sur le majeur ; clic pouce sur index ou majeur");
                        }});
    }
    // Geste de clic
    static const char* const clickModes[] = {"any", "side", "pinch", "middle"};
    static const wchar_t* const clickNames[] = {L"Pouce sur l'index ou le majeur (au plus proche)", L"Pouce sur le côté de l'index", L"Pouce sur le bout de l'index", L"Pouce sur le bout du majeur"};
    {
        const int i = IndexOf(clickModes, cfg->clickMode);
        rows.push_back({std::wstring(L"Clic : ") + clickNames[i], true, [i] {
                            const int n = (i + 1) % 4;
                            Set("emulation", "click_mode", clickModes[n], std::wstring(L"Clic : ") + clickNames[n]);
                        }});
    }
    // Relâchement du clic (préréglages)
    {
        auto approx = [](double a, double b) { return std::fabs(a - b) < 0.5; };
        static const wchar_t* const names[] = {L"très court (19 / 21 mm)", L"court (20 / 23 mm)", L"normal (21 / 27 mm)", L"tolérant (22 / 35 mm)"};
        static const double on[] = {19, 20, 21, 22}, off[] = {21, 23, 27, 35};
        int cur = -1;
        for (int k = 0; k < 4; ++k)
            if (approx(cfg->clickOnMm, on[k]) && approx(cfg->clickOffMm, off[k])) cur = k;
        std::wstring label = L"Relâchement du clic : ";
        if (cur >= 0) label += names[cur];
        else {
            wchar_t b[64];
            swprintf(b, 64, L"personnalisé (%.0f / %.0f mm)", cfg->clickOnMm, cfg->clickOffMm);
            label += b;
        }
        rows.push_back({label, true, [cur] {
                            const int n = (cur + 1) % 4;
                            Set("emulation", "click_on_mm", Num(on[n]), std::wstring(L"Relâchement : ") + names[n]);
                            Set("emulation", "click_off_mm", Num(off[n]), std::wstring(L"Relâchement : ") + names[n]);
                        }});
    }
    // Départ du rayon (mode épaule)
    {
        const bool bone = cfg->anchorSource == "bone";
        rows.push_back({std::wstring(L"Départ du rayon : ") + (bone ? L"base du doigt (os)" : L"bout du doigt"), true, [bone] {
                            Set("emulation", "anchor_source", bone ? "fingertip" : "bone", bone ? L"Départ : bout du doigt" : L"Départ : base du doigt");
                        }});
    }
    // Mains verticales : le rayon suit la direction du doigt quand elle s'écarte de celle du bras
    rows.push_back({std::wstring(L"Rayon suit le doigt (main verticale) : ") + (cfg->fingerBlend ? L"OUI" : L"NON"), true, [on = cfg->fingerBlend] {
                        Set("emulation", "finger_blend", on ? "false" : "true", on ? L"Rayon suit le doigt : NON" : L"Rayon suit le doigt : OUI");
                    }});
    // Interrupteurs
    rows.push_back({std::wstring(L"Deux mains actives : ") + (cfg->sharedPointer ? L"OUI" : L"NON"), true, [on = cfg->sharedPointer] {
                        Set("emulation", "shared_pointer", on ? "false" : "true", on ? L"Deux mains actives : NON" : L"Deux mains actives : OUI");
                    }});
    if (!g_fine) {
        rows.push_back({L"Réglages fins du rayon  ▸", true, [] { g_fine = true; }});
    } else {
        rows.clear();
        rows.push_back({L"◂  Retour", true, [] { g_fine = false; }});
    }
    // Inclinaison du rayon
    if (g_fine) {
        wchar_t b[96];
        swprintf(b, 96, L"Inclinaison du rayon : %+.0f° haut/bas, %+.0f° gauche/droite", cfg->pointerPitchDeg, cfg->pointerYawDeg);
        rows.push_back({b, false, nullptr});
        const double pitch = cfg->pointerPitchDeg, yaw = cfg->pointerYawDeg;
        auto clamp = [](double v) { return v < -45.0 ? -45.0 : (v > 45.0 ? 45.0 : v); };
        rows.push_back({L"   Rayon plus haut (+2°)", true, [=] { Set("emulation", "pointer_pitch_deg", Num(clamp(pitch + 2)), L"Inclinaison : plus haut"); }});
        rows.push_back({L"   Rayon plus bas (−2°)", true, [=] { Set("emulation", "pointer_pitch_deg", Num(clamp(pitch - 2)), L"Inclinaison : plus bas"); }});
        rows.push_back({L"   Rayon plus à gauche (+2°)", true, [=] { Set("emulation", "pointer_yaw_deg", Num(clamp(yaw + 2)), L"Inclinaison : plus à gauche"); }});
        rows.push_back({L"   Rayon plus à droite (−2°)", true, [=] { Set("emulation", "pointer_yaw_deg", Num(clamp(yaw - 2)), L"Inclinaison : plus à droite"); }});
        rows.push_back({L"   Remettre l'inclinaison à zéro", true, [] {
                            Set("emulation", "pointer_pitch_deg", "0", L"Inclinaison remise à zéro");
                            Set("emulation", "pointer_yaw_deg", "0", L"Inclinaison remise à zéro");
                        }});
    }
    // Épaule estimée
    if (g_fine) {
        const double drop = cfg->shoulderDropMm;
        wchar_t b1[64], b2[64];
        swprintf(b1, 64, L"Épaule plus basse (+2 cm) — actuel %.0f cm", drop / 10.0);
        swprintf(b2, 64, L"Épaule plus haute (−2 cm)");
        rows.push_back({b1, true, [drop] { Set("emulation", "shoulder_drop_mm", Num(std::min(500.0, drop + 20)), L"Épaule plus basse"); }});
        rows.push_back({b2, true, [drop] { Set("emulation", "shoulder_drop_mm", Num(std::max(0.0, drop - 20)), L"Épaule plus haute"); }});
    }
    // Hauteur constante (le panneau est créé une fois) : on complète avec des lignes vides.
    while (rows.size() + 1 < kMenuRows) rows.push_back({L"", false, nullptr});
    rows.push_back({L"Fermer le menu", true, [] {
                        g.open = false;
                        g_fine = false;
                        Emu_SetMenuOpen(false);
                        g.status.clear();
                        Logger::Get().Log(LogLevel::Info, "menu : ferme");
                    }});
    return rows;
}

// Page d'accueil : trois sous-menus.
std::vector<Row> BuildMainRows() {
    std::vector<Row> rows;
    rows.push_back({L"Zones du pochoir et leurs commandes  ▸", true, [] { g_page = Page::Zones; }});
    rows.push_back({L"Affichage : mains et boîtes des zones  ▸", true, [] { g_page = Page::Display; }});
    rows.push_back({L"Rayon et clic (mode rayon aux mains)  ▸", true, [] { g_page = Page::Pointer; }});
    while (rows.size() + 1 < kMenuRows) rows.push_back({L"", false, nullptr});
    rows.push_back({L"Fermer le menu", true, [] { CloseMenu(); }});
    return rows;
}

// Affichage : aspect des mains (squelette, gant, mains nues, translucides) et boites des zones.
std::vector<Row> BuildDisplayRows() {
    const auto cfg = CurrentConfig();
    std::vector<Row> rows;
    rows.push_back({L"◂  Retour", true, [] { g_page = Page::Main; }});
    {
        const std::string m = cfg->handsStyle;
        const wchar_t* name = m == "glove" ? L"gants" : (m == "bare" ? L"mains nues" : (m == "ghost" ? L"mains translucides" : L"squelette"));
        rows.push_back({std::wstring(L"Mains : ") + name, true, [m] {
                            const char* n = m == "skeleton" ? "glove" : (m == "glove" ? "bare" : (m == "bare" ? "ghost" : "skeleton"));
                            const wchar_t* t = std::string(n) == "glove" ? L"Mains : gants" : (std::string(n) == "bare" ? L"Mains : mains nues" : (std::string(n) == "ghost" ? L"Mains : translucides" : L"Mains : squelette"));
                            Set("hands", "style", n, t);
                        }});
    }
    rows.push_back({std::wstring(L"Mains affichées : ") + (cfg->handsEnabled ? L"OUI" : L"NON"), true, [on = cfg->handsEnabled] {
                        Set("hands", "enabled", on ? "false" : "true", on ? L"Mains cachées" : L"Mains affichées");
                    }});
    {
        const std::string m = cfg->zoneShowBoxes;
        const wchar_t* name = m == "near" ? L"près des doigts" : (m == "hidden" ? L"cachées" : L"toutes");
        rows.push_back({std::wstring(L"Boîtes des zones : ") + name, true, [m] {
                            const char* n = m == "all" ? "near" : (m == "near" ? "hidden" : "all");
                            Set("zones", "show_boxes", n, L"Boîtes des zones modifiées");
                        }});
    }
    {
        const bool face = cfg->zoneBoxFaces == "face";
        rows.push_back({std::wstring(L"Forme des boîtes : ") + (face ? L"face avant seule (carré)" : L"boîte complète"), true, [face] {
                            Set("zones", "box_faces", face ? "full" : "face", face ? L"Boîtes complètes" : L"Faces avant seules");
                        }});
    }
    while (rows.size() + 1 < kMenuRows) rows.push_back({L"", false, nullptr});
    rows.push_back({L"Fermer le menu", true, [] { CloseMenu(); }});
    return rows;
}

std::vector<Row> BuildRows() {
    switch (g_page) {
        case Page::Zones: return BuildZonesRows();
        case Page::Commands: return BuildCommandRows();
        case Page::Zone: return BuildZoneRows();
        case Page::Display: return BuildDisplayRows();
        case Page::Pointer: return BuildPointerRows();
        default: return BuildMainRows();
    }
}

bool EnsureSwapchain(XrSession session, ID3D12Device* device, ID3D12CommandQueue* queue, size_t rowCount) {
    if (g.swapchain) return true;
    const Dispatch& d = g_dispatch;
    if (!d.CreateSwapchain || !d.EnumerateSwapchainFormats || !d.EnumerateSwapchainImages || !d.AcquireSwapchainImage || !d.WaitSwapchainImage ||
        !d.ReleaseSwapchainImage || !d.DestroySwapchain)
        return false;
    uint32_t n = 0;
    if (XR_FAILED(d.EnumerateSwapchainFormats(session, 0, &n, nullptr)) || n == 0) return false;
    std::vector<int64_t> formats(n);
    if (XR_FAILED(d.EnumerateSwapchainFormats(session, n, &n, formats.data()))) return false;
    int64_t fmt = 0;
    for (int64_t want : {static_cast<int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM), static_cast<int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)})
        for (int64_t f : formats)
            if (f == want && !fmt) fmt = f;
    if (!fmt) {
        Logger::Get().Log(LogLevel::Warn, "menu : aucun format B8G8R8A8 propose par le runtime");
        return false;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = kMenuWidth;
    ci.height = MenuHeight(rowCount);
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (XR_FAILED(d.CreateSwapchain(session, &ci, &g.swapchain))) {
        g.swapchain = XR_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    if (XR_FAILED(d.EnumerateSwapchainImages(g.swapchain, 0, &count, nullptr)) || count == 0) return false;
    std::vector<XrSwapchainImageD3D12KHR> imgs(count, XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    if (XR_FAILED(d.EnumerateSwapchainImages(g.swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())))) return false;
    g.images.clear();
    for (const auto& im : imgs) g.images.push_back(im.texture);
    g.w = ci.width;
    g.h = ci.height;
    g.uploader = std::make_unique<TextureUploader>();
    const char* err = nullptr;
    if (!g.uploader->Init(device, queue, &err)) {
        Logger::Get().Log(LogLevel::Error, "menu : envoi de texture impossible : %s", err ? err : "?");
        return false;
    }
    Logger::Get().Log(LogLevel::Info, "menu : panneau %ux%u pret (format DXGI %lld)", g.w, g.h, static_cast<long long>(fmt));
    return true;
}

void Disable(const char* why) {
    Logger::Get().Log(LogLevel::Error, "menu dans le casque DESACTIVE : %s", why);
    g.disabled = true;
    g.open = false;
    Emu_SetMenuOpen(false);
}

// Dessine le menu et l'envoie dans la prochaine image de la swapchain.
bool Redraw(const std::vector<Row>& rows) {
    std::vector<MenuRow> mr;
    mr.reserve(rows.size());
    for (const Row& r : rows) mr.push_back({r.text, r.selectable});
    std::vector<uint32_t> px;
    RenderMenuBitmap(L"Mains nues OpenXR — réglages",
                     g.status.empty() ? L"Visez et pincez, ou touchez une ligne du doigt · pincez les 2 mains pour fermer" : g.status, mr, g.hover, &px);
    const Dispatch& d = g_dispatch;
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(d.AcquireSwapchainImage(g.swapchain, &ai, &index))) return false;
    bool ok = false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_SUCCEEDED(d.WaitSwapchainImage(g.swapchain, &wi)) && index < g.images.size()) {
        ok = g.uploader->Upload(g.images[index], D3D12_RESOURCE_STATE_RENDER_TARGET, DXGI_FORMAT_B8G8R8A8_UNORM, px.data(), g.w, g.h, g.w * 4);
        if (!ok) Logger::Get().Log(LogLevel::Error, "menu : envoi de l'image echoue : %s", g.uploader->LastError());
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    d.ReleaseSwapchainImage(g.swapchain, &ri);  // toujours, sinon la swapchain reste bloquée
    return ok;
}

}  // namespace

bool VrMenu_Frame(XrSession session, ID3D12Device* device, ID3D12CommandQueue* queue, const XrCompositionLayerProjection* game, XrTime displayTime,
                  const HandsFrame& frame, XrCompositionLayerQuad* quad) {
    const auto cfg = CurrentConfig();
    if (g.disabled || !device || !queue || !game) return false;
    if (!cfg->menuEnabled) {
        if (g.open) {
            g.open = false;
            Emu_SetMenuOpen(false);
        }
        return false;
    }

    // --- Geste d'ouverture / fermeture : les deux mains pincées ensemble ---------------------------------------------
    const ULONGLONG now = GetTickCount64();
    const bool p[2] = {frame.hand[0].valid && frame.hand[0].pressed, frame.hand[1].valid && frame.hand[1].pressed};
    if (!p[0] && !p[1]) g.armed = true;
    if (p[0] && p[1] && g.armed) {
        if (g.bothSince == 0) g.bothSince = now;
        else if (now - g.bothSince >= static_cast<ULONGLONG>(cfg->menuHoldMs)) {
            g.bothSince = 0;
            g.armed = false;
            if (g.open) {
                g.open = false;
                Emu_SetMenuOpen(false);
                Logger::Get().Log(LogLevel::Info, "menu : ferme (geste)");
            } else {
                XrPosef head;
                g.panelW = cfg->emulationEnabled ? kPanelWidthRay : kPanelWidthTouch;
                g.panelDist = cfg->emulationEnabled ? kPanelDistRay : kPanelDistTouch;
                g_page = Page::Main;
                if (Emu_HeadPose(game->space, displayTime, &head)) {
                    const em::Pose hp{{head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w}, {head.position.x, head.position.y, head.position.z}};
                    const em::Vec3 fwd = em::Rotate(hp.q, em::Vec3{0.0f, 0.0f, -1.0f});
                    em::Vec3 fh = em::Normalized(em::Vec3{fwd.x, 0.0f, fwd.z});
                    if (em::Length(fh) < 0.5f) fh = em::Vec3{0.0f, 0.0f, -1.0f};
                    const em::Vec3 up{0.0f, 1.0f, 0.0f};
                    g.quad = {em::LookAlong(fh, up), hp.p + fh * g.panelDist - up * 0.05f};  // devant la tete, un peu plus bas
                    g.open = true;
                    g.dirty = true;
                    g.hover = -1;
                    g.status.clear();
                    Emu_SetMenuOpen(true);
                    Logger::Get().Log(LogLevel::Info, "menu : ouvert (geste des deux mains)");
                } else {
                    Logger::Get().Log(LogLevel::Warn, "menu : position de la tete indisponible, ouverture impossible");
                }
            }
        }
    } else if (!(p[0] && p[1])) {
        g.bothSince = 0;
    }
    const bool prev[2] = {g.prevPressed[0], g.prevPressed[1]};
    g.prevPressed[0] = p[0];
    g.prevPressed[1] = p[1];
    if (!g.open) return false;

    // --- Panneau et image ------------------------------------------------------------------------------------------
    if (g_page == Page::Commands && Zones_CommandCount() != g_lastCmdCount) {
        g_lastCmdCount = Zones_CommandCount();
        g.dirty = true;
    }
    std::vector<Row> rows = BuildRows();
    if (!EnsureSwapchain(session, device, queue, rows.size())) {
        Disable("creation du panneau impossible");
        return false;
    }
    const float panelH = g.panelW * static_cast<float>(g.h) / static_cast<float>(g.w);

    // --- Pointage : le rayon que le jeu recevrait, coupé avec le plan du panneau --------------------------------------
    int hoverRow = -1, hoverHand = -1;
    int rowOfHand[2] = {-1, -1};
    for (int h = 0; h < 2; ++h) {
        XrPosef aim;
        if (!Emu_LastPointerPose(h, &aim)) continue;
        const em::Vec3 o{aim.position.x, aim.position.y, aim.position.z};
        const em::Vec3 dir = em::Rotate(em::Quat{aim.orientation.x, aim.orientation.y, aim.orientation.z, aim.orientation.w}, em::Vec3{0.0f, 0.0f, -1.0f});
        float lx = 0, ly = 0, t = 0;
        if (!em::RayQuadHit(o, dir, g.quad, &lx, &ly, &t)) continue;
        const int x = static_cast<int>((lx / g.panelW + 0.5f) * static_cast<float>(g.w));
        const int y = static_cast<int>((0.5f - ly / panelH) * static_cast<float>(g.h));
        rowOfHand[h] = MenuRowAtPixel(x, y, rows.size());
    }
    for (int h : {1, 0}) {  // la main qui pince a priorité, sinon la droite
        if (rowOfHand[h] >= 0 && (hoverHand < 0 || p[h])) {
            hoverHand = h;
            hoverRow = rowOfHand[h];
        }
    }
    if (hoverRow >= 0 && !rows[static_cast<size_t>(hoverRow)].selectable) hoverRow = -1;

    // --- Toucher du doigt : le bout de l'index sur le panneau (ou presque), maintenu un instant, choisit la ligne --------------------
    {
        static ULONGLONG pokeSince[2] = {0, 0};
        static int pokeRow[2] = {-1, -1};
        static bool pokeLock[2] = {false, false};
        bool activated = false;
        for (int h = 0; h < 2; ++h) {
            const HandPoseData& hd = frame.hand[h];
            if (!(hd.valid && hd.jointValid[10])) {
                pokeRow[h] = -1;
                pokeLock[h] = false;
                continue;
            }
            const em::Vec3 loc = em::Rotate(em::Conjugate(g.quad.q), hd.joint[10].p - g.quad.p);  // z > 0 : devant le panneau
            if (loc.z > 0.07f) pokeLock[h] = false;      // le doigt s'est retire : nouveau choix possible
            int row = -1;
            if (!pokeLock[h] && loc.z < 0.03f && loc.z > -0.06f) {
                const int x = static_cast<int>((loc.x / g.panelW + 0.5f) * static_cast<float>(g.w));
                const int y = static_cast<int>((0.5f - loc.y / panelH) * static_cast<float>(g.h));
                row = MenuRowAtPixel(x, y, rows.size());
                if (row >= 0 && !rows[static_cast<size_t>(row)].selectable) row = -1;
            }
            if (row != pokeRow[h]) {
                pokeRow[h] = row;
                pokeSince[h] = now;
            } else if (row >= 0 && !activated && now - pokeSince[h] >= kPokeDwellMs) {
                activated = true;
                pokeLock[h] = true;
                pokeRow[h] = -1;
                Row& r = rows[static_cast<size_t>(row)];
                if (r.action) {
                    r.action();
                    g.dirty = true;
                }
            }
            if (pokeRow[h] >= 0 && hoverRow < 0) hoverRow = pokeRow[h];
        }
        if (activated && !g.open) return false;
        if (activated) {
            g.hover = -1;
            rows = BuildRows();
        }
    }
    if (hoverRow != g.hover) {
        g.hover = hoverRow;
        g.dirty = true;
    }

    // --- Choix : front montant du pincement d'une main qui vise une ligne (jamais pendant le geste des deux mains) --------
    if (!(p[0] && p[1])) {
        for (int h = 0; h < 2; ++h) {
            if (p[h] && !prev[h] && rowOfHand[h] >= 0) {
                Row& r = rows[static_cast<size_t>(rowOfHand[h])];
                if (r.selectable && r.action) {
                    r.action();  // peut fermer le menu ou changer la configuration (déjà relue)
                    g.dirty = true;
                    break;
                }
            }
        }
    }
    if (!g.open) return false;

    if (g.dirty) {
        rows = BuildRows();  // valeurs à jour après un choix
        if (!Redraw(rows)) {
            if (++g.failures >= 3) Disable("echecs repetes du dessin du menu");
            return false;
        }
        g.dirty = false;
        g.failures = 0;
    }

    quad->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    quad->next = nullptr;
    quad->layerFlags = 0;  // opaque
    quad->space = game->space;
    quad->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad->subImage.swapchain = g.swapchain;
    quad->subImage.imageRect = {{0, 0}, {static_cast<int32_t>(g.w), static_cast<int32_t>(g.h)}};
    quad->subImage.imageArrayIndex = 0;
    quad->pose.orientation = {g.quad.q.x, g.quad.q.y, g.quad.q.z, g.quad.q.w};
    quad->pose.position = {g.quad.p.x, g.quad.p.y, g.quad.p.z};
    quad->size = {g.panelW, panelH};
    return true;
}

void VrMenu_NoteEndFrameFailure() {
    if (++g.failures >= 3) Disable("xrEndFrame refuse la couche du menu");
}

void VrMenu_Stop() {
    Emu_SetMenuOpen(false);
    if (g.uploader) g.uploader->WaitIdle();
    if (g.swapchain && g_dispatch.DestroySwapchain) g_dispatch.DestroySwapchain(g.swapchain);
    g.uploader.reset();
    g = MenuState{};
}

}  // namespace layer
