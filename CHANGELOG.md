# Journal des versions

Format : [Semantic Versioning](https://semver.org/lang/fr/). La version courante est dans le fichier `VERSION`.

## [Non publie]

### Ajoute
- Zones en **boites orientees en fil de fer**, comme Archan XR : chaque zone a sa taille (L x H x P, mm) et son orientation (lacet, tangage, roulis), par defaut
  selon le type de commande (petit pour un bouton, grand pour une molette ou un ecran). Seules les aretes sont dessinees : les commandes restent visibles.
  Couleur selon le type (bleu poussoir/bascule, jaune interrupteur, orange rotatif, vert curseur) ; arete epaissie et claire quand le doigt est dedans,
  cyan pendant une saisie, vert vif au declenchement, blanc a la memorisation.
- Le contact se teste dans la boite (agrandie de la taille du bout du doigt, `[zones] finger_mm`) et non plus dans une sphere ; un curseur a une boite allongee le long de sa course.
- Affichage : `[zones] show_boxes = all | near | hidden` (reglable dans le menu en VR), `near_cm`.
- Menu en VR : page de detail d'une zone (boite plus grande / plus petite, forme suivante, reapprendre, supprimer). Fenetre de configuration : taille et rotation.
- **Faces seules** (`[zones] box_faces = face`, menu VR) : chaque boite est dessinee par sa face avant seulement, un carre ou un rectangle (option « FACE ONLY » d'Archan XR).
- **Rotatif au glisse** (`gesture = slide`) : pincer puis glisser le long de la boite, un cran tous les `step_deg` mm, en plus du geste « tourner le poignet » (`twist`).
- **Molettes a deux niveaux** (`levels = 2`, `inner_ratio`) : bague exterieure et bouton central, deux boites imbriquees ; le centre est prioritaire ; emplacements `inc2` / `dec2` / `press2` (commandes COM, NAV, RANGE du Garmin).
- `zones.ini` : `size_mm = L, H, P` et `rot_deg = lacet, tangage, roulis` ; l'ancien `radius_mm` devient un cube equivalent.

## [0.0.2] - 2026-11-09

### Corrige
- Le bouton Installer / Mettre a jour de la tray affichait « Script introuvable : install.ps1 » apres une installation par le Setup : l'installateur
  copie maintenant la DLL « source » et `scripts\install.ps1` / `uninstall.ps1` a cote de la tray.

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
