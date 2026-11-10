// hand_renderer.h — dessin du squelette des mains en Direct3D 12 (étape 3a).
//
// Ce module ne connaît PAS OpenXR : il reçoit un périphérique D3D12 et sa file de commandes (ceux du
// jeu), des images cibles (les images de swapchain) et des poses d'articulations. Cela permet de le
// tester seul (tests/hand_renderer_test.cpp) en rendant dans une texture que l'on relit.
//
// Dessin : chaque articulation = un petit cube, chaque os = un cube allongé entre deux articulations.
// Pas de buffer de sommets : le cube est généré dans le vertex shader à partir de SV_VertexID.
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <memory>

#include "emulation/PoseMath.h"

namespace layer {

constexpr int kHandJointCount = 26;  // XR_HAND_JOINT_COUNT_EXT

struct HandPoseData {
    bool valid = false;
    bool pressed = false;  // clic en cours : le bout du pouce et l'articulation visée s'allument en vert
    int clickJoint = 10;   // articulation touchée par le pouce pour cliquer (10 = bout de l index)
    hands::emulation::Pose joint[kHandJointCount];
    float radius[kHandJointCount] = {};
    bool jointValid[kHandJointCount] = {};
};

// Petit cube de repere dessine dans l'espace du jeu (zones apprises, retour visuel de l'apprentissage).
struct MarkerData {
    hands::emulation::Vec3 pos;
    float size = 0.01f;  // arete du cube, en metres
    float r = 1.0f, g = 1.0f, b = 1.0f;
};
constexpr int kMaxMarkers = 8;

// Boite en fil de fer (zones du pochoir) : seules les 12 aretes sont dessinees, pour ne pas cacher les commandes qu'elle designe.
struct BoxData {
    hands::emulation::Pose pose;      // centre et orientation
    hands::emulation::Vec3 size;      // dimensions totales selon les axes locaux (metres)
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float edge = 0.0016f;             // epaisseur des aretes (metres)
    bool faceOnly = false;            // seulement la face avant (+Z local, tournee vers le pilote) : un carre ou un rectangle
};
constexpr int kMaxBoxes = 96;

struct HandsFrame {
    HandPoseData hand[2];  // 0 = gauche, 1 = droite
    MarkerData marker[kMaxMarkers];
    int markerCount = 0;
    BoxData box[kMaxBoxes];
    int boxCount = 0;
    bool headValid = false;            // pose de la tete (espace du jeu)
    hands::emulation::Pose head;
};

// Point de vue d'un œil : pose dans l'espace de référence + champ de vision (angles en radians, comme OpenXR).
struct EyeView {
    hands::emulation::Pose pose;
    float angleLeft = -0.78f, angleRight = 0.78f, angleUp = 0.78f, angleDown = -0.78f;
};

struct RenderTarget {
    ID3D12Resource* resource = nullptr;  // doit être dans l'état RENDER_TARGET (comme exigé par OpenXR)
    uint32_t width = 0, height = 0;
    EyeView view;
};

// Aspect des mains : squelette (cubes), gant (volume opaque sombre), main nue (volume opaque couleur peau), main translucide.
enum HandMode { kHandSkeleton = 0, kHandGlove = 1, kHandBare = 2, kHandGhost = 3 };

struct RenderStyle {
    float jointScale = 1.0f;      // multiplicateur de la taille des articulations
    float boneThickness = 0.006f; // épaisseur des os (mètres), squelette seulement
    int mode = kHandSkeleton;
    float alpha = 0.35f;          // opacite des mains translucides
};

class HandRenderer {
public:
    HandRenderer();
    ~HandRenderer();
    HandRenderer(const HandRenderer&) = delete;
    HandRenderer& operator=(const HandRenderer&) = delete;

    // `rtFormat` : format DXGI des images cibles (celui de la swapchain). Retourne faux en cas d'échec ;
    // `error` reçoit alors une description.
    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT rtFormat, const char** error);

    // Dessine les mains dans chaque cible (effacée en transparent d'abord) puis soumet à la file de commandes.
    // Au retour, le travail GPU est SOUMIS (pas forcément terminé) : OpenXR l'ordonne par la file.
    bool Render(const RenderTarget* targets, int count, const HandsFrame& frame, const RenderStyle& style);

    // Attend la fin de tout le travail GPU soumis (avant destruction).
    void WaitIdle();

    const char* LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace layer
