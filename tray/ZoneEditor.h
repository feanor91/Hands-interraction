// ZoneEditor.h — fenetre de configuration des zones du pochoir (liste, choix de la commande, essai, enregistrement dans zones.ini).
#pragma once

#include <windows.h>

// Ouvre la fenetre (ou la ramene au premier plan si elle existe deja).
void ZoneEditor_Show(HWND owner, HINSTANCE inst);
