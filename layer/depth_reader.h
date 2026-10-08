// depth_reader.h — lecture de quelques pixels d'une image de profondeur Direct3D 12 (copie GPU -> CPU).
//
// Ce module ne connaît pas OpenXR : il reçoit des ressources de profondeur (ici, les images des swapchains de
// profondeur du jeu) et une liste de petites zones à copier. Une requête à la fois : on soumet, puis on interroge
// plus tard (TryCollect) sans jamais bloquer le rendu du jeu.
//
// Format attendu : ressource R32G8X24_TYPELESS (D32_FLOAT_S8X24), plan 0 = profondeur en 32 bits flottants.
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
    ID3D12Resource* depth = nullptr;  // ressource de profondeur (nullptr = œil ignoré)
    std::vector<DepthBox> boxes;
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

    // Soumet les copies à la file de commandes. `stateBefore` = état dans lequel les ressources se trouvent (elles sont
    // passées en COPY_SOURCE le temps de la copie, puis remises dans cet état). Retourne faux si une requête précédente
    // n'est pas encore terminée ou en cas d'erreur.
    bool Submit(const std::vector<DepthRequestEye>& eyes, D3D12_RESOURCE_STATES stateBefore);

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
