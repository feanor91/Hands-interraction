// mouse_click.h — mains = souris de Windows (mode souris).
//
// Deux types :
//   * « collé au doigt » (Servo, par defaut) : le pointeur de MSFS est amene sous le bout de l'index par une boucle qui REGARDE ou le jeu le dessine (servo.h) ;
//   * « souris de bureau » (Desktop) : le deplacement de la main (plan de la vue) devient un deplacement relatif de la souris ; index replie = souris soulevee.
// Dans les deux cas : pincer = bouton gauche enfonce, garder le pincement = glisser (verriere, curseurs).
//
// L'injection se fait dans un fil dedie, en evenements de 8 comptes au plus (MSFS lit des deplacements, pas des positions : jamais de SendInput absolu).
#pragma once

#include "hand_renderer.h"

namespace layer {

enum class MouseKind { Off, Servo, Desktop };

MouseKind Mouse_Kind();
void Mouse_SetKind(MouseKind k);  // Off relache le bouton et arrete l'asservissement
bool Mouse_ModeOn();
void Mouse_ToggleMode();          // Off <-> dernier type actif (Servo au depart) ; Ctrl+Alt+F11 aussi
bool Mouse_Attached();            // le type actif ou choisi est « collé au doigt »
void Mouse_SetAttached(bool on);  // choisit le type (s'applique tout de suite si le mode est actif)

// Appelee a chaque xrEndFrame : touche de secours Ctrl+Alt+F11 et mode souris de bureau.
void Mouse_Frame(const HandsFrame& frame);

// Sensibilite de la souris de bureau (pixels par metre de deplacement de la main) x `factor`, enregistree dans mouse.ini.
void Mouse_Gain(double factor);

// Oeil dans l'image duquel on cherche le pointeur (0 = gauche : c'est de lui que MSFS tire le rayon, 1 = droit : si c'est l'oeil directeur), enregistre.
int Mouse_Eye();
void Mouse_SetEye(int eye);

// --- injection (utilisee aussi par l'asservissement) : file d'attente, jamais bloquante ---------------------------------------------------------
void MouseInject_Move(int dx, int dy);  // comptes relatifs (decoupes en evenements de 8 comptes au plus)
void MouseInject_Button(bool leftDown);
void MouseInject_ReleaseIfHeld();       // relache le bouton gauche s'il est enfonce par nous

}  // namespace layer
