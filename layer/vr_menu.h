// vr_menu.h — menu des réglages affiché dans le casque (panneau flottant, piloté par les mains).
//
// Ouverture/fermeture : pincer les DEUX mains en même temps pendant `[menu] open_hold_ms`. Une fois ouvert, on vise une ligne avec le
// rayon habituel et on pince pour la choisir. Chaque choix est écrit dans hands.ini (voir Layer_SetConfigValue) et pris en compte aussitôt.
// Pendant que le menu est ouvert, les pincements ne sont plus transmis au jeu.
#pragma once

#include <d3d12.h>

#include "hand_renderer.h"
#include "state.h"


namespace layer {

// Appelé à chaque xrEndFrame. Met à jour le menu (geste, pointage, choix) et, s'il est ouvert, remplit `quad` (couche de composition à
// ajouter aux couches du jeu) et retourne vrai. `game` = couche de projection du jeu (donne l'espace de référence), `frame` = mains.
bool VrMenu_Frame(XrSession session, ID3D12Device* device, ID3D12CommandQueue* queue, const XrCompositionLayerProjection* game, XrTime displayTime,
                  const HandsFrame& frame, XrCompositionLayerQuad* quad);

// xrEndFrame a refusé la couche du menu : au bout de quelques refus le menu est désactivé pour la session.
void VrMenu_NoteEndFrameFailure();

// Détruit la swapchain et remet tout à zéro (avant la destruction de la session).
void VrMenu_Stop();

}  // namespace layer
