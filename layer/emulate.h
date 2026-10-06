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

// Fournis par emulate.cpp : hooks supplémentaires de l'étape 1 (nullptr si inconnu).
PFN_xrVoidFunction FindEmulationHook(const char* name);

}  // namespace layer
