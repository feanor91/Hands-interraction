# Feuille de route

Idées retenues mais pas encore faites, pour ne rien perdre. Dernière mise à jour : 10 novembre 2026. Ce qui est fait est dans [CHANGELOG.md](CHANGELOG.md).

## Observé dans Archan XR (vidéo de test du 10 novembre)

Ce qu'Archan XR fait et que nous avons déjà repris : boîtes d'interaction en fil de fer de tailles et d'orientations différentes, option « face seule », molettes à deux niveaux (bague + centre),
curseurs / leviers par glissé du doigt, « move cockpit » (recentrage), gants, menu de réglages dans le casque.

## À faire

### Couleurs des boîtes
Aujourd'hui : bleu poussoir / bascule, jaune interrupteur, orange rotatif, vert curseur (couleur par **type de commande**).
Archan XR : bleu = boutons, jaune = molettes / leviers / interrupteurs, rouge = grands volumes et écrans, violet = cas particuliers (une couleur par **famille d'usage**).
→ À décider plus tard : s'aligner sur Archan, ou laisser l'utilisateur choisir la couleur par zone / par type (`color = ...` dans `zones.ini`, réglage dans la fenêtre de configuration).

### Écrans tactiles
* **Pincer un écran pour le faire apparaître en grand devant le pilote** (Archan XR) : on ne sait pas encore comment c'est fait. Pistes : (1) un quad OpenXR de plus, alimenté par une copie de la région de l'écran
  dans l'image du jeu (les couches du jeu sont accessibles dans `xrEndFrame`) ; (2) relais de l'image d'un écran du simulateur (fenêtre déportée, caméra de cockpit) ; (3) redirection de la souris sur l'écran agrandi.
  À étudier : comment la zone de l'écran dans l'image du jeu est localisée depuis l'espace du cockpit (projection d'une boîte rouge dans l'image de chaque œil).
* **Rafale : interactions sur les écrans tactiles à prendre en compte.** AzurPoly ne publie **aucun** événement tactile (ni coordonnées, ni défilement, ni zoom) : la page des
  commandes H: / L: / K: ne propose que la sélection de pages (`AZP_RAF_VTLD_SWITCH_MOVED_*`, `AZP_RAF_VTLG_SWITCH_MOVED_*`). Pour toucher un écran il faudra donc passer par une entrée
  que le jeu reconnaît : clic de souris au bon pixel (projection de la boîte de l'écran dans la fenêtre du jeu), ou Input Events de l'avion s'il y en a pour les touches de l'écran
  (`simbridge_cli inputs` sur le Rafale : à examiner écran par écran).

### Autres options vues dans le menu d'Archan XR (non reprises pour l'instant)
* **Mode édition** : déplacer et redimensionner une boîte directement avec la main, dans le casque.
* **Finger pull to box** : le bout du doigt est aimanté vers la boîte.
* **Lissage du bout du doigt** (« smoothing »), taille du cube du bout du doigt (déjà : `finger_mm`), « lock », « toggle press », vitesse des rotatifs (« rotary speed »).
* **Réglage fin d'une boîte au doigt** (« trim ») et « unhide all ».

### Pont SimConnect, étape 4 (déjà prévue)
Apprendre une commande en l'actionnant dans le cockpit : repérer l'Input Event ou la variable L: qui change et pré-remplir la zone.

### Distribution
* Intégration continue de la release complète : le pont SimConnect demande le SDK de MSFS, non redistribuable ; la release se construit pour l'instant en local (`scripts\build-release.ps1`).
* Signature du Setup.

## Rafale : commandes ajoutées par la mise à jour d'AzurPoly (novembre 2026)
Prises en compte dans la bibliothèque de commandes : pilote automatique ALT / ALT SEL / FPL / HOM / ILS, molette multifonction (bague extérieure `MF_KNOB_MODE_INC/DEC` + centre `MF_KNOB_INC/DEC/PUSHED`,
donc un rotatif à deux niveaux), cran +/− des commandes moteur AEC. Source : <https://docs.azurpoly.com/rafale/controls-assignments/#hevents>.
Reste à examiner : les événements K: de la table des bindings (trim, direction, fumée, plasma…) qui passent par SimConnect.
