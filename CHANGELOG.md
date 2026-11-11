# Journal des versions

Format : [Semantic Versioning](https://semver.org/lang/fr/). La version courante est dans le fichier `VERSION`.

## [0.0.28] - 2026-11-11

### Modifie
- **Mode souris « colle au doigt » : asservissement VISUEL.** Les versions precedentes calculaient ou placer le pointeur d'apres un modele de la souris de MSFS ; les mesures
  (captures natives de la couche : pointeur a 140 a 320 px du doigt) ont montre que ce modele ne tient pas. La couche **regarde maintenant ou MSFS dessine son pointeur** : elle lit un
  morceau de l'image que le jeu rend pour l'oeil gauche (celui d'ou MSFS tire son rayon), y detecte la fleche blanche, la compare au bout de l'index vu du meme oeil et envoie de petits
  deplacements relatifs de souris jusqu'a ce que la fleche soit sous le doigt. Main droite ou gauche : celle dont l'index est tendu ou qui pince en premier. Pincer = clic gauche (apres 90 ms,
  pour ne pas cliquer pendant le geste d'ouverture du menu), garder le pincement = glisser.
  - **Au demarrage** : recherche de la fleche par dalles, puis quatre petites salves de comptes connus qui **mesurent le gain** (pixels par compte) et la **latence** de la souris de MSFS ;
    le pointeur « tremble » donc une seconde ; main immobile. Remesure possible dans le menu.
  - **Boucle sans oscillation** : une boucle simple a gain 1 diverge des que la mesure a 2 images de retard (c'etait l'orbite observee) ; la boucle est maintenant un observateur
    (modele des comptes envoyes + correction lente par la mesure), valide en simulation de bout en bout (retard 1-3 images, gain reel de 0.4 a 3 fois l'a priori).
  - **Curseur masque** : MSFS masque le curseur apres quelques secondes sans mouvement et ignore les clics tant qu'il est masque ; la couche envoie un compte de garde toutes les 0.7 s.
  - **Injection** dans un fil dedie, par evenements de 8 comptes au plus (jamais de SendInput absolu : MSFS lit des deplacements bruts).
  - Menu VR > Souris : activer, type (colle au doigt / souris de bureau), etat, oeil de reference, remesure. Le doigt hors du champ de l'oeil est borne au bord de l'image (pas de fuite du
    curseur de Windows vers l'autre ecran).
  - **Diagnostic** : une ligne par seconde (`servo : suivi, ... ecart estime, pointeur (x,y), doigt (x,y), L=, gain ...`) et des recadrages annotes `servo_NN_*.bmp` dans `logs\captures`
    (croix rouge = pointeur detecte, jaune = bout du doigt, verte = position attendue).
- Le calibrage par la souris reelle, le reglage a vue et le cube vert sont retires (inutiles) ; la « souris de bureau » (deplacements relatifs de la main) reste disponible.

### Ajoute
- Tests : detecteur de pointeur sur de vrais recadrages natifs, boucle de controle, mesure du gain, simulation de bout en bout (117 tests au total).

## [0.0.27] - 2026-11-11

### Modifie
- **Souris : deux types, « pointeur colle au bout du doigt » par defaut.** Le pointeur de MSFS est amene sur la ligne tete -> bout de l'index, c'est-a-dire la ou l'on voit le doigt (il est dessine par MSFS sur la surface visee, il ne peut pas flotter en l'air). Utilise l'etalonnage de la souris (menu Souris > Etalonner). Pendant le pincement, la position de depart du bout du doigt est gardee et seul le deplacement de la base de l'index est suivi : pas de derapage au clic, glisser possible. L'autre type, « souris de bureau » (deplacements relatifs de la main, 0.0.26), reste disponible dans le menu.

## [0.0.26] - 2026-11-11

### Modifie
- **Mode souris : le doigt EST la souris** (plus de visee depuis la tete, plus d'etalonnage). Le deplacement de la main (gauche / droite, haut / bas, dans le plan de la vue) devient un deplacement relatif de la souris de Windows, que MSFS traite comme celui d'une vraie souris. Pincer = bouton gauche ; garder le pincement = glisser. Index replie = souris soulevee (la main revient sans deplacer le pointeur). La main est suivie par la base de l'index, qui bouge peu quand on pince. Menu Souris : activer, plus rapide / plus lente (3500 px par metre au depart, `gain` dans mouse.ini).

## [0.0.25] - 2026-11-11

### Corrige
- **Mode souris : deplacements RELATIFS de la souris.** MSFS lit la souris en entree brute : un deplacement absolu de `SendInput` lui arrivait comme un deplacement de milliers d'unites, d'ou le pointeur qui dansait loin du doigt (video du 11 nov., 2:05 : doigt a droite, pointeur a gauche alors que le cube calcule etait au doigt). La souris recoit maintenant des deplacements relatifs, corriges d'apres la position reelle du curseur Windows.

## [0.0.24] - 2026-11-11

### Modifie
- **Souris : cube vert a la profondeur de la surface visee** (60 cm par defaut) au lieu de 6 m : le pointeur de MSFS est dessine sur la surface, un cube a 6 m ne se superpose pas avec lui (parallaxe). Lignes « Cube plus pres / plus loin de moi » dans le menu pour l'ajuster jusqu'a ce que les deux se confondent. (L'etalonnage du 11 nov. 11:43 est bon : ecarts de 1 a 7 px, centre = centre de la fenetre MSFS.)
- **Mode souris plus stable** : lissage plus fort et zone morte de 3 px, pour que le pointeur ne saute plus d'un element aimante a l'autre.

## [0.0.23] - 2026-11-11

### Corrige
- **Souris : le pointeur de MSFS est fixe dans le monde, pas dans la tete.** Il ne bouge que si la souris bouge ; tourner la tete ne le deplace pas. Le calcul le supposait colle a la vue, d'ou un etalonnage incoherent (la position du pointeur pour une meme direction vue variait de plus de 400 px), un cube vert qui suivait la tete et un pointeur qui « dansait » en mode souris. Pixel <-> direction se calcule maintenant en cap / site dans l'espace du jeu, depuis la position de la tete. Le cube vert ne suit donc que la souris.
- **L'ancien etalonnage est ignore : il faut le refaire une fois** (menu Souris > Etalonner).

## [0.0.22] - 2026-11-11

### Ajoute
- **Etalonnage de la souris : le repere passe au vert quand la souris est dessus** (a 45 px pres selon l'etalonnage precedent), pour savoir quand cliquer. Sans etalonnage existant, le repere reste blanc.

## [0.0.21] - 2026-11-11

### Corrige
- **Souris : reperes d'etalonnage et cube vert a 6 m** (au lieu de 60 cm). MSFS dessine le pointeur de la souris de la meme facon dans chaque oeil (comme a l'infini) : on le voit double quand on regarde un objet proche, et un repere a 60 cm est vu a ~3 degres de plus par l'oeil dominant, soit environ 60 px d'erreur d'etalonnage. A 6 m l'ecart tombe a ~6 px et les deux se superposent sans relief.

## [0.0.20] - 2026-11-11

### Modifie
- **Souris : tout se fait dans le menu VR, plus aucune touche du clavier** (le casque cache le clavier). Page « Souris » : activer le mode souris, lancer l'etalonnage (5 clics de souris sur les reperes), et **reglage a vue** : un cube vert montre ou le calcul place le pointeur ; on le recale au doigt avec les lignes « Cube plus a gauche / droite / haut / bas / ecarter / resserrer », puis « terminer et enregistrer ». Les anciens raccourcis clavier restent actifs mais ne sont plus necessaires.

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
