// texture_uploader.cpp — voir texture_uploader.h.
#include "texture_uploader.h"

#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace layer {

using Microsoft::WRL::ComPtr;

namespace {
UINT64 AlignUp(UINT64 v, UINT64 a) { return (v + a - 1) / a * a; }
}  // namespace

struct TextureUploader::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> upload;
    UINT64 uploadSize = 0;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;
    std::string error;

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
        if (!fence || !event) return;
        if (fence->GetCompletedValue() >= fenceValue) return;
        fence->SetEventOnCompletion(fenceValue, event);
        WaitForSingleObject(event, 5000);
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
        return true;
    }

    bool Upload(ID3D12Resource* dst, D3D12_RESOURCE_STATES state, DXGI_FORMAT format, const void* data, uint32_t w, uint32_t h, uint32_t rowBytes) {
        if (!dst || !data || w == 0 || h == 0) return Fail("parametres invalides");
        WaitIdle();  // le tampon d'envoi et l'allocateur ne sont réutilisables qu'après la fin de l'envoi précédent
        const UINT rowPitch = static_cast<UINT>(AlignUp(static_cast<UINT64>(w) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
        const UINT64 needed = static_cast<UINT64>(rowPitch) * h;
        if (!upload || uploadSize < needed) {
            upload.Reset();
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = needed;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            const HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
            if (FAILED(hr)) return Fail("CreateCommittedResource(envoi)", hr);
            uploadSize = needed;
        }
        uint8_t* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        if (FAILED(upload->Map(0, &none, reinterpret_cast<void**>(&mapped))) || !mapped) return Fail("Map");
        const uint8_t* src = static_cast<const uint8_t*>(data);
        for (uint32_t y = 0; y < h; ++y) std::memcpy(mapped + static_cast<size_t>(y) * rowPitch, src + static_cast<size_t>(y) * rowBytes, static_cast<size_t>(w) * 4);
        D3D12_RANGE written{0, static_cast<SIZE_T>(needed)};
        upload->Unmap(0, &written);

        HRESULT hr = alloc->Reset();
        if (FAILED(hr)) return Fail("allocator->Reset", hr);
        hr = list->Reset(alloc.Get(), nullptr);
        if (FAILED(hr)) return Fail("list->Reset", hr);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = dst;
        b.Transition.StateBefore = state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
        dl.pResource = dst;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dl.SubresourceIndex = 0;
        sl.pResource = upload.Get();
        sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint.Offset = 0;
        sl.PlacedFootprint.Footprint.Format = format;
        sl.PlacedFootprint.Footprint.Width = w;
        sl.PlacedFootprint.Footprint.Height = h;
        sl.PlacedFootprint.Footprint.Depth = 1;
        sl.PlacedFootprint.Footprint.RowPitch = rowPitch;
        list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);

        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        hr = list->Close();
        if (FAILED(hr)) return Fail("list->Close", hr);
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        hr = queue->Signal(fence.Get(), ++fenceValue);
        if (FAILED(hr)) return Fail("queue->Signal", hr);
        return true;
    }
};

TextureUploader::TextureUploader() : impl_(new Impl) {}
TextureUploader::~TextureUploader() = default;
bool TextureUploader::Init(ID3D12Device* device, ID3D12CommandQueue* queue, const char** error) {
    const bool ok = impl_->Init(device, queue);
    if (!ok && error) *error = impl_->error.c_str();
    return ok;
}
bool TextureUploader::Upload(ID3D12Resource* dst, D3D12_RESOURCE_STATES state, DXGI_FORMAT format, const void* data, uint32_t w, uint32_t h, uint32_t rowBytes) {
    return impl_->Upload(dst, state, format, data, w, h, rowBytes);
}
void TextureUploader::WaitIdle() { impl_->WaitIdle(); }
const char* TextureUploader::LastError() const { return impl_->error.c_str(); }

}  // namespace layer
