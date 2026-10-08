// depth_reader.h — lecture de quelques pixels d'une image de profondeur Direct3D 12 (copie GPU -> CPU).
//
// Ce module ne connaît pas OpenXR : il reçoit des ressources de profondeur (ici, les images des swapchains de
// profondeur du jeu) et une liste de petites zones à copier. Une requête à la fois : on soumet, puis on interroge
// plus tard (TryCollect) sans jamais bloquer le rendu du jeu.
//
// Formats : voir DepthRequestEye::format (plan profondeur d'une ressource R32G8X24, ou image couleur 8 bits).
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace layer {

struct DepthBox {
    uint32_t x = 0, y = 0, w = 1, h = 1;
};

struct DepthRequestEye {
    ID3D12Resource* depth = nullptr;  // ressource a lire (nullptr = oeil ignore)
    std::vector<DepthBox> boxes;
    // Etat dans lequel la ressource se trouve (elle est passee en COPY_SOURCE le temps de la copie, puis remise dans cet etat).
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    // Format de lecture : R32_TYPELESS pour le plan profondeur de R32G8X24 ; R8G8B8A8_UNORM pour une image couleur 8 bits.
    // Dans tous les cas 4 octets par pixel, rendus tels quels dans `boxes` (un flottant de 32 bits = un mot de 4 octets).
    DXGI_FORMAT format = DXGI_FORMAT_R32_TYPELESS;
};

struct DepthResultEye {
    uint32_t texWidth = 0, texHeight = 0;
    std::vector<std::vector<float>> boxes;  // une liste de valeurs (ligne par ligne) par zone demandée ; vide si hors image
};

class DepthReader {
public:
    DepthReader();
    ~DepthReader();
    DepthReader(const DepthReader&) = delete;
    DepthReader& operator=(const DepthReader&) = delete;

    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, const char** error);

    // Soumet les copies à la file de commandes. Retourne faux si une requête précédente n'est pas encore terminée ou en cas d'erreur.
    bool Submit(const std::vector<DepthRequestEye>& eyes);

    bool Pending() const;

    // Si la dernière requête est terminée côté GPU, remplit `out` (un élément par œil demandé) et retourne vrai.
    bool TryCollect(std::vector<DepthResultEye>* out);

    void WaitIdle();
    const char* LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace layer
