// servo.h — mode souris « collé au doigt » : asservissement visuel du pointeur de MSFS.
//
// A chaque image : (1) on lit un morceau (ROI) de l'image que le jeu a rendue pour un oeil (retour GPU -> CPU asynchrone, une requete a la fois) ;
// (2) le detecteur (core/servo/PointerDetect) y trouve la fleche blanche de MSFS ; (3) la boucle (core/servo/ServoController) compare sa direction a celle du
// bout de l'index vu du meme oeil et calcule des comptes de souris ; (4) un fil dedie les injecte (mouse_click.cpp). Aucun modele de la facon dont MSFS
// convertit la souris en rayon : on regarde le resultat. Au demarrage, quatre salves de comptes connus mesurent le gain et la latence (GainProbe).
#pragma once

#include <d3d12.h>
#include <openxr/openxr.h>

#include <string>

#include "hand_renderer.h"

namespace layer {

// Image couleur du jeu derriere la vue `eye` de la couche de projection (la derniere image terminee). Fournie par hands_overlay.cpp (qui suit les swapchains).
struct EyeImage {
    ID3D12Resource* tex = nullptr;
    uint32_t texW = 0, texH = 0;
    int rectX = 0, rectY = 0, rectW = 0, rectH = 0;  // subImage.imageRect : zone effectivement rendue (jamais supposee egale a la ressource)
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
};
bool Overlay_GetEyeImage(const XrCompositionLayerProjection* game, int eye, EyeImage* out);

// Un appel par xrEndFrame tant que le mode souris est « collé au doigt ». `menuOpen` : le menu VR est ouvert (pas de clic, bouton relache).
void Servo_Frame(const XrCompositionLayerProjection* game, const HandsFrame& frame, bool menuOpen, ID3D12Device* device, ID3D12CommandQueue* queue);

// Arrete l'asservissement (relache le bouton, attend la fin des copies GPU, oublie l'etat). Appelee au changement de type de souris, d'oeil, a la fin de session.
void Servo_Stop();

// Une swapchain du jeu va etre detruite : les copies en cours pourraient lire une image sur le point d'etre liberee.
void Servo_OnSwapchainDestroy();

// Refait la mesure du gain et de la latence (une seconde environ).
void Servo_Recalibrate();

// Etat en une ligne pour le menu.
std::string Servo_StatusText();

}  // namespace layer
