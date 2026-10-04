# Composants tiers

| Composant | Fichiers | Licence | Origine |
|---|---|---|---|
| En-têtes OpenXR 1.0.20 (`openxr.h`, `openxr_platform.h`, `openxr_platform_defines.h`) | `third_party/openxr/include/openxr/` | `Apache-2.0 OR MIT` (identifiant SPDX présent dans chaque fichier) | Paquet Ubuntu `libopenxr-dev` 1.0.20 (copie du Khronos Group, OpenXR-SDK) |

Notes :
- Version ancienne des en-têtes (1.0.20) : suffisante pour l'étape 0 (hand tracking EXT présent) ; à mettre à jour avant l'étape 3 si besoin.
- `layer/loader_negotiation.h` est **écrit à la main** d'après la spécification du loader (le paquet ne contient pas l'en-tête officiel). Voir l'avertissement dans le fichier.
- Les projets de référence (HTCC, OpenXR Toolkit, OpenXR-Layer-Template) sont, d'après leurs pages GitHub, sous licence MIT. **Aucun de leurs codes n'a été copié** à ce stade. Si un extrait est repris plus tard, il sera listé ici avec sa mention de copyright.
