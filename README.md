# Hands Interaction — mains nues pour MSFS 2024

Couche OpenXR implicite (DLL 64 bits) entre **MSFS 2024 (Store, D3D12)** et **Virtual Desktop (VDXR)** avec un **Meta Quest 3**, pour agir sur le cockpit avec les mains,
sans manettes. Version courante : voir [VERSION](VERSION) · [journal des versions](CHANGELOG.md).

## Principe

Le **rayon aux mains** (viser une commande comme avec une manette) a été essayé à fond : le suivi des mains tremble de quelques millimètres et le geste de clic
bouge la main, donc viser une petite cible (interrupteur, tablette) est moins efficace qu'avec une manette ou une souris. Le projet a donc pivoté vers un
**pochoir** : on **apprend la position de chaque commande** au bout de l'index, puis un contact du doigt (ou un pincement et un geste) envoie la commande au simulateur.
Pas de visée, donc pas de tremblement qui compte : on touche l'endroit où se trouve la commande.

```
 Quest 3 ──► Virtual Desktop (VDXR) ──► [couche OpenXR : suivi, zones, menu VR] ──► MSFS 2024
                                                │ tube nommé
                                                ▼
                                      HandsBridge.exe ──SimConnect──► MSFS (événements, variables, Input Events, code H:)
```

## Installation (utilisateur)

1. Télécharger `HandsInteraction-Setup-<version>.exe` dans les [releases](https://github.com/feanor91/Hands-interraction/releases) et le lancer (droits administrateur).
   **MSFS doit être fermé** (la couche est chargée par le jeu).
2. L'installateur copie la couche dans `C:\ProgramData\HandsLayer\`, l'enregistre auprès d'OpenXR, installe la tray et le pont SimConnect dans
   `C:\Program Files\Hands Interaction\`, et propose de lancer la tray au démarrage de Windows.
3. Dans **Virtual Desktop**, activer le suivi des mains transmis aux applications (« forward tracking »).
4. Lancer MSFS : l'icône de la tray passe au vert. Désinstallation : *Paramètres Windows › Applications*. Vos réglages (`hands.ini`), vos zones et les journaux sont conservés.

Seul le suivi des mains de VDXR est testé (Quest 3, MSFS 2024 Store). La couche ne s'active que pour `FlightSimulator2024.exe` (`[general] process_allowlist`).

## Utilisation

### Mode « mains seules »
VDXR fabrique de lui-même des manettes virtuelles à partir de vos mains (rayon, clics, manettes fantomes sur le bureau). Réglage par défaut : la couche les **masque**
tant que les mains sont suivies (`[hands] hide_controllers`), et le jeu ne voit plus que des mains. Le rayon aux mains (`[emulation] enabled = true`) reste disponible, mais peu précis.

### Aspect des mains
`[hands] style` (ou menu VR › *Affichage*) : **squelette** (cubes, défaut), **gants** (volume opaque sombre), **mains nues** (opaque, couleur peau) ou **mains translucides** (`ghost_alpha`) pour voir les commandes à travers.

### Recentrage du cockpit
Quand vous recentrez la vue dans MSFS, le cockpit se replace autour de votre tête : les zones doivent suivre. Réglez **la même touche** que celle de MSFS dans `[zones] recenter_key`
(`LSHIFT` par défaut, ou tray › *Zones* › *Touche de recentrage…*). Procédure : s'asseoir en position neutre, recentrer la vue dans MSFS, puis apprendre ou utiliser les zones.
Les positions sont enregistrées relativement au repère posé à la tête lors du dernier recentrage.

### Créer une zone
Deux façons, qui écrivent le même fichier (`C:\ProgramData\HandsLayer\data\zones.ini`, rechargé à chaud) :

* **Dans le casque** : pincer les deux mains (ou la touche / le bouton de manette ou HOTAS choisi dans la tray › *Menu en VR*) pour ouvrir ou fermer le menu › *Zones du pochoir et leurs commandes* › *Ajouter une zone* › choisir la commande dans la liste
  (les 48 commandes du Rafale, puis, avec *Lire aussi les commandes de l'avion*, tous les Input Events de l'avion chargé). Le menu se ferme : **pointez la commande du bout de l'index, puis pincez**
  (60 s pour le faire) ; la zone est enregistrée à la position du doigt juste avant le pincement (son aigu + flash blanc), avec une orientation déduite de la direction du doigt (à plat ou verticale). Le menu se choisit au doigt (toucher la ligne 0,45 s) ou au rayon.
* **Sur le PC** : icône de la tray › *Zones* › *Configurer les zones et leurs commandes…* (liste, type, étapes, bouton *Tester* qui envoie la commande au simulateur sans passer par le casque).
  Les positions s'apprennent ensuite en vol avec un **bouton du joystick / HOTAS** (choisi dans la tray) : l'index sur la commande, appui sur le bouton.
  Repli clavier : Ctrl+Alt+F9 (apprendre), Ctrl+Alt+F10 (effacer les positions).

Chaque zone est une **boîte orientée en fil de fer** (comme dans Archan XR) : sa taille dépend de la commande (petite pour un bouton, grande pour une molette ou un écran) et seules ses arêtes sont
dessinées, pour ne pas cacher la commande. Couleurs : bleu = poussoir / bascule, jaune = interrupteur, orange = rotatif, vert = curseur ; arête épaissie et claire = doigt dedans, cyan = saisie en cours,
vert vif = déclenchée, blanc = mémorisée. Le contact se teste dans la boîte, agrandie de la taille du bout du doigt (`[zones] finger_mm`).
Dimensions et rotation se règlent dans la fenêtre de configuration (L/H/P en mm, lacet / tangage / roulis) ou dans le menu VR (*zone › boîte plus grande / plus petite / forme suivante*) ;
l'affichage se change dans le menu VR : `[zones] show_boxes` (toutes, près des doigts, cachées) et `box_faces` (boîte complète ou face avant seule, un carré).
Les positions sont dans l'espace du jeu : **si le cockpit est recalé, réapprendre les zones.**

### Types de commande
Repris de StreamSim (voir [core/sim/ZoneControl.h](core/sim/ZoneControl.h)) :

| Type | Geste | Envoie |
|---|---|---|
| Poussoir | toucher la zone (index maintenu `dwell_ms`) | la séquence « appui » |
| Bascule | idem | l'action en mode *toggle* : l'état réel est lu dans le simulateur |
| Interrupteur à N positions | chaque contact passe à la position suivante (en boucle ou aller-retour) | la séquence de la position ; la position réelle peut être lue (`read`) |
| Rotatif | **pincer** dans la zone puis **tourner le poignet** (`gesture = twist`, un cran tous les `step_deg` °) ou, sans pincer, **pointer et glisser** le long de la boîte (`slide`, un cran tous les `step_deg` mm) ; pincer-relâcher sans bouger = appui. Un **indicateur de position** (petit slider gradué avec une poignée) apparaît au-dessus de la molette pendant qu'on l'actionne. Option **deux niveaux** (`levels = 2`) pour les molettes concentriques : bague extérieure + bouton central (boîtes imbriquées, le centre est prioritaire) | cran + / cran − / appui, et `inc2` / `dec2` / `press2` pour le centre |
| Curseur | deux points appris (début et fin de course) ; **pointer et glisser** le doigt dans la boîte (sans pincer) | une valeur min..max, ou des crans + / − |

Chaque emplacement est une **séquence d'actions** : événement SimConnect (`PARKING_BRAKES`), variable (`L:...` ou SimVar), Input Event de MSFS 2024, code avionique
(`(>H:...)`, exécuté par le module MobiFlight WASM fourni avec l'avion) ou pause. Opérations sur variables et Input Events : `set`, `toggle` (bascule entre 0 et la valeur d'après l'état réel),
`add` (pas, avec min / max ou bouclage, par ex. un cap 0–360).

### Le pont SimConnect
`HandsBridge.exe` (lancé et surveillé par la tray) se connecte à MSFS (2020 ou 2024) et exécute les commandes ; il n'a besoin ni de StreamSim ni d'aucune configuration. Journal : `logs\bridge.log`.
Outil d'essai en ligne de commande : `simbridge_cli.exe status | event | get-var | set-var | inputs | get-input | set-input | code`.

## Configuration

`C:\ProgramData\HandsLayer\hands.ini`, relu à chaud (sauf `[general]` et `[emulation] enabled`, lus au lancement du jeu). Les réglages courants se font depuis la tray. Sections :
`[general]` (activation, programmes), `[hands]` (squelette, masquage des manettes virtuelles), `[emulation]` (rayon aux mains), `[touch]`, `[menu]` (menu VR), `[zones]` (rayon, délais, boutons du HOTAS).
Les journaux de la couche sont dans `C:\ProgramData\HandsLayer\logs\layer_<programme>.log`.

La suite prévue est dans [ROADMAP.md](ROADMAP.md).

## Compiler et publier (développeur)

Prérequis : Visual Studio 2022 (C++), CMake ≥ 3.20, et, pour `HandsBridge.exe`, le **SDK SimConnect de MSFS** (`C:\MSFS 2024 SDK\SimConnect SDK`, option `-DHANDS_SIMCONNECT_SDK=`).

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Release complète (compilation, tests, installateur [Inno Setup 6](https://jrsoftware.org/isdl.php)) :

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-release.ps1              # produit dist\HandsInteraction-Setup-<version>.exe
powershell -ExecutionPolicy Bypass -File .\scripts\build-release.ps1 -Version 0.1.0 -Publish   # change la version, tag vX.Y.Z et release GitHub (gh)
```

La version est dans le fichier [VERSION](VERSION) (X.Y.Z, première publication : 0.0.1) ; elle est compilée dans la couche, la tray et le pont, et reprise par l'installateur.
L'intégration continue GitHub ([.github/workflows/ci.yml](.github/workflows/ci.yml)) compile la couche et la tray et lance les tests ; elle ne peut pas construire le pont (SDK non redistribuable),
c'est pourquoi **le Setup complet se construit en local**. Installation sans Setup, pour le développement : `scripts\install.ps1` (administrateur), `scripts\uninstall.ps1`.

## Arborescence

```
core/       cœur portable C++17 : config INI, journal, calculs (pose, zones, gestes), modèle des zones et des actions — testé
layer/      couche OpenXR : entry, hooks, masquage des manettes, émulation du rayon, surimpression, menu VR, zones, client du pont
tray/       HandsTray : icône de barre des tâches, réglages, fenêtre de configuration des zones
tools/      HandsBridge (pont SimConnect résident), simbridge_cli (essai en ligne de commande)
win/        code Windows partagé : SimBridge (SimConnect), JoyPoll (DirectInput)
installer/  HandsSetup.iss (Inno Setup)
scripts/    build-release.ps1, install.ps1, uninstall.ps1, gen_command_library.py (bibliothèque Rafale), check-process.ps1
config/     hands.ini d'exemple · manifest/ manifeste de la couche · tests/ tests unitaires · third_party/ en-têtes OpenXR (THIRD_PARTY.md)
```

## Bilan de la voie « rayon aux mains » (conservé)

Ce qui marche : suivi des mains exposé à MSFS, clic fiable, squelette des mains, menu VR. Ce qui ne marche pas assez : la **précision du rayon**, surtout sur les tablettes du cockpit.
Essayé sans le résoudre : rayon par l'épaule estimée (torse lent, mélange avec la direction du doigt), lissage One Euro, pré-verrouillage de la pose avant le clic, zone morte.
Voies fermées : profondeur (MSFS n'acquiert jamais ses swapchains de profondeur), SimConnect/WASM pour un « clic sous le rayon » générique (seulement des variables par avion).
Piste non explorée : accroche du rayon sur la flèche bleue d'élément interactif que MSFS dessine.

## Choix de sécurité

* **Fail-open** : toute erreur laisse passer sans rien modifier ; chaque hook appelle toujours la vraie fonction, en dehors de tout bloc `try`.
* Hors processus autorisé : aucun journal, aucun thread, aucun hook.
* La DLL n'est modifiable que par les administrateurs ; seuls `logs\`, `data\` et `hands.ini` sont inscriptibles par le jeu.
* Le tube nommé du pont n'accepte que des clients locaux.
* Licence du projet : à choisir.
