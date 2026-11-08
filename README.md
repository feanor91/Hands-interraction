# Couche OpenXR « mains nues » pour MSFS 2024

Couche OpenXR implicite (DLL 64 bits) entre MSFS 2024 et VDXR (Virtual Desktop),
pour piloter le cockpit avec les mains (Meta Quest 3). **Projet arrêté après l'étape 3a : l'interaction aux mains n'est pas assez précise (voir « Bilan »).**

## État

| Étape | Contenu | État |
|---|---|---|
| 0 | Couche transparente qui journalise (extensions, profils, actions, suivi des mains) | **Testée en VR** (MSFS 2024 Store + VDXR + Quest 3, option « forward tracking » de Virtual Desktop requise) |
| 1 | Contrôleur émulé + clic par le pouce (index, côté de l'index, majeur, ou « au plus proche »), deux mains actives | **Testée en VR** : le clic et le glisser des boutons rotatifs fonctionnent |
| 3a | Squelette des mains en surimpression (D3D12), menu de réglages dans le casque, application de barre des tâches (mise à jour, options) | **Testée en VR** |
| 2, 3b, 3c, 4 | Swipe, mains stylisées, modèles réalistes, finitions | abandonnées |

## Bilan

Ce qui marche : le suivi des mains est exposé à MSFS, le clic se déclenche de façon fiable, le squelette des mains s'affiche, le menu VR et la tray (installation, mises à jour, options) sont utilisables.

Ce qui ne marche pas assez bien : **la précision du rayon**. Le suivi des mains tremble de quelques millimètres et le geste de clic bouge la main. MSFS attend un rayon de manette précis, sans zone d'aide ; le rayon est ici estimé (épaule, poignet ou paume vers le doigt, plusieurs modes), ce qui amplifie les écarts. En test réel, manettes ou souris sont plus efficaces, et les tablettes du cockpit (petites cibles, défilement) sont presque inutilisables.

Essayé sans résoudre : rayon par l'épaule estimée (torse lent, mélange avec la direction du doigt), lissage One Euro, verrouillage de la pose avant le clic (pré-verrouillage dès l'approche du pouce, zone morte). Voies fermées : profondeur (MSFS n'acquiert jamais ses swapchains de profondeur) ; SimConnect / WASM (pas de « clic sous le rayon » générique, seulement des variables par avion) ; masquer le modèle de manette (`hide_controller_model` est sans effet).

Piste non explorée : accroche sur la cible. MSFS dessine une flèche bleue quand le rayon survole un élément interactif ; la détecter dans l'image du jeu permettrait de figer le rayon sur l'élément.

## Zones (pochoir) : essai

Piste ouverte après le bilan : au lieu d'un rayon, des **zones sphériques** apprises au bout de l'index, qui déclenchent une touche de
StreamSim (`POST /api/press`) au contact (maintien `dwell_ms`). Essai sur le frein de parc (touche « Frein libre » de la page MSFS : profil `default`, page `od16uzxc`, touche 1) ; la batterie n'existe pas sur le Rafale.
Activé par défaut (`[zones] enabled`, ou menu de la tray *Zones (pochoir)*) ; sans zone apprise, il ne fait rien.

* Mémoriser : poser le bout de l'index sur la commande, puis appuyer sur un **bouton de joystick / HOTAS** choisi dans la tray (*Zones >
  Choisir le bouton…*, on appuie sur le bouton voulu) ; la zone est enregistrée `teach_delay_ms` plus tard (bip de confirmation). Repli au clavier :
  Ctrl+Alt+F9 (apprendre), Ctrl+Alt+F10 (tout effacer). Les zones sont dans `zones.ini`, à côté de la DLL, dans l'espace du jeu : à réapprendre si le
  cockpit est recalé.
* Code : `core/emulation/ZoneTracker.h` (maintien et réarmement, testé), `layer/zones.cpp`, `win/JoyPoll.*` (DirectInput, partagé avec la tray).

## Ce qui est vérifié, ce qui ne l'est pas (étape 0)

| Élément | Statut |
|---|---|
| Cœur portable (INI, config à chaud, journal asynchrone, utilitaires) : compilation + 15 tests unitaires | **Vérifié** (Linux, g++ 13) |
| Compilation de la couche avec MinGW-w64 ; la DLL exporte `xrNegotiateLoaderApiLayerInterface` | **Vérifié** (syntaxe/édition de liens uniquement, pas MSVC) |
| Compilation avec Visual Studio 2022 / MSVC | **Non vérifié** |
| Scripts PowerShell (install / uninstall / check-process) | **Non vérifiés** (aucun PowerShell disponible ici ; relus à la main) |
| Chargement par le loader OpenXR, négociation, structures `loader_negotiation.h` écrites à la main | **Non vérifié** |
| Comportement avec MSFS 2024 Store, VDXR, `XR_EXT_hand_tracking`, droits d'écriture du log | **Non vérifié** : c'est l'objet du test de l'étape 0 |
| Nom exact de l'exécutable de MSFS 2024 (`FlightSimulator2024.exe` n'est qu'une hypothèse) | **Non vérifié** |

## Compilation (Visual Studio 2022)

Prérequis : VS 2022 (charge de travail « Développement Desktop en C++ »), CMake ≥ 3.20, Git.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure   # tests du coeur
```

La DLL, le manifeste et `hands.ini` se retrouvent dans `build\layer\Release\`.

## Installation / désinstallation (PowerShell **64 bits, en administrateur**)

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\uninstall.ps1 -RemoveFiles
```

Fichiers installés dans `C:\ProgramData\HandsLayer\` (DLL, manifeste, `hands.ini`, `logs\`).

## Arborescence

```
core/      cœur portable C++17 (config INI, journal, utilitaires) — testé sous Linux
layer/     couche OpenXR Windows (entry, layer, hooks, émulation, surimpression, menu VR)
tray/      application de barre des tâches (HandsTray)
manifest/  manifeste JSON de la couche
config/    hands.ini d'exemple (copié à côté de la DLL)
scripts/   install.ps1, uninstall.ps1, check-process.ps1
tests/     tests unitaires du cœur (mini cadre maison, sans dépendance)
docs/procedures-test/   procédure de test à exécuter dans le casque, par étape
third_party/openxr/     en-têtes OpenXR officiels (voir THIRD_PARTY.md)
cmake/     toolchain MinGW (vérification de compilation depuis Linux)
```

## Choix de sécurité

* **Fail-open** : toute erreur => la couche laisse passer sans rien modifier. Chaque hook appelle toujours la vraie fonction, en dehors de tout bloc `try`.
* Hors processus autorisé (hors mode découverte) : aucun log, aucun thread, aucun hook.
* Étape 0 : la seule modification du flux OpenXR est l'ajout de `XR_EXT_hand_tracking` aux extensions (désactivable : `enable_hand_tracking_extension = false`) ; si le runtime refuse, la création est retentée sans modification.
* Licence du projet : à choisir.
