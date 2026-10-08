// depth_reader_test.cpp — vérifie DepthReader sur une texture de profondeur au même format que celle du jeu
// (R32G8X24_TYPELESS, vue D32_FLOAT_S8X24) : on y écrit des valeurs connues, on les relit, on compare.
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "depth_reader.h"
#include "texture_uploader.h"

using Microsoft::WRL::ComPtr;
using namespace layer;

namespace {
int g_failures = 0;
#define EXPECT(cond, msg)                                          \
    do {                                                           \
        if (!(cond)) {                                             \
            std::printf("[ECHEC] %s (ligne %d)\n", msg, __LINE__); \
            ++g_failures;                                          \
        }                                                          \
    } while (0)

ComPtr<ID3D12Device> CreateDevice() {
    ComPtr<ID3D12Device> dev;
    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return dev;
    ComPtr<IDXGIFactory4> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))))
            return dev;
    }
    return nullptr;
}
bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }
}  // namespace

int main() {
    ComPtr<ID3D12Device> dev = CreateDevice();
    if (!dev) {
        std::printf("[IGNORE] aucun peripherique D3D12 disponible\n");
        return 0;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    EXPECT(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))), "CreateCommandQueue");

    // Texture de profondeur comme celle du jeu : R32G8X24_TYPELESS, drapeau ALLOW_DEPTH_STENCIL seul
    const uint32_t W = 640, H = 480;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = W;
    rd.Height = H;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    cv.DepthStencil.Depth = 0.25f;
    ComPtr<ID3D12Resource> depth;
    EXPECT(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                  IID_PPV_ARGS(&depth))),
           "creation de la texture de profondeur");
    if (g_failures) return 1;

    // Remplissage : tout à 0.25, puis un rectangle à 0.75 (200..300 x 100..180)
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    EXPECT(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap))), "tas DSV");
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateDepthStencilView(depth.Get(), &dv, dsv);

    ComPtr<ID3D12CommandAllocator> a;
    ComPtr<ID3D12GraphicsCommandList> l;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l));
    l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, nullptr);
    const D3D12_RECT rect{200, 100, 300, 180};
    l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, 1, &rect);
    l->Close();
    ID3D12CommandList* lists[] = {l.Get()};
    queue->ExecuteCommandLists(1, lists);

    // Lecture : un pixel hors rectangle, un pixel dedans, une zone 3x3 à cheval sur le bord, une zone hors image
    DepthReader reader;
    const char* err = nullptr;
    const bool ok = reader.Init(dev.Get(), queue.Get(), &err);
    if (!ok) std::printf("Init : %s\n", err ? err : "?");
    EXPECT(ok, "DepthReader::Init");
    if (!ok) return 1;

    std::vector<DepthRequestEye> req(1);
    req[0].depth = depth.Get();
    req[0].boxes = {{10, 10, 1, 1}, {250, 140, 1, 1}, {199, 140, 3, 1}, {5000, 5000, 1, 1}, {636, 476, 10, 10}};
    EXPECT(reader.Submit(req), "Submit");
    EXPECT(reader.Pending(), "requete en cours");
    EXPECT(!reader.Submit(req), "une seule requete a la fois");

    std::vector<DepthResultEye> res;
    for (int i = 0; i < 2000 && !reader.TryCollect(&res); ++i) Sleep(5);
    EXPECT(!reader.Pending(), "requete terminee");
    EXPECT(res.size() == 1 && res[0].boxes.size() == 5, "un resultat par zone");
    if (res.size() == 1 && res[0].boxes.size() == 5) {
        const auto& b = res[0].boxes;
        std::printf("valeurs : hors=%.3f dedans=%.3f bord=[%.3f %.3f %.3f] horsimage=%zu coin=%zu valeurs\n", b[0].empty() ? -1.0f : b[0][0],
                    b[1].empty() ? -1.0f : b[1][0], b[2].size() > 2 ? b[2][0] : -1.0f, b[2].size() > 2 ? b[2][1] : -1.0f,
                    b[2].size() > 2 ? b[2][2] : -1.0f, b[3].size(), b[4].size());
        EXPECT(b[0].size() == 1 && Near(b[0][0], 0.25f), "pixel hors rectangle = 0.25");
        EXPECT(b[1].size() == 1 && Near(b[1][0], 0.75f), "pixel dans le rectangle = 0.75");
        EXPECT(b[2].size() == 3 && Near(b[2][0], 0.25f) && Near(b[2][1], 0.75f) && Near(b[2][2], 0.75f), "zone a cheval sur le bord");
        EXPECT(b[3].empty(), "zone hors image ignoree");
        EXPECT(b[4].size() == 16, "zone au coin tronquee a 4x4");
    }

    // Une deuxieme requete fonctionne apres la premiere (rotation des tampons)
    EXPECT(reader.Submit(req), "deuxieme Submit");
    for (int i = 0; i < 2000 && !reader.TryCollect(&res); ++i) Sleep(5);
    EXPECT(res.size() == 1 && res[0].boxes.size() == 5 && res[0].boxes[1].size() == 1 && Near(res[0].boxes[1][0], 0.75f), "relecture repetee");

    // La ressource doit etre restee utilisable (etat d'origine) : on peut la reeffacer sans erreur
    a->Reset();
    l->Reset(a.Get(), nullptr);
    l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, nullptr);
    l->Close();
    queue->ExecuteCommandLists(1, lists);
    reader.WaitIdle();

    // --- Lecture d'une image COULEUR (R8G8B8A8_TYPELESS, comme les swapchains du jeu), pour les captures ---------------
    {
        D3D12_RESOURCE_DESC cd{};
        cd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        cd.Width = 320;
        cd.Height = 200;
        cd.DepthOrArraySize = 1;
        cd.MipLevels = 1;
        cd.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        cd.SampleDesc.Count = 1;
        cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE ccv{};
        ccv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ccv.Color[0] = 1.0f;
        ccv.Color[1] = 128.0f / 255.0f;
        ccv.Color[2] = 0.0f;
        ccv.Color[3] = 1.0f;
        ComPtr<ID3D12Resource> color;
        EXPECT(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_RENDER_TARGET, &ccv, IID_PPV_ARGS(&color))),
               "creation de la texture couleur");
        D3D12_DESCRIPTOR_HEAP_DESC rh{};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        EXPECT(SUCCEEDED(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvHeap))), "tas RTV");
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(color.Get(), &rv, rtv);
        a->Reset();
        l->Reset(a.Get(), nullptr);
        const float orange[4] = {1.0f, 128.0f / 255.0f, 0.0f, 1.0f}, bleu[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        l->ClearRenderTargetView(rtv, orange, 0, nullptr);
        const D3D12_RECT zone{100, 50, 120, 60};
        l->ClearRenderTargetView(rtv, bleu, 1, &zone);
        l->Close();
        queue->ExecuteCommandLists(1, lists);

        std::vector<DepthRequestEye> creq(1);
        creq[0].depth = color.Get();
        creq[0].state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        creq[0].format = DXGI_FORMAT_R8G8B8A8_UNORM;
        creq[0].boxes = {{5, 5, 1, 1}, {110, 55, 1, 1}, {98, 55, 6, 1}};
        EXPECT(reader.Submit(creq), "Submit couleur");
        std::vector<DepthResultEye> cres;
        for (int i = 0; i < 2000 && !reader.TryCollect(&cres); ++i) Sleep(5);
        EXPECT(cres.size() == 1 && cres[0].boxes.size() == 3, "resultat couleur");
        if (cres.size() == 1 && cres[0].boxes.size() == 3) {
            uint32_t w0 = 0, w1 = 0, w2 = 0;
            if (!cres[0].boxes[0].empty()) std::memcpy(&w0, &cres[0].boxes[0][0], 4);
            if (!cres[0].boxes[1].empty()) std::memcpy(&w1, &cres[0].boxes[1][0], 4);
            if (cres[0].boxes[2].size() == 6) std::memcpy(&w2, &cres[0].boxes[2][2], 4);  // pixel x = 100 : premier du rectangle bleu
            std::printf("couleur : hors=0x%08X dedans=0x%08X bord=0x%08X\n", w0, w1, w2);
            EXPECT(w0 == 0xFF0080FFu, "pixel orange (octets R=255 G=128 B=0 A=255)");
            EXPECT(w1 == 0xFFFF0000u, "pixel bleu (octets R=0 G=0 B=255 A=255)");
            EXPECT(w2 == 0xFFFF0000u, "premier pixel du rectangle bleu");
        }
        reader.WaitIdle();
    }

    // --- Envoi d'une image CPU dans une texture B8G8R8A8 (comme le menu dans le casque), puis relecture ----------------
    {
        const uint32_t uw = 100, uh = 60;
        D3D12_RESOURCE_DESC ud{};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ud.Width = uw;
        ud.Height = uh;
        ud.DepthOrArraySize = 1;
        ud.MipLevels = 1;
        ud.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        ud.SampleDesc.Count = 1;
        ud.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> tex;
        EXPECT(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&tex))),
               "creation de la texture d'envoi");
        std::vector<uint32_t> pix(uw * uh);
        for (uint32_t y = 0; y < uh; ++y)
            for (uint32_t x = 0; x < uw; ++x) pix[y * uw + x] = 0xFF000000u | (y << 8) | x;  // B = x, G = y
        TextureUploader up;
        const char* uerr = nullptr;
        const bool uok = up.Init(dev.Get(), queue.Get(), &uerr);
        if (!uok) std::printf("Init envoi : %s\n", uerr ? uerr : "?");
        EXPECT(uok, "TextureUploader::Init");
        if (uok) {
            EXPECT(up.Upload(tex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, DXGI_FORMAT_B8G8R8A8_UNORM, pix.data(), uw, uh, uw * 4), "Upload");
            EXPECT(up.Upload(tex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, DXGI_FORMAT_B8G8R8A8_UNORM, pix.data(), uw, uh, uw * 4), "deuxieme Upload");
            up.WaitIdle();
            std::vector<DepthRequestEye> ureq(1);
            ureq[0].depth = tex.Get();
            ureq[0].state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            ureq[0].format = DXGI_FORMAT_B8G8R8A8_UNORM;
            ureq[0].boxes = {{0, 0, 1, 1}, {37, 21, 1, 1}, {99, 59, 1, 1}, {10, 5, 3, 2}};
            EXPECT(reader.Submit(ureq), "Submit relecture envoi");
            std::vector<DepthResultEye> ures;
            for (int i = 0; i < 2000 && !reader.TryCollect(&ures); ++i) Sleep(5);
            EXPECT(ures.size() == 1 && ures[0].boxes.size() == 4, "resultat relecture envoi");
            if (ures.size() == 1 && ures[0].boxes.size() == 4) {
                auto word = [&](size_t box, size_t i) {
                    uint32_t v = 0;
                    if (i < ures[0].boxes[box].size()) std::memcpy(&v, &ures[0].boxes[box][i], 4);
                    return v;
                };
                std::printf("envoi : (0,0)=0x%08X (37,21)=0x%08X (99,59)=0x%08X\n", word(0, 0), word(1, 0), word(2, 0));
                EXPECT(word(0, 0) == 0xFF000000u, "pixel (0,0)");
                EXPECT(word(1, 0) == (0xFF000000u | (21u << 8) | 37u), "pixel (37,21)");
                EXPECT(word(2, 0) == (0xFF000000u | (59u << 8) | 99u), "pixel (99,59)");
                EXPECT(ures[0].boxes[3].size() == 6 && word(3, 5) == (0xFF000000u | (6u << 8) | 12u), "zone 3x2 : dernier pixel (12,6)");
            }
            reader.WaitIdle();
        }
    }

    if (g_failures == 0) std::printf("depth_reader_test : OK\n");
    return g_failures == 0 ? 0 : 1;
}
