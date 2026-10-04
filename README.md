# Couche OpenXR « mains nues » pour MSFS 2024

Couche OpenXR implicite (DLL 64 bits) entre MSFS 2024 et VDXR (Virtual Desktop),
pour piloter le cockpit avec les mains (Meta Quest 3). **Projet en cours : étape 0 seulement.**

## État

| Étape | Contenu | État |
|---|---|---|
| 0 | Couche transparente qui journalise (extensions, profils, actions, suivi des mains) | Écrite, **non testée en VR** |
| 1 | Contrôleur émulé au bout de l'index + appui | à faire |
| 2 | Swipe / maintien | à faire |
| 3 | Mains en surimpression (DX12) | à faire |
| 4 | Finitions | à faire |

## Ce qui est vérifié, ce qui ne l'est pas (étape 0)

| Élément | Statut |
|---|---|
| Cœur portable (INI, config à chaud, journal asynchrone, utilitaires) : compilation + 13 tests unitaires | **Vérifié** (Linux, g++ 13) |
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
layer/     couche OpenXR Windows (entry, layer, hooks)
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
