// depth_reader.cpp — voir depth_reader.h.
#include "depth_reader.h"

#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace layer {

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT64 kReadbackBytes = 4 * 1024 * 1024;
constexpr UINT kPitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;          // 256
constexpr UINT64 kPlacementAlign = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;  // 512

UINT64 AlignUp(UINT64 v, UINT64 a) { return (v + a - 1) / a * a; }
}  // namespace

struct DepthReader::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> readback;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;
    bool pending = false;
    std::string error;

    // Disposition de la dernière requête, pour décoder le tampon de relecture
    struct Placed {
        UINT64 offset = 0;
        uint32_t w = 0, h = 0, rowPitch = 0;
        bool valid = false;
    };
    std::vector<std::vector<Placed>> placed;  // par œil, par zone
    std::vector<std::pair<uint32_t, uint32_t>> texSize;

    ~Impl() {
        if (event) {
            WaitIdle();
            CloseHandle(event);
        }
    }

    bool Fail(const char* msg, HRESULT hr = S_OK) {
        char buf[200];
        if (hr != S_OK) std::snprintf(buf, sizeof(buf), "%s (HRESULT 0x%08lX)", msg, static_cast<unsigned long>(hr));
        else std::snprintf(buf, sizeof(buf), "%s", msg);
        error = buf;
        return false;
    }

    void WaitIdle() {
        if (!queue || !fence || !event) return;
        const UINT64 v = ++fenceValue;
        if (FAILED(queue->Signal(fence.Get(), v))) return;
        if (fence->GetCompletedValue() < v) {
            fence->SetEventOnCompletion(v, event);
            WaitForSingleObject(event, 5000);
        }
        pending = false;
    }

    bool Init(ID3D12Device* dev, ID3D12CommandQueue* q) {
        device = dev;
        queue = q;
        if (!device || !queue) return Fail("peripherique ou file de commandes absents");
        HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (FAILED(hr)) return Fail("CreateCommandAllocator", hr);
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
        if (FAILED(hr)) return Fail("CreateCommandList", hr);
        list->Close();
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr)) return Fail("CreateFence", hr);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return Fail("CreateEvent");
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kReadbackBytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        if (FAILED(hr)) return Fail("CreateCommittedResource(relecture)", hr);
        return true;
    }

    bool Submit(const std::vector<DepthRequestEye>& eyes, D3D12_RESOURCE_STATES stateBefore) {
        if (pending) return false;
        HRESULT hr = alloc->Reset();
        if (FAILED(hr)) return Fail("allocator->Reset", hr);
        hr = list->Reset(alloc.Get(), nullptr);
        if (FAILED(hr)) return Fail("list->Reset", hr);

        placed.assign(eyes.size(), {});
        texSize.assign(eyes.size(), {0, 0});
        UINT64 cursor = 0;
        std::vector<D3D12_RESOURCE_BARRIER> barriers;

        // 1) passer chaque ressource utilisée en COPY_SOURCE
        for (const auto& e : eyes) {
            if (!e.depth || e.boxes.empty()) continue;
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = e.depth;
            b.Transition.StateBefore = stateBefore;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers.push_back(b);
        }
        if (!barriers.empty()) list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

        // 2) copies
        for (size_t i = 0; i < eyes.size(); ++i) {
            const DepthRequestEye& e = eyes[i];
            placed[i].assign(e.boxes.size(), {});
            if (!e.depth) continue;
            const D3D12_RESOURCE_DESC d = e.depth->GetDesc();
            texSize[i] = {static_cast<uint32_t>(d.Width), d.Height};
            for (size_t k = 0; k < e.boxes.size(); ++k) {
                DepthBox b = e.boxes[k];
                if (b.x >= d.Width || b.y >= d.Height || b.w == 0 || b.h == 0) continue;
                b.w = std::min<uint32_t>(b.w, static_cast<uint32_t>(d.Width) - b.x);
                b.h = std::min<uint32_t>(b.h, d.Height - b.y);
                const UINT rowPitch = static_cast<UINT>(AlignUp(static_cast<UINT64>(b.w) * 4, kPitchAlign));
                const UINT64 bytes = static_cast<UINT64>(rowPitch) * b.h;
                cursor = AlignUp(cursor, kPlacementAlign);
                if (cursor + bytes > kReadbackBytes) continue;  // plus de place : zone ignorée
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = readback.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Offset = cursor;
                dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_TYPELESS;  // plan « profondeur » de R32G8X24
                dst.PlacedFootprint.Footprint.Width = b.w;
                dst.PlacedFootprint.Footprint.Height = b.h;
                dst.PlacedFootprint.Footprint.Depth = 1;
                dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
                src.pResource = e.depth;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;  // mip 0, tranche 0, plan 0 (profondeur)
                const D3D12_BOX box{b.x, b.y, 0, b.x + b.w, b.y + b.h, 1};
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                placed[i][k] = {cursor, b.w, b.h, rowPitch, true};
                cursor += bytes;
            }
        }

        // 3) remettre les ressources dans leur état d'origine
        for (auto& b : barriers) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        if (!barriers.empty()) list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

        hr = list->Close();
        if (FAILED(hr)) return Fail("list->Close", hr);
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        hr = queue->Signal(fence.Get(), ++fenceValue);
        if (FAILED(hr)) return Fail("queue->Signal", hr);
        pending = true;
        return true;
    }

    bool TryCollect(std::vector<DepthResultEye>* out) {
        if (!pending) return false;
        if (fence->GetCompletedValue() < fenceValue) return false;
        pending = false;
        D3D12_RANGE range{0, static_cast<SIZE_T>(kReadbackBytes)};
        uint8_t* mapped = nullptr;
        if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped) return Fail("Map");
        out->assign(placed.size(), {});
        for (size_t i = 0; i < placed.size(); ++i) {
            (*out)[i].texWidth = texSize[i].first;
            (*out)[i].texHeight = texSize[i].second;
            (*out)[i].boxes.assign(placed[i].size(), {});
            for (size_t k = 0; k < placed[i].size(); ++k) {
                const Placed& p = placed[i][k];
                if (!p.valid) continue;
                std::vector<float>& v = (*out)[i].boxes[k];
                v.resize(static_cast<size_t>(p.w) * p.h);
                for (uint32_t row = 0; row < p.h; ++row)
                    std::memcpy(&v[static_cast<size_t>(row) * p.w], mapped + p.offset + static_cast<UINT64>(row) * p.rowPitch,
                                static_cast<size_t>(p.w) * 4);
            }
        }
        D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
        return true;
    }
};

DepthReader::DepthReader() : impl_(new Impl) {}
DepthReader::~DepthReader() = default;
bool DepthReader::Init(ID3D12Device* device, ID3D12CommandQueue* queue, const char** error) {
    const bool ok = impl_->Init(device, queue);
    if (!ok && error) *error = impl_->error.c_str();
    return ok;
}
bool DepthReader::Submit(const std::vector<DepthRequestEye>& eyes, D3D12_RESOURCE_STATES stateBefore) {
    return impl_->Submit(eyes, stateBefore);
}
bool DepthReader::Pending() const { return impl_->pending; }
bool DepthReader::TryCollect(std::vector<DepthResultEye>* out) { return impl_->TryCollect(out); }
void DepthReader::WaitIdle() { impl_->WaitIdle(); }
const char* DepthReader::LastError() const { return impl_->error.c_str(); }

}  // namespace layer
