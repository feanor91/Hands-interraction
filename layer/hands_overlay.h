// hands_overlay.h — étape 3a : ajoute une couche de composition (les mains dessinées) à chaque xrEndFrame.
#pragma once

#include "state.h"

namespace layer {

// Appelé par le hook xrCreateSession quand la création contient une liaison graphique Direct3D 12
// (XrGraphicsBindingD3D12KHR). `device` = ID3D12Device*, `queue` = ID3D12CommandQueue* (ceux du jeu).
void Overlay_OnGraphicsBinding(void* device, void* queue);

// Détruit swapchains et ressources GPU (avant la destruction de la session ou de l'instance).
void Overlay_Stop();

// Demande de recaler le centre de la portee des mains sur la position et le regard actuels (prise en compte a la prochaine image).
void Overlay_RecenterReach();

// Hooks fournis par ce module (xrEndFrame) ; nullptr si le nom est inconnu.
PFN_xrVoidFunction FindOverlayHook(const char* name);

}  // namespace layer
