// emulate.h — étape 1 : contrôleur émulé au bout de l'index, gâchette = pincement.
//
// Principe : le jeu lit la pose des actions « aim_pose » / « grip_pose » avec xrLocateSpace
// et l'état de la gâchette avec xrGetActionStateBoolean/Float. Quand le suivi des mains est
// valide, on remplace ces valeurs (pose = bout de l'index, appui = pincement pouce-index).
// Sinon (main non suivie, extension absente...) tout passe tel quel : fail-open.
#pragma once

#include <string>

#include "state.h"

namespace layer {

// Création de session : crée les « hand trackers » si l'émulation est activée. Retourne vrai
// si l'émulation est démarrée (la sonde de l'étape 0 n'a alors plus à créer/détruire les siens).
bool Emu_Start(XrInstance instance, XrSession session);
// Destruction de session / d'instance : détruit les hand trackers et vide les tables.
void Emu_Stop(XrSession session);

// Enregistrement des actions et espaces d'action créés par le jeu (voir hooks_probe.cpp).
void Emu_OnAction(XrAction action, const std::string& fullName);
void Emu_OnActionSpace(XrAction action, XrPath subactionPath, XrSpace space);

// Localise les 26 articulations de la main `hand` (0 gauche, 1 droite) dans `base` à l'instant `time`,
// pour le dessin. `out` doit pouvoir contenir XR_HAND_JOINT_COUNT_EXT éléments. `pressed` (facultatif)
// reçoit l'état du pincement. Retourne faux si la main n'est pas suivie ou si le suivi n'est pas démarré.
bool Emu_LocateForRender(int hand, XrSpace base, XrTime time, XrHandJointLocationEXT* out, bool* pressed, int* clickJoint);

// Derniere pose de pointeur (celle que le jeu recoit) de la main `hand`, si elle est suivie. Sert aux captures de diagnostic.
bool Emu_LastPointerPose(int hand, XrPosef* out);

// Menu ouvert : les clics (pincements) ne sont plus transmis au jeu, qui voit la gachette relachee.
void Emu_SetMenuOpen(bool open);

// Pose de la tete dans `base` a l'instant `time` (pour placer le menu devant l'utilisateur). Faux si indisponible.
bool Emu_HeadPose(XrSpace base, XrTime time, XrPosef* out);

// --- Mode « mains seules » (voir [hands] hide_controllers dans Config.h) ---------------------------------------------------
// Une action liée (par une suggestion de bindings) à un chemin /user/hand/... : seules ces actions sont masquées (pas le regard).
void Emu_OnHandBinding(XrAction action);
// Mise à jour une fois par image (xrEndFrame) : lit l'état de suivi de chaque main, filtre (maintien) et décide quelles manettes masquer.
void Emu_UpdateHandPresence(XrTime displayTime);
// La manette virtuelle de ce chemin de haut niveau (/user/hand/left ou right) est-elle déclarée « aucun profil » au jeu ?
bool Emu_ShouldHideProfile(XrPath topLevelUserPath);
// Évènement « profil d'interaction changé » synthétique en attente (à livrer par xrPollEvent quand le runtime n'a rien d'autre) ?
bool Emu_TakePendingProfileEvent(XrSession* session);

// Fournis par emulate.cpp : hooks supplémentaires de l'étape 1 (nullptr si inconnu).
PFN_xrVoidFunction FindEmulationHook(const char* name);

}  // namespace layer
