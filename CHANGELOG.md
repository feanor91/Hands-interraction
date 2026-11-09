# Journal des versions

Format : [Semantic Versioning](https://semver.org/lang/fr/). La version courante est dans le fichier `VERSION`.

## [0.0.1] - 2026-11-09

Premiere version publiee : l'interaction aux mains par **pochoir de zones** (la voie du rayon, trop imprecise, est conservee en option).

### Ajoute
- Couche OpenXR « mains nues » (MSFS 2024 Store + Virtual Desktop VDXR + Quest 3) : suivi des mains expose au jeu, squelette des mains en surimpression.
- Mode « mains seules » : masque les manettes virtuelles que VDXR fabrique a partir du suivi des mains (rayon, clics et manettes fantomes).
- Pochoir : zones apprises au bout de l'index (bouton de HOTAS, ou menu en VR) qui commandent le simulateur. Types de commande : poussoir, bascule,
  interrupteur a N positions, rotatif (pincer et tourner), curseur (pincer et glisser). Actions : evenement SimConnect, variable (L: ou SimVar),
  Input Event de MSFS 2024, code avionique (H:, par MobiFlight WASM), pauses ; operations set, toggle et add (borne ou bouclee).
- `HandsBridge.exe` : pont SimConnect resident, independant de StreamSim, lance par la tray (tube nomme).
- Bibliotheque de 48 commandes du Rafale (AzurPoly) reprises de StreamSim.
- Application de barre des taches : interrupteur, reglages, boutons de memorisation des zones, fenetre « Configurer les zones et leurs commandes ».
- Menu en VR (pincer les deux mains) : reglages, pages Zones et Commandes, selection au doigt.
- Installateur Windows (Setup) et script de release `scripts\build-release.ps1`.

### Limites connues
- Gestes (rotatif, curseur), contact au doigt du menu VR et pont SimConnect : non valides en vol au moment de la publication.
- Les zones sont memorisees dans l'espace du jeu : a reapprendre si le cockpit est recale.
- Le pont SimConnect demande le SDK MSFS pour etre compile ; la release se construit donc en local.
