// zones.h — pochoir : zones apprises au bout de l'index, qui commandent le simulateur par le pont SimConnect (HandsBridge.exe).
//
// Types de commande (comme les touches de StreamSim, voir core/sim/ZoneControl.h) :
//   poussoir / bascule   toucher la zone (le bout de l'index y reste `[zones] dwell_ms`) envoie la sequence `press` ;
//   interrupteur         chaque contact passe a la position suivante (cycle ou aller-retour) ;
//   rotatif              pincer dans la zone puis tourner le poignet : un cran tous les `step_deg` degres ; pincer-relacher = appui ;
//   curseur              deux points appris (debut et fin de course) ; pincer et glisser, par valeur ou par crans.
// Apprentissage : bouton de la tray ou Ctrl+Alt+F9 (premiere zone incomplete), ou menu en VR (zone choisie). Positions dans l'espace du jeu,
// enregistrees dans `zones.ini` (data a cote de la DLL) ; si le cockpit est recale d'une session a l'autre, il faut reapprendre les zones.
#pragma once

#include <openxr/openxr.h>

#include <string>
#include <vector>

#include "hand_renderer.h"

namespace layer {

// Appelé à chaque xrEndFrame avec les mains (dans l'espace du jeu). Sans effet si `[zones] enabled = false`.
void Zones_Frame(const HandsFrame& frame);

// Ajoute au rendu des mains les reperes des zones apprises (magenta ; jaune quand le doigt est dedans ; vert au declenchement ; blanc a
// l'apprentissage) et le repere de l'apprentissage en cours (au bout de l'index).
void Zones_FillMarkers(HandsFrame* frame);

// --- Pilotage depuis le menu en VR (vr_menu.cpp) --------------------------------------------------------------------------------
struct ZoneInfo {
    std::string id;
    std::wstring text;  // « nom — cible »
    bool hasPos = false;
};
std::vector<ZoneInfo> Zones_List();

// Commandes proposees : bibliotheque du Rafale (core/sim/CommandLibrary.inc) puis Input Events lus dans l'avion (Zones_RefreshInputs).
size_t Zones_CommandCount();
std::wstring Zones_CommandLabel(size_t index);
void Zones_RefreshInputs();  // lecture asynchrone via le pont (MSFS doit tourner)
bool Zones_InputsLoaded();

// Cree une zone pour la commande choisie et lance son apprentissage (compte a rebours puis memorisation du bout de l'index).
void Zones_AddCommand(size_t index);
// Relance l'apprentissage de la position d'une zone existante.
void Zones_Reteach(const std::string& id);

}  // namespace layer
