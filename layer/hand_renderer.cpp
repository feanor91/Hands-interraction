// hand_renderer.cpp — voir hand_renderer.h.
#include "hand_renderer.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace layer {

namespace em = hands::emulation;
using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------------------------
// Matrices 4x4 en ligne (row-major), convention « vecteur colonne » : v' = M * v.
struct Mat4 {
    float m[16];
};

Mat4 Identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}

Mat4 FromPose(const em::Pose& p) {
    const float x = p.q.x, y = p.q.y, z = p.q.z, w = p.q.w;
    Mat4 r = Identity();
    r.m[0] = 1 - 2 * (y * y + z * z);  r.m[1] = 2 * (x * y - z * w);      r.m[2] = 2 * (x * z + y * w);
    r.m[4] = 2 * (x * y + z * w);      r.m[5] = 1 - 2 * (x * x + z * z);  r.m[6] = 2 * (y * z - x * w);
    r.m[8] = 2 * (x * z - y * w);      r.m[9] = 2 * (y * z + x * w);      r.m[10] = 1 - 2 * (x * x + y * y);
    r.m[3] = p.p.x;
    r.m[7] = p.p.y;
    r.m[11] = p.p.z;
    return r;
}

Mat4 Scale(float sx, float sy, float sz) {
    Mat4 r = Identity();
    r.m[0] = sx;
    r.m[5] = sy;
    r.m[10] = sz;
    return r;
}

// Projection OpenXR (caméra regardant vers -Z) vers l'espace de découpage Direct3D (z dans [0,1]).
Mat4 Projection(const EyeView& v, float n, float f) {
    const float tl = std::tan(v.angleLeft), tr = std::tan(v.angleRight);
    const float tu = std::tan(v.angleUp), td = std::tan(v.angleDown);
    Mat4 r{};
    r.m[0] = 2.0f / (tr - tl);  r.m[2] = (tr + tl) / (tr - tl);
    r.m[5] = 2.0f / (tu - td);  r.m[6] = (tu + td) / (tu - td);
    r.m[10] = f / (n - f);      r.m[11] = n * f / (n - f);
    r.m[14] = -1.0f;
    return r;
}

// Rotation envoyant +Z sur la direction `d` (unitaire).
em::Quat RotationFromZ(em::Vec3 d) {
    const em::Vec3 z{0, 0, 1};
    const float dot = d.z;
    if (dot < -0.9999f) return {1, 0, 0, 0};  // demi-tour autour de X
    const em::Vec3 axis{z.y * d.z - z.z * d.y, z.z * d.x - z.x * d.z, z.x * d.y - z.y * d.x};
    return em::Normalize({axis.x, axis.y, axis.z, 1.0f + dot});
}

// ---------------------------------------------------------------------------------------------
// Primitives à dessiner (cubes)
// Primitive : cube (36 sommets), sphere (576) ou tronc de cone (72), tous generes dans le vertex shader a partir de SV_VertexID.
enum PrimKind { kCube = 0, kSphere = 1, kFrustum = 2 };
constexpr uint32_t kVertexCount[3] = {36, 576, 72};

struct Prim {
    Mat4 model;
    float color[4];
    PrimKind kind = kCube;
    float r0 = 0, r1 = 0;  // sphere : rayon (r0) ; tronc de cone : rayons aux deux bouts (le long de +Z local, de -0,5 a +0,5)
    float alpha = 1.0f;    // < 1 : dessine dans la passe translucide
};

struct RootConstants {
    float mvp[16];
    float model[16];
    float color[4];
    float params[4];  // x = type de primitive, y = rayon 0, z = rayon 1, w = opacite
};
static_assert(sizeof(RootConstants) == 40 * 4, "40 DWORD de constantes racine");

// Os du squelette (indices de articulations OpenXR : 0 paume, 1 poignet, 2-5 pouce, 6-10 index,
// 11-15 majeur, 16-20 annulaire, 21-25 auriculaire).
const int kBones[][2] = {
    {1, 2},   {2, 3},   {3, 4},   {4, 5},                     // pouce
    {1, 6},   {6, 7},   {7, 8},   {8, 9},   {9, 10},          // index
    {1, 11},  {11, 12}, {12, 13}, {13, 14}, {14, 15},         // majeur
    {1, 16},  {16, 17}, {17, 18}, {18, 19}, {19, 20},         // annulaire
    {1, 21},  {21, 22}, {22, 23}, {23, 24}, {24, 25},         // auriculaire
    {7, 12},  {12, 17}, {17, 22},                             // largeur de la paume (articulations proximales)
};

constexpr int kThumbTip = 5;

void AddCube(std::vector<Prim>& out, const em::Pose& pose, float sx, float sy, float sz, float r, float g, float b) {
    Prim p;
    p.model = Mul(FromPose(pose), Scale(sx, sy, sz));
    p.color[0] = r; p.color[1] = g; p.color[2] = b; p.color[3] = 1.0f;
    out.push_back(p);
}

// Boite en fil de fer : 12 aretes, chacune un cube tres fin le long d'un axe local.
void AddWireBox(std::vector<Prim>& out, const BoxData& bx) {
    const float hx = bx.size.x * 0.5f, hy = bx.size.y * 0.5f, hz = bx.size.z * 0.5f, t = bx.edge;
    auto edge = [&](float ox, float oy, float oz, float sx, float sy, float sz) {
        const em::Pose p{bx.pose.q, bx.pose.p + em::Rotate(bx.pose.q, em::Vec3{ox, oy, oz})};
        AddCube(out, p, sx, sy, sz, bx.r, bx.g, bx.b);
    };
    if (bx.faceOnly) {  // face avant : 4 aretes a z = +hz
        edge(0, hy, hz, bx.size.x + t, t, t);
        edge(0, -hy, hz, bx.size.x + t, t, t);
        edge(hx, 0, hz, t, bx.size.y + t, t);
        edge(-hx, 0, hz, t, bx.size.y + t, t);
        return;
    }
    for (float a : {-1.0f, 1.0f})
        for (float b : {-1.0f, 1.0f}) {
            edge(0, a * hy, b * hz, bx.size.x + t, t, t);  // aretes le long de X
            edge(a * hx, 0, b * hz, t, bx.size.y + t, t);  // le long de Y
            edge(a * hx, b * hy, 0, t, t, bx.size.z + t);  // le long de Z
        }
}

void AddSphere(std::vector<Prim>& out, const em::Vec3& c, float radius, float r, float g, float b, float alpha) {
    Prim p;
    p.model = FromPose(em::Pose{em::Quat{0, 0, 0, 1}, c});
    p.color[0] = r; p.color[1] = g; p.color[2] = b; p.color[3] = 1.0f;
    p.kind = kSphere;
    p.r0 = radius;
    p.alpha = alpha;
    out.push_back(p);
}

void AddFrustum(std::vector<Prim>& out, const em::Vec3& a, const em::Vec3& b, float ra, float rb, float r, float g, float bl, float alpha) {
    const em::Vec3 d = b - a;
    const float len = em::Length(d);
    if (len < 1e-4f) return;
    Prim p;
    p.model = Mul(FromPose(em::Pose{RotationFromZ(d * (1.0f / len)), a + d * 0.5f}), Scale(1.0f, 1.0f, len));
    p.color[0] = r; p.color[1] = g; p.color[2] = bl; p.color[3] = 1.0f;
    p.kind = kFrustum;
    p.r0 = ra;
    p.r1 = rb;
    p.alpha = alpha;
    out.push_back(p);
}

// Main pleine (gant, main nue, main translucide) : une sphere par articulation, un tronc de cone par os, et la paume comblee.
void AddSolidHand(std::vector<Prim>& out, const HandPoseData& hd, const RenderStyle& style, int hand) {
    float cr = 0.12f, cg = 0.15f, cb = 0.10f, alpha = 1.0f, grow = 1.15f;  // gant : vert-noir mat, un peu plus epais que la peau
    if (style.mode == kHandBare) { cr = 0.90f; cg = 0.70f; cb = 0.58f; grow = 1.0f; }
    if (style.mode == kHandGhost) { cr = 0.55f; cg = 0.80f; cb = 1.0f; alpha = std::max(0.05f, std::min(1.0f, style.alpha)); grow = 1.0f; }
    (void)hand;
    auto rad = [&](int j) { return std::max(hd.radius[j], 0.004f) * grow; };
    auto ok = [&](int j) { return hd.jointValid[j]; };
    // Os du squelette (doigts, pouce, metacarpiens) : tronc de cone entre deux articulations.
    for (const auto& bone : kBones) {
        const int a = bone[0], b = bone[1];
        if (!ok(a) || !ok(b)) continue;
        AddFrustum(out, hd.joint[a].p, hd.joint[b].p, rad(a), rad(b), cr, cg, cb, alpha);
    }
    // Paume : metacarpiens relies entre eux, et gros volumes au poignet et au centre de la paume.
    static const int kPalmLinks[][2] = {{6, 11}, {11, 16}, {16, 21}, {7, 12}, {12, 17}, {17, 22}};
    for (const auto& l : kPalmLinks)
        if (ok(l[0]) && ok(l[1])) AddFrustum(out, hd.joint[l[0]].p, hd.joint[l[1]].p, std::max(rad(l[0]), 0.009f * grow), std::max(rad(l[1]), 0.009f * grow), cr, cg, cb, alpha);
    if (ok(0)) AddSphere(out, hd.joint[0].p, 0.030f * grow, cr, cg, cb, alpha);
    if (ok(1)) AddSphere(out, hd.joint[1].p, 0.026f * grow, cr, cg, cb, alpha);
    for (int j = 0; j < kHandJointCount; ++j)
        if (ok(j) && j != 0 && j != 1) AddSphere(out, hd.joint[j].p, rad(j), cr, cg, cb, alpha);
}

void BuildPrims(const HandsFrame& frame, const RenderStyle& style, std::vector<Prim>& out) {
    for (int i = 0; i < frame.boxCount && i < kMaxBoxes; ++i) AddWireBox(out, frame.box[i]);
    for (int m = 0; m < frame.markerCount && m < kMaxMarkers; ++m) {
        const MarkerData& mk = frame.marker[m];
        AddCube(out, em::Pose{em::Quat{0, 0, 0, 1}, mk.pos}, mk.size, mk.size, mk.size, mk.r, mk.g, mk.b);
    }
    for (int h = 0; h < 2; ++h) {
        const HandPoseData& hd = frame.hand[h];
        if (!hd.valid) continue;
        if (style.mode != kHandSkeleton) {
            AddSolidHand(out, hd, style, h);
            continue;
        }
        const float br = h == 0 ? 0.20f : 1.00f, bg = h == 0 ? 0.50f : 0.45f, bb = h == 0 ? 1.00f : 0.15f;  // os
        for (const auto& bone : kBones) {
            const int a = bone[0], b = bone[1];
            if (!hd.jointValid[a] || !hd.jointValid[b]) continue;
            const em::Vec3 d = hd.joint[b].p - hd.joint[a].p;
            const float len = em::Length(d);
            if (len < 1e-4f) continue;
            const em::Vec3 dir = d * (1.0f / len);
            const em::Pose pose{RotationFromZ(dir), hd.joint[a].p + d * 0.5f};
            AddCube(out, pose, style.boneThickness, style.boneThickness, len, br, bg, bb);
        }
        for (int j = 0; j < kHandJointCount; ++j) {
            if (!hd.jointValid[j]) continue;
            float size = std::max(hd.radius[j] * 2.0f * style.jointScale, 0.006f);
            float r = h == 0 ? 0.62f : 1.00f, g = h == 0 ? 0.80f : 0.78f, b = h == 0 ? 1.00f : 0.55f;  // teinte claire de la main
            if (hd.pressed && (j == kThumbTip || j == hd.clickJoint)) { r = 0.15f; g = 1.0f; b = 0.25f; size *= 1.4f; }
            AddCube(out, hd.joint[j], size, size, size, r, g, b);
        }
    }
}

// ---------------------------------------------------------------------------------------------
const char* kShader = R"HLSL(
cbuffer Root : register(b0) {
    row_major float4x4 mvp;
    row_major float4x4 model;
    float4 color;
    float4 params;   // x = type (0 cube, 1 sphere, 2 tronc de cone), y = rayon 0, z = rayon 1, w = opacite
};
struct VSOut { float4 pos : SV_POSITION; float3 nrm : NORMAL; };
static const float3 C[8] = {
    float3(-0.5,-0.5,-0.5), float3( 0.5,-0.5,-0.5), float3( 0.5, 0.5,-0.5), float3(-0.5, 0.5,-0.5),
    float3(-0.5,-0.5, 0.5), float3( 0.5,-0.5, 0.5), float3( 0.5, 0.5, 0.5), float3(-0.5, 0.5, 0.5) };
static const uint IDX[36] = {
    0,2,1, 0,3,2,   4,5,6, 4,6,7,   0,1,5, 0,5,4,
    3,6,2, 3,7,6,   0,4,7, 0,7,3,   1,2,6, 1,6,5 };
static const float3 NRM[6] = {
    float3(0,0,-1), float3(0,0,1), float3(0,-1,0), float3(0,1,0), float3(-1,0,0), float3(1,0,0) };
static const float PI2 = 6.2831853;
float3 SpherePoint(uint i, uint j) {   // 12 meridiens x 8 paralleles
    float th = PI2 * (float)(i % 12) / 12.0;
    float ph = 3.1415927 * (float)j / 8.0;
    return float3(sin(ph) * cos(th), cos(ph), sin(ph) * sin(th));
}
VSOut VS(uint vid : SV_VertexID) {
    VSOut o;
    float3 p; float3 n;
    if (params.x < 0.5) {
        p = C[IDX[vid]];
        n = mul((float3x3)model, NRM[vid / 6]);
    } else if (params.x < 1.5) {
        uint tri = vid / 3, c = vid % 3, q = tri / 2, second = tri % 2;
        uint i = q % 12, j = q / 12;
        uint k = second == 0 ? (c == 0 ? 0 : (c == 1 ? 1 : 2)) : (c == 0 ? 0 : (c == 1 ? 2 : 3));
        uint di = (k == 1 || k == 2) ? 1 : 0, dj = (k == 2 || k == 3) ? 1 : 0;
        float3 u = SpherePoint(i + di, j + dj);
        p = u * params.y;
        n = mul((float3x3)model, u);
    } else {
        uint tri = vid / 3, c = vid % 3, q = tri / 2, second = tri % 2;
        uint k = second == 0 ? (c == 0 ? 0 : (c == 1 ? 1 : 2)) : (c == 0 ? 0 : (c == 1 ? 2 : 3));
        uint di = (k == 1 || k == 2) ? 1 : 0, dt = (k == 2 || k == 3) ? 1 : 0;
        float th = PI2 * (float)((q + di) % 12) / 12.0;
        float rr = lerp(params.y, params.z, (float)dt);
        p = float3(cos(th) * rr, sin(th) * rr, (float)dt - 0.5);
        n = mul((float3x3)model, float3(cos(th), sin(th), 0.0));
    }
    o.pos = mul(mvp, float4(p, 1.0));
    o.nrm = n;
    return o;
}
float4 PS(VSOut i) : SV_Target {
    float3 n = normalize(i.nrm);
    float l = 0.55 + 0.45 * saturate(dot(n, normalize(float3(0.3, 0.8, 0.5))));
    float a = params.w;
    return float4(color.rgb * l * a, a);   // alpha premultiplie (couche de composition sans bit UNPREMULTIPLIED)
}
)HLSL";

constexpr int kSlots = 3;     // allocateurs de commandes en rotation
constexpr int kMaxTargets = 4;

using PFN_D3DCompileLocal = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                             UINT, UINT, ID3DBlob**, ID3DBlob**);

}  // namespace

struct HandRenderer::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    DXGI_FORMAT rtFormat = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;        // opaque
    ComPtr<ID3D12PipelineState> psoDepth;   // profondeur seule (passe 1 des mains translucides)
    ComPtr<ID3D12PipelineState> psoBlend;   // melange alpha premultiplie, sans ecriture de profondeur (passe 2)
    ComPtr<ID3D12CommandAllocator> alloc[kSlots];
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    UINT64 slotValue[kSlots] = {};

    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
    UINT rtvInc = 0;
    ComPtr<ID3D12Resource> depth;
    uint32_t depthW = 0, depthH = 0;

    std::string error;
    bool ready = false;

    ~Impl() {
        WaitIdle();
        if (fenceEvent) CloseHandle(fenceEvent);
    }

    bool Fail(const char* msg, HRESULT hr = S_OK) {
        char buf[200];
        if (hr != S_OK) std::snprintf(buf, sizeof(buf), "%s (HRESULT 0x%08lX)", msg, static_cast<unsigned long>(hr));
        else std::snprintf(buf, sizeof(buf), "%s", msg);
        error = buf;
        return false;
    }

    void WaitIdle() {
        if (!queue || !fence || !fenceEvent) return;
        const UINT64 v = ++fenceValue;
        if (FAILED(queue->Signal(fence.Get(), v))) return;
        if (fence->GetCompletedValue() < v) {
            fence->SetEventOnCompletion(v, fenceEvent);
            WaitForSingleObject(fenceEvent, 5000);
        }
    }

    bool CompileShaders(ComPtr<ID3DBlob>& vs, ComPtr<ID3DBlob>& ps) {
        HMODULE lib = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!lib) return Fail("d3dcompiler_47.dll introuvable");
        auto compile = reinterpret_cast<PFN_D3DCompileLocal>(GetProcAddress(lib, "D3DCompile"));
        if (!compile) return Fail("D3DCompile introuvable");
        const struct { const char* entry; const char* target; ComPtr<ID3DBlob>* out; } jobs[] = {
            {"VS", "vs_5_0", &vs}, {"PS", "ps_5_0", &ps}};
        for (const auto& j : jobs) {
            ComPtr<ID3DBlob> err;
            const HRESULT hr = compile(kShader, std::strlen(kShader), "hands.hlsl", nullptr, nullptr, j.entry, j.target,
                                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, j.out->GetAddressOf(), err.GetAddressOf());
            if (FAILED(hr)) {
                error = std::string("compilation shader ") + j.entry + " : " +
                        (err ? static_cast<const char*>(err->GetBufferPointer()) : "erreur inconnue");
                return false;
            }
        }
        return true;
    }

    bool Init(ID3D12Device* dev, ID3D12CommandQueue* q, DXGI_FORMAT fmt) {
        device = dev;
        queue = q;
        rtFormat = fmt;
        if (!device || !queue) return Fail("peripherique ou file de commandes absents");

        ComPtr<ID3DBlob> vs, ps;
        if (!CompileShaders(vs, ps)) return false;

        // Signature racine : 40 constantes de 32 bits (mvp, modele, couleur, parametres de primitive)
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.ShaderRegister = 0;
        param.Constants.RegisterSpace = 0;
        param.Constants.Num32BitValues = 40;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 1;
        rs.pParameters = &param;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> rsBlob, rsErr;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf());
        if (FAILED(hr)) return Fail("serialisation de la signature racine", hr);
        hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));
        if (FAILED(hr)) return Fail("CreateRootSignature", hr);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rootSig.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = rtFormat;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count = 1;
        hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) return Fail("CreateGraphicsPipelineState", hr);
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC dd = pd;  // passe 1 : pas de couleur, profondeur ecrite
            dd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
            hr = device->CreateGraphicsPipelineState(&dd, IID_PPV_ARGS(&psoDepth));
            if (FAILED(hr)) return Fail("CreateGraphicsPipelineState(profondeur)", hr);
            D3D12_GRAPHICS_PIPELINE_STATE_DESC bd = pd;  // passe 2 : melange, la couche la plus proche seulement
            D3D12_RENDER_TARGET_BLEND_DESC& rt = bd.BlendState.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_ONE;
            rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            bd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            bd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            hr = device->CreateGraphicsPipelineState(&bd, IID_PPV_ARGS(&psoBlend));
            if (FAILED(hr)) return Fail("CreateGraphicsPipelineState(melange)", hr);
        }

        for (int i = 0; i < kSlots; ++i) {
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i]));
            if (FAILED(hr)) return Fail("CreateCommandAllocator", hr);
        }
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), pso.Get(), IID_PPV_ARGS(&list));
        if (FAILED(hr)) return Fail("CreateCommandList", hr);
        list->Close();
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr)) return Fail("CreateFence", hr);
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) return Fail("CreateEvent");

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kMaxTargets;
        hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
        if (FAILED(hr)) return Fail("CreateDescriptorHeap(RTV)", hr);
        rtvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap));
        if (FAILED(hr)) return Fail("CreateDescriptorHeap(DSV)", hr);
        ready = true;
        return true;
    }

    bool EnsureDepth(uint32_t w, uint32_t h) {
        if (depth && depthW >= w && depthH >= h) return true;
        WaitIdle();  // l'ancienne profondeur peut encore servir au GPU
        depth.Reset();
        const uint32_t nw = std::max(w, depthW), nh = std::max(h, depthH);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = nw;
        rd.Height = nh;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_D32_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1.0f;
        const HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                           IID_PPV_ARGS(&depth));
        if (FAILED(hr)) return Fail("CreateCommittedResource(profondeur)", hr);
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(depth.Get(), &dv, dsvHeap->GetCPUDescriptorHandleForHeapStart());
        depthW = nw;
        depthH = nh;
        return true;
    }

    bool Render(const RenderTarget* targets, int count, const HandsFrame& frame, const RenderStyle& style) {
        if (!ready) return false;
        count = std::min(count, kMaxTargets);
        if (count <= 0) return true;
        uint32_t mw = 0, mh = 0;
        for (int i = 0; i < count; ++i) {
            if (!targets[i].resource) return Fail("cible nulle");
            mw = std::max(mw, targets[i].width);
            mh = std::max(mh, targets[i].height);
        }
        if (!EnsureDepth(mw, mh)) return false;

        std::vector<Prim> prims;
        prims.reserve(2600);
        BuildPrims(frame, style, prims);

        const int slot = static_cast<int>(frame_++ % kSlots);
        if (fence->GetCompletedValue() < slotValue[slot]) {
            fence->SetEventOnCompletion(slotValue[slot], fenceEvent);
            if (WaitForSingleObject(fenceEvent, 2000) != WAIT_OBJECT_0) return Fail("attente GPU trop longue");
        }
        HRESULT hr = alloc[slot]->Reset();
        if (FAILED(hr)) return Fail("allocator->Reset", hr);
        hr = list->Reset(alloc[slot].Get(), pso.Get());
        if (FAILED(hr)) return Fail("list->Reset", hr);

        list->SetGraphicsRootSignature(rootSig.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        const float clear[4] = {0, 0, 0, 0};

        for (int i = 0; i < count; ++i) {
            const RenderTarget& t = targets[i];
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += static_cast<SIZE_T>(i) * rtvInc;
            D3D12_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = rtFormat;
            rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(t.resource, &rd, rtv);

            list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            const D3D12_VIEWPORT vp{0, 0, static_cast<float>(t.width), static_cast<float>(t.height), 0.0f, 1.0f};
            const D3D12_RECT sc{0, 0, static_cast<LONG>(t.width), static_cast<LONG>(t.height)};
            list->RSSetViewports(1, &vp);
            list->RSSetScissorRects(1, &sc);

            const Mat4 viewProj = Mul(Projection(t.view, 0.02f, 20.0f), FromPose(em::Inverse(t.view.pose)));
            auto draw = [&](const Prim& p) {
                RootConstants rc;
                const Mat4 mvp = Mul(viewProj, p.model);
                std::memcpy(rc.mvp, mvp.m, sizeof(rc.mvp));
                std::memcpy(rc.model, p.model.m, sizeof(rc.model));
                std::memcpy(rc.color, p.color, sizeof(rc.color));
                rc.params[0] = static_cast<float>(p.kind);
                rc.params[1] = p.r0;
                rc.params[2] = p.r1;
                rc.params[3] = p.alpha;
                list->SetGraphicsRoot32BitConstants(0, 40, &rc, 0);
                list->DrawInstanced(kVertexCount[p.kind], 1, 0, 0);
            };
            for (const Prim& p : prims)
                if (p.alpha >= 1.0f) draw(p);
            bool anyGhost = false;
            for (const Prim& p : prims) anyGhost = anyGhost || p.alpha < 1.0f;
            if (anyGhost) {  // mains translucides : profondeur d'abord (une seule couche visible), puis melange
                list->SetPipelineState(psoDepth.Get());
                for (const Prim& p : prims)
                    if (p.alpha < 1.0f) draw(p);
                list->SetPipelineState(psoBlend.Get());
                for (const Prim& p : prims)
                    if (p.alpha < 1.0f) draw(p);
                list->SetPipelineState(pso.Get());
            }
        }
        hr = list->Close();
        if (FAILED(hr)) return Fail("list->Close", hr);
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        slotValue[slot] = ++fenceValue;
        hr = queue->Signal(fence.Get(), slotValue[slot]);
        if (FAILED(hr)) return Fail("queue->Signal", hr);
        return true;
    }

    uint64_t frame_ = 0;
};

HandRenderer::HandRenderer() : impl_(new Impl) {}
HandRenderer::~HandRenderer() = default;

bool HandRenderer::Init(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT rtFormat, const char** error) {
    const bool ok = impl_->Init(device, queue, rtFormat);
    if (!ok && error) *error = impl_->error.c_str();
    return ok;
}

bool HandRenderer::Render(const RenderTarget* targets, int count, const HandsFrame& frame, const RenderStyle& style) {
    return impl_->Render(targets, count, frame, style);
}

void HandRenderer::WaitIdle() { impl_->WaitIdle(); }
const char* HandRenderer::LastError() const { return impl_->error.c_str(); }

}  // namespace layer
