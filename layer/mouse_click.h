// mouse_click.h — mains = souris de Windows. En « mode souris », le pointeur de MSFS suit le bout de l'index (on projette le bout du doigt dans la vue ecran
// de MSFS, qui suit la tete) ; pincer = bouton gauche enfonce, relacher = relache (donc glisser possible : verriere, curseurs). La souris sait tout manipuler
// dans le cockpit, et ne depend d'aucune zone apprise (un interrupteur qui bouge ne pose plus de probleme).
//
// Touches : Ctrl+Alt+F11 active / desactive le mode souris ; Ctrl+Alt+F10 etalonne (une fois) : 5 reperes blancs apparaissent l'un apres l'autre, fixes
// devant les yeux ; avec la vraie souris, posez le pointeur de MSFS sur le repere puis Ctrl+Alt+F10 ; apres le 5e, le resultat va dans mouse.ini.
#pragma once

#include "hand_renderer.h"

namespace layer {

// A chaque image : etalonnage, mode souris.
void Mouse_Frame(const HandsFrame& frame);
// Reperes de l'etalonnage (suivent la tete).
void Mouse_FillMarkers(HandsFrame* frame);
bool Mouse_IsCalibrated();
bool Mouse_Calibrating();
bool Mouse_ModeOn();
void Mouse_ToggleMode();
// Lance l'etalonnage (comme Ctrl+Alt+F10) ; les reperes suivants se prennent avec Ctrl+Alt+F10.
void Mouse_StartCalibration();

}  // namespace layer
