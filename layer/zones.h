// zones.h — pochoir : zones sphériques apprises au bout de l'index, qui déclenchent une touche de StreamSim au contact.
//
// Apprentissage : Ctrl+Alt+F9 lance un compte a rebours (5 s, un bip par seconde) puis enregistre la position du bout de l'index (main droite,
// sinon gauche) comme zone "frein_parc" : le temps de poser le doigt sur la commande ; Ctrl+Alt+F10
// efface toutes les zones. Les positions sont dans l'espace du jeu (celui des couches de projection) et sont enregistrées dans
// `zones.ini` à côté de la DLL ; si le cockpit est recalé d'une session à l'autre, il faut réapprendre la zone.
// Contact : le bout de l'index reste `[zones] dwell_ms` dans la zone -> POST http://127.0.0.1:<server_port>/api/press (touche StreamSim).
#pragma once

#include <openxr/openxr.h>

#include "hand_renderer.h"

namespace layer {

// Appelé à chaque xrEndFrame avec les mains (dans l'espace du jeu). Sans effet si `[zones] enabled = false`.
void Zones_Frame(const HandsFrame& frame);

// Ajoute au rendu des mains les reperes des zones apprises (magenta ; jaune quand le doigt est dedans ; vert au declenchement ; blanc a
// l'apprentissage) et le repere de l'apprentissage en cours (au bout de l'index).
void Zones_FillMarkers(HandsFrame* frame);

}  // namespace layer
