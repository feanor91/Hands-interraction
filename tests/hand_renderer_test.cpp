// hand_renderer_test.cpp — rend une main synthétique avec HandRenderer dans une texture, la relit,
// vérifie que le dessin n'est pas vide et écrit une image BMP (pour contrôle visuel).
// Utilise le GPU par défaut, sinon WARP (rendu logiciel). Windows uniquement.
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "hand_renderer.h"
#include "emulation/ZoneBox.h"

using Microsoft::WRL::ComPtr;
using namespace layer;
namespace em = hands::emulation;

namespace {

int g_failures = 0;
#define EXPECT(cond, msg)                                         \
    do {                                                          \
        if (!(cond)) {                                            \
            std::printf("[ECHEC] %s (ligne %d)\n", msg, __LINE__); \
            ++g_failures;                                         \
        }                                                         \
    } while (0)

ComPtr<ID3D12Device> CreateDevice(bool* warp) {
    ComPtr<ID3D12Device> dev;
    *warp = false;
    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return dev;
    ComPtr<IDXGIFactory4> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
            *warp = true;
            return dev;
        }
    }
    return nullptr;
}

// Main à plat dans le plan XY, doigts vers +Y, à `z` mètres devant la caméra (-Z), centrée en x0.
void MakeFlatHand(HandPoseData& h, float x0, float z, bool pressed) {
    h.valid = true;
    h.pressed = pressed;
    auto set = [&](int j, float x, float y) {
        h.joint[j] = {{0, 0, 0, 1}, {x0 + x, y, z}};
        h.radius[j] = 0.011f;  // ordre de grandeur des rayons rapportes par le runtime
        h.jointValid[j] = true;
    };
    set(0, 0.0f, 0.0f);    // paume
    set(1, 0.0f, -0.07f);  // poignet
    // pouce (2-5) : part du bas de la paume vers l'extérieur
    const float tx[4] = {-0.025f, -0.05f, -0.07f, -0.085f}, ty[4] = {-0.05f, -0.025f, -0.0f, 0.02f};
    for (int i = 0; i < 4; ++i) set(2 + i, tx[i], ty[i]);
    // quatre doigts : 5 articulations chacun (métacarpe + 3 phalanges + bout), indices 6, 11, 16, 21
    const float fx[4] = {-0.030f, -0.010f, 0.010f, 0.030f};
    const float flen[4] = {0.095f, 0.105f, 0.098f, 0.080f};
    for (int f = 0; f < 4; ++f) {
        const int base = 6 + 5 * f;
        set(base, fx[f] * 0.6f, -0.02f);
        for (int s = 1; s < 5; ++s) set(base + s, fx[f], -0.02f + flen[f] * (static_cast<float>(s) / 4.0f) + 0.03f);
    }
}

bool WriteBmp(const char* path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, uint32_t rowPitch) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    const uint32_t imgSize = w * h * 4;
    uint8_t header[54] = {'B', 'M'};
    const uint32_t fileSize = 54 + imgSize, offset = 54, dibSize = 40;
    std::memcpy(header + 2, &fileSize, 4);
    std::memcpy(header + 10, &offset, 4);
    std::memcpy(header + 14, &dibSize, 4);
    const int32_t sw = static_cast<int32_t>(w), sh = -static_cast<int32_t>(h);  // hauteur négative = lignes de haut en bas
    std::memcpy(header + 18, &sw, 4);
    std::memcpy(header + 22, &sh, 4);
    const uint16_t planes = 1, bpp = 32;
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &imgSize, 4);
    std::fwrite(header, 1, 54, f);
    std::vector<uint8_t> row(w * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {  // RGBA -> BGRA ; fond gris foncé là où alpha = 0 pour que le dessin se voie
            const uint8_t* p = &rgba[y * rowPitch + x * 4];
            const float a = p[3] / 255.0f;
            row[x * 4 + 0] = static_cast<uint8_t>(p[2] * a + 40 * (1 - a));
            row[x * 4 + 1] = static_cast<uint8_t>(p[1] * a + 40 * (1 - a));
            row[x * 4 + 2] = static_cast<uint8_t>(p[0] * a + 40 * (1 - a));
            row[x * 4 + 3] = 255;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* outPath = argc > 1 ? argv[1] : "hand_renderer_test.bmp";
    bool warp = false;
    ComPtr<ID3D12Device> dev = CreateDevice(&warp);
    if (!dev) {
        std::printf("[IGNORE] aucun peripherique D3D12 disponible (ni GPU ni WARP)\n");
        return 0;  // pas un échec du code : machine sans D3D12
    }
    std::printf("peripherique D3D12 : %s\n", warp ? "WARP (logiciel)" : "GPU");

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    EXPECT(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))), "CreateCommandQueue");

    const uint32_t W = 384, H = 384;
    const DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12Resource> tex[2];
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = W;
    rd.Height = H;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = fmt;
    for (auto& t : tex)
        EXPECT(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                                      IID_PPV_ARGS(&t))),
               "creation de la texture cible");
    if (g_failures) return 1;

    HandRenderer renderer;
    const char* err = nullptr;
    const bool ok = renderer.Init(dev.Get(), queue.Get(), fmt, &err);
    if (!ok) std::printf("Init : %s\n", err ? err : "?");
    EXPECT(ok, "HandRenderer::Init");
    if (!ok) return 1;

    HandsFrame frame;
    MakeFlatHand(frame.hand[0], -0.09f, -0.35f, false);  // gauche
    MakeFlatHand(frame.hand[1], 0.09f, -0.35f, true);    // droite, pincement allumé (bouts verts)

    // Boites en fil de fer (zones) : un bouton bleu, un rotatif orange incline, un ecran rouge.
    frame.box[frame.boxCount++] = {{{0, 0, 0, 1}, {-0.05f, 0.02f, -0.45f}}, {0.03f, 0.03f, 0.04f}, 0.30f, 0.60f, 1.0f, 0.0016f};
    frame.box[frame.boxCount++] = {{hands::emulation::QuatFromEulerDeg(25, 15, 0), {0.06f, 0.0f, -0.5f}}, {0.05f, 0.05f, 0.05f}, 1.0f, 0.55f, 0.10f, 0.0016f};
    frame.box[frame.boxCount++] = {{{0, 0, 0, 1}, {0.0f, 0.12f, -0.6f}}, {0.16f, 0.11f, 0.06f}, 1.0f, 0.25f, 0.25f, 0.0016f};

    RenderTarget targets[2];
    for (int i = 0; i < 2; ++i) {
        targets[i].resource = tex[i].Get();
        targets[i].width = W;
        targets[i].height = H;
        targets[i].view.pose = {{0, 0, 0, 1}, {i == 0 ? -0.032f : 0.032f, 0.0f, 0.0f}};  // deux yeux écartés de 64 mm
        targets[i].view.angleLeft = -0.7f;
        targets[i].view.angleRight = 0.7f;
        targets[i].view.angleUp = 0.7f;
        targets[i].view.angleDown = -0.7f;
    }
    RenderStyle style;
    style.jointScale = 0.5f;       // valeurs par defaut de hands.ini
    style.boneThickness = 0.008f;
    if (argc > 2) style.mode = std::atoi(argv[2]);  // 0 squelette, 1 gant, 2 main nue, 3 translucide
    // Deux rendus consécutifs : vérifie la rotation des allocateurs et de la profondeur.
    for (int pass = 0; pass < 5; ++pass) {
        const bool r = renderer.Render(targets, 2, frame, style);
        if (!r) std::printf("Render : %s\n", renderer.LastError());
        EXPECT(r, "HandRenderer::Render");
        if (!r) return 1;
    }
    renderer.WaitIdle();

    // --- Relecture de l'œil gauche -------------------------------------------------------------
    for (int eye = 0; eye < 2; ++eye) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0;
        UINT64 rowBytes = 0, total = 0;
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowBytes, &total);
        D3D12_HEAP_PROPERTIES rhp{};
        rhp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        EXPECT(SUCCEEDED(dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&readback))),
               "buffer de relecture");
        ComPtr<ID3D12CommandAllocator> a;
        ComPtr<ID3D12GraphicsCommandList> l;
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l));
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex[eye].Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        l->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = tex[eye].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        l->Close();
        ID3D12CommandList* lists[] = {l.Get()};
        queue->ExecuteCommandLists(1, lists);
        ComPtr<ID3D12Fence> fence;
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        queue->Signal(fence.Get(), 1);
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 10000);
        CloseHandle(ev);

        uint8_t* mapped = nullptr;
        D3D12_RANGE rr{0, static_cast<SIZE_T>(total)};
        EXPECT(SUCCEEDED(readback->Map(0, &rr, reinterpret_cast<void**>(&mapped))), "Map");
        if (!mapped) return 1;
        std::vector<uint8_t> pixels(mapped, mapped + total);
        readback->Unmap(0, nullptr);

        uint32_t covered = 0, green = 0, blue = 0, orange = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const uint8_t* p = &pixels[y * fp.Footprint.RowPitch + x * 4];
                if (p[3] == 0) continue;
                ++covered;
                if (p[1] > 150 && p[0] < 100 && p[2] < 120) ++green;     // bouts allumés (pincement)
                if (p[2] > 150 && p[0] < 110) ++blue;                    // os de la main gauche
                if (p[0] > 150 && p[2] < 90 && p[1] > 70) ++orange;      // os de la main droite
            }
        std::printf("oeil %d : %u pixels couverts, %u verts, %u bleus, %u oranges\n", eye, covered, green, blue, orange);
        EXPECT(covered > 1500, "la main doit couvrir des pixels");
        EXPECT(covered < W * H * 3 / 4, "la main ne doit pas remplir l'image");
        if (style.mode == 0) {  // les couleurs gauche/droite et le vert du pincement ne concernent que le squelette
            EXPECT(green > 20, "les bouts de doigts du pincement doivent etre verts");
            EXPECT(blue > 100, "main gauche (bleue) visible");
            EXPECT(orange > 100, "main droite (orange) visible");
        }

        char path[512];
        std::snprintf(path, sizeof(path), "%s", outPath);
        if (eye == 1) std::snprintf(path, sizeof(path), "%s.droit.bmp", outPath);
        EXPECT(WriteBmp(path, pixels, W, H, fp.Footprint.RowPitch), "ecriture du BMP");
    }

    if (g_failures == 0) std::printf("hand_renderer_test : OK (image : %s)\n", outPath);
    return g_failures == 0 ? 0 : 1;
}
