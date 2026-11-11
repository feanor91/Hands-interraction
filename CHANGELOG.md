# Journal des versions

Format : [Semantic Versioning](https://semver.org/lang/fr/). La version courante est dans le fichier `VERSION`.

## [0.0.19] - 2026-11-11

### Ajoute
- **Reglage a vue de la souris (Ctrl+Alt+F12)** : un cube vert est dessine la ou le calcul place le pointeur de la souris. Bougez la vraie souris, comparez le cube au pointeur de MSFS, et corrigez avec Ctrl+Alt + fleches (decaler) et Ctrl+Alt + Page haut / bas (ecarter / resserrer). Ctrl+Alt+F12 de nouveau enregistre dans `mouse.ini`. Remplace la visee d'un carre avec un pointeur minuscule.

## [0.0.18] - 2026-11-11

### Modifie
- **Etalonnage de la souris plus robuste** : une seule echelle pour x et y (pixels carres) au lieu de deux echelles libres ; un repere mal vise ne fausse plus l'ensemble. Le journal donne maintenant, pour chaque repere, l'angle attendu, la position du pointeur et l'ecart apres calcul.

## [0.0.17] - 2026-11-10

### Modifie
- **Etalonnage de la souris, plus simple** : chaque repere blanc est valide par un **clic gauche** de la souris (Ctrl+Alt+F10 marche toujours). Le repere est maintenant **fixe dans le monde** (il ne suit plus le regard : on peut bouger la tete pour viser) et place a 60 cm, la distance d'un tableau de bord, pour que le pointeur de MSFS et le repere soient en contact (a 1 m ils flottaient l'un devant l'autre en relief).

## [0.0.16] - 2026-11-10

### Ajoute
- **Menu VR : mode souris.** Page principale : « Mode souris : activer / desactiver » et « Etalonner la souris » (lance l'etalonnage ; les 5 reperes se prennent ensuite avec Ctrl+Alt+F10 et la vraie souris).

## [0.0.15] - 2026-11-10

### Ajoute (essai)
- **Mode souris** : les mains pilotent la souris de Windows, donc le pointeur de MSFS, qui sait tout manipuler dans le cockpit. Le pointeur suit le bout de l'index (projete dans la vue ecran de MSFS) ; pincer = bouton gauche enfonce, relacher = relache (glisser possible : verriere, curseurs). Aucune zone apprise necessaire.
  - **Ctrl+Alt+F10** : etalonnage (une fois). 5 reperes blancs apparaissent, fixes devant les yeux ; avec la vraie souris, posez le pointeur de MSFS sur le repere puis Ctrl+Alt+F10 ; apres le 5e, le resultat va dans `mouse.ini` (a cote de `zones.ini`).
  - **Ctrl+Alt+F11** : active / desactive le mode souris.
  Mode « mains seules » conseille (la fenetre MSFS doit avoir le focus).

## [0.0.14] - 2026-11-10

### Corrige
- **Clic virtuel : le rayon part de l'oeil** (comme une souris) et traverse le centre de la zone. Avec le sens du doigt, le rayon rasait la surface et touchait un point plus loin que le bouton (frein de parc manque). Le doigt n'est plus utilise que si la tete n'est pas suivie.

## [0.0.13] - 2026-11-10

### Corrige
- **Clic virtuel : le rayon suit maintenant votre doigt.** Il partait dans le sens appris de la boite (vers le bas pour le frein de parc) ; il part desormais de 10 cm derriere le bout de l'index, dans la direction reelle du doigt au moment du contact.

## [0.0.12] - 2026-11-10

### Ajoute (essai)
- **Clic virtuel, etape 1** : une zone (poussoir ou bascule) peut cliquer dans le simulateur au lieu d'envoyer une commande. Une manette fantome apparait le temps du clic, avec un rayon qui part
  de 10 cm devant la zone, dans le sens du doigt de l'apprentissage, et traverse son centre ; la gachette est pressee 180 ms. C'est le simulateur lui-meme qui traite le clic, comme avec une vraie
  manette (aucune commande a connaitre). Active par `click = true` dans la zone de `zones.ini`, ou dans le menu en VR (page d'une zone : « Action : commande / clic virtuel »).
  Mode « mains seules » uniquement. Vise le glissement de la verriere et les ecrans tactiles aux etapes suivantes.

## [0.0.11] - 2026-11-10

### Modifie
- **Boutons HOTAS retires de la liste des commandes** : `AZP_RAF_CONTROLS_MG*`, `MGP*`, `PM*` (manche et manette des gaz) ne sont plus proposes, meme en « avances ».

### Ajoute
- **Releve par etapes** (`simbridge_cli record [secondes] [fichier]`) : mesure 8 s de bruit de fond, puis note, pour chaque bouton actionne, les Input Events, variables L: et SimVars qui changent. CTRL DROIT du clavier ouvre une nouvelle etape (bip). Sert a cataloguer toutes les commandes du cockpit.

## [0.0.10] - 2026-11-10

### Modifie
- **Liste des commandes triee** : de 19 pages a 8. Les Input Events deja couverts par la bibliotheque ne sont plus proposes en double, et les « avances » (`UNKNOWN_*` : verins, trappes, cales, capots du sol ; `AZP_RAF_CONTROLS_*` : entrees des manettes) sont masques par defaut. Une ligne en bas de la page « Commandes » les affiche ou les masque (« Afficher les commandes avancees »).
- **Outils de decouverte** (`simbridge_cli`) : `dump`, `watch` (Input Events), `lvars` / `watchl` (variables L: du Rafale par le module MobiFlight, 1000 noms au plus), `watchs` (SimVars standard) ; filtres avec exclusion (`AZP_RAF,!SHAKE`). Le relevé affiche ce qui change quand on manipule une commande dans le cockpit. Pause entre deux passages pour ne pas surcharger le simulateur.

### Ajoute
- **Verriere : couvercle de protection** (`L:AZP_RAF_CANOPY_SWITCH_HOOD`, 1 = ouvert), releve avec le debogueur de MSFS. Le bouton de la verriere (a maintenir, trois positions) n'a pas de commande externe connue pour l'instant ; son etat se lit par la SimVar `CANOPY OPEN`.

### Corrige
- Lectures de variables en boucle (`ReadVar`) : une definition SimConnect par variable, reutilisee, au lieu d'une par lecture (saturait le simulateur lors des releves).

## [0.0.9] - 2026-11-10

### Modifie
- **Boites en « faces seulement »** : le carre dessine est maintenant la face arriere (pour une boite a plat, le dessous), et non plus la face avant / le dessus, pour ne pas passer entre le doigt et la commande.

## [0.0.8] - 2026-11-10

### Corrige
- **Apprentissage d'une zone declenche tout seul** : en mode « mains seules », le pincement lu sur les articulations comptait le majeur replie contre le pouce (main qui pointe) comme un pincement.
  Le majeur n'est plus pris en compte que s'il est tendu ; seul le pincement pouce-index compte quand on pointe.
- **Mains perdues apres avoir repris les manettes** : si une main suivie ne l'est plus depuis 6 s (manettes reposees), son hand tracker est detruit et recree (toutes les 6 s au plus) pour retrouver le suivi.

### Ajoute
- **Nouvelle icone** : pastille sombre, cube filaire et point d'etat (vert actif, gris desactive, orange couche absente), pour la tray, le Setup et les raccourcis (`installer/hands.ico`, `scripts/gen_icon.py`).

## [0.0.7] - 2026-11-10

### Modifie
- **Menu en mode « mains seules » plus bas** : le panneau s'ouvre 32 cm sous les yeux et s'incline vers la tete (on le regarde de dessus, le bras reste detendu).
- **Menu rouvert apres l'apprentissage** : quand une zone est apprise depuis le menu (ajout ou reapprentissage), le menu se rouvre sur la page de cette zone.

### Corrige
- **Selection immediate dans le menu** : a chaque changement de page, le doigt doit d'abord sortir de la ligne qu'il touche avant d'en choisir une (le choix d'une commande n'est plus declenche aussitot).

## [0.0.6] - 2026-11-10

### Corrige
- **Les mains ne sont plus cachees par le menu** : la surimpression des mains (et le curseur, les boites) est maintenant dessinee APRES le panneau du menu.
- **Memorisation d'une zone par pincement** (`[zones] teach_mode = pinch`, defaut ; `timer` = ancien compte a rebours) : on pointe la commande du bout de l'index puis on pince (60 s pour le faire,
  sons discrets toutes les 2 s) ; la position retenue est celle du doigt 300 ms AVANT le pincement, qui le deplace. Marche depuis le menu en VR comme depuis le bouton de manette / HOTAS et
  enchaine debut puis fin de course pour un curseur.
- **Orientation automatique des boites** : a la memorisation, la boite prend la direction du doigt (commande a plat enfoncee par le haut -> boite a plat face vers le haut ; commande verticale ->
  face vers le pilote, tournee par pas de 15 degres). Menu en VR, page d'une zone : bascule « verticale / a plat » (banquettes et consoles du Rafale).

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
