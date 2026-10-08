// texture_uploader.h — envoi d'une image (CPU) vers une texture Direct3D 12 (copie par la file de commandes du jeu).
//
// Sert à afficher le menu dans le casque : le menu est dessiné côté CPU, puis copié dans une image de swapchain.
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <memory>

namespace layer {

class TextureUploader {
public:
    TextureUploader();
    ~TextureUploader();
    TextureUploader(const TextureUploader&) = delete;
    TextureUploader& operator=(const TextureUploader&) = delete;

    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, const char** error);

    // Copie `w` x `h` pixels de 4 octets (`rowBytes` octets par ligne) dans le niveau 0 de `dst`. `state` = état actuel de `dst`
    // (rendu à cet état après la copie). `format` = format de lecture des octets (ex. B8G8R8A8_UNORM).
    // Attend d'abord la fin de l'envoi précédent (court : une image de menu).
    bool Upload(ID3D12Resource* dst, D3D12_RESOURCE_STATES state, DXGI_FORMAT format, const void* data, uint32_t w, uint32_t h, uint32_t rowBytes);

    void WaitIdle();
    const char* LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace layer
