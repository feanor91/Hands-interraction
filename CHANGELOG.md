# Journal des versions

Format : [Semantic Versioning](https://semver.org/lang/fr/). La version courante est dans le fichier `VERSION`.

## [0.0.5] - 2026-11-10

### Corrige
- **Pincement en mode « mains seules »** : le geste d'ouverture du menu (pincer les deux mains) et la saisie des rotatifs ne fonctionnaient pas, car le pincement n'etait calcule que par
  l'emulation de manette (desactivee dans ce mode). Il est maintenant lu directement sur les articulations (pouce contre index ou majeur, seuils `click_on_mm` / `click_off_mm`).

## [0.0.4] - 2026-11-10

### Corrige
- **Menu en VR : interaction aux mains refaite comme celle des zones du cockpit** (plus rien ne depend d'une manette emulee). Chaque ligne est une dalle 3D devant le panneau
  (de 9 cm derriere a 7 cm devant) ; le doigt qui y reste 0,3 s choisit la ligne, puis doit en sortir pour recommencer (meme moteur que les zones). Le pointeur est **le bout de doigt le plus
  en avant** (pouce, index, majeur, annulaire, auriculaire) : une main a plat ou pointee fonctionne, le bout de l'index n'est plus indispensable.
- « Mains affichees : NON » ne cache plus que les mains : les boites des zones et le curseur du menu restent dessines.
- Menu en VR au doigt : le panneau n'affiche plus que les lignes utiles (il etait trois fois trop haut, avec des lignes vides), plus petit et plus pres ; un **curseur** (carre cyan qui se resserre, vert quand on peut choisir)
  montre le point du panneau vise par le doigt ; survol a moins de 15 cm, choix a moins de 5 cm pendant 0,3 s (au lieu de 3 cm / 0,45 s) ; le bout du majeur compte aussi.
- Le journal `layer_<programme>.log` note la position du doigt par rapport au panneau (une ligne par seconde, 120 lignes) pour diagnostiquer les decalages.

## [0.0.3] - 2026-11-10

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
- **Mains pleines** (`[hands] style = glove | bare | ghost`, menu VR › Affichage) : gants (volume opaque sombre), mains nues (couleur peau), mains translucides (`ghost_alpha`) ;
  le squelette reste le defaut. Volumes generes par le shader (spheres aux articulations, troncs de cone pour les os, paume comblee) ; les translucides sont rendues en deux passes
  (profondeur puis melange) pour qu'une seule couche soit visible.
- **Le cockpit suit le recentrage de MSFS** : touche `[zones] recenter_key` (LSHIFT par defaut, la meme que MSFS ; choix dans la tray). Les positions des zones sont relatives a un repere
  pose a la tete au moment du recentrage.
- **Rotatif au glisse et curseur sans pincement** : un simple pointage dans la boite (200 ms) suffit.
- Nouvelles commandes AzurPoly dans la bibliotheque : pilote automatique ALT / ALT SEL / FPL / HOM / ILS, molette multifonction a deux niveaux (bague + centre), cran des commandes moteur AEC.
- **Indicateur de position des molettes** (comme Archan XR) : tant qu'on actionne un rotatif, un petit slider horizontal gradue apparait au-dessus avec une poignee qui montre la position en pourcentage (et 1,8 s apres). La position est lue dans le simulateur quand l'action « cran + » est un ajout borne sur une variable L: ou un Input Event (eclairages, luminosites, commandes moteur) ou par `read` / `min` / `max` dans `zones.ini` ; sinon elle est estimee a partir des crans envoyes (`notches` crans sur la course). Un curseur affiche aussi sa poignee le long de la course.
- **Touche ou bouton pour afficher / cacher le menu en VR** : `[menu] toggle_key`, `toggle_button`, `toggle_joystick`, choisis dans la tray (*Menu en VR (touche / bouton)*). Le pincement des deux mains reste possible.
- Menu en VR reorganise en sous-pages (Zones, Affichage, Rayon et clic) : toutes les pages tiennent dans le panneau.
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
