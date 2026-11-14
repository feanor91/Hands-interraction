// Config.h — réglages de la couche, rechargés à chaud.
//
// Principe : un objet `Config` est IMMUABLE une fois construit. Quand le
// fichier change, le thread de surveillance construit un NOUVEL objet puis
// remplace le pointeur partagé de façon atomique. Le code de rendu lit donc
// toujours un ensemble de valeurs cohérent, sans verrou (équivalent d'un
// remplacement de référence `volatile` sur un objet immuable en C#).
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../log/LogLevel.h"
#include "IniFile.h"

namespace hands::config {

struct Config {
    // --- [general] ---
    // Noms d'exécutables pour lesquels la couche s'active.
    // ATTENTION : valeur par défaut NON VÉRIFIÉE (le nom exact de MSFS 2024
    // doit être confirmé sur le PC de l'utilisateur, cf. scripts/check-process.ps1).
    std::vector<std::string> processAllowlist{"FlightSimulator2024.exe"};
    // Mode découverte : la couche s'active (en simple journalisation) quel que
    // soit le processus, pour révéler le nom exact de l'exécutable.
    bool discoveryMode = false;
    // Interrupteur général (application de la barre des tâches) : false = la couche laisse
    // tout passer. Lu à la création de l'instance OpenXR, donc effectif au prochain lancement du jeu.
    bool enabled = true;
    hands::log::LogLevel logLevel = hands::log::LogLevel::Info;

    // --- [probe] (étape 0) ---
    // Ajouter XR_EXT_hand_tracking à la liste des extensions si le jeu ne l'a
    // pas demandée et que le runtime la propose.
    bool enableHandTrackingExtension = true;
    // Au CreateSession : créer puis détruire un « hand tracker » pour prouver
    // que le suivi des mains fonctionne.
    bool probeHandTracker = true;
    // Journaliser le détail de chaque binding suggéré par le jeu.
    bool logBindings = true;

    // --- [emulation] (étape 1) ---
    // Remplace, pour le jeu, la pose des actions aim/grip par le bout de l'index suivi
    // (XR_EXT_hand_tracking) et simule la gâchette par un pincement pouce-index.
    bool emulationEnabled = true;
    // Origine du pointeur :
    //   "shoulder" = comme l'interface du casque : le rayon part du POINT DE CONTACT (là où le pouce touche le doigt) et sa direction va de
    //              l'ÉPAULE estimée vers ce point. La droite est longue (40 à 60 cm) : le rayon est très peu sensible aux doigts ;
    //              il suit la position de la main par rapport au corps ;
    //   "contact" = le rayon passe par le POINT DE CONTACT du clic (là où le pouce touche le doigt) : au clic il est aligné avec
    //              l'endroit visé ; direction du poignet vers ce point, origine ramenée vers la paume ;
    //   "palm"   = comme l'interface du casque : le rayon part de la PAUME et suit l'axe du doigt choisi dans sa pose naturelle ;
    //              la direction est lissée lentement, donc plier le doigt brièvement (clic) ne la déplace pas ;
    //   "hand"   = rigide à la paume : le rayon suit l'axe de la main (origine à la base du doigt) ;
    //   "finger" = suit l'orientation du bout du doigt.
    std::string pointerMode = "shoulder";
    // Mode "shoulder" : position estimée de l'épaule par rapport à la tête, en millimètres (écart latéral, hauteur sous la tête, recul).
    double shoulderSideMm = 180.0;
    double shoulderDropMm = 170.0;
    double shoulderBackMm = 50.0;
    // Mode "shoulder" : le rayon part d'un point rigide de la main : base du doigt avancée de ce nombre de millimètres le long de l'os de la main.
    double anchorForwardMm = 70.0;
    // Mode "shoulder" : d'où part le rayon. "fingertip" (défaut) = le bout du doigt du pointeur (pointer_finger) : le rayon part de la
    // pointe de la main, ce qui rapproche le focus du contact ; "bone" = point rigide sur l'os (anchor_forward_mm), pour un départ qui
    // ne dépend d'aucun doigt. Avec le majeur comme doigt du pointeur et un clic au pouce sur l'index, la pointe ne bouge pas au clic.
    std::string anchorSource = "fingertip";
    // Mode "shoulder" : quand les doigts pointent dans une autre direction que le bras (main verticale, bouton sur le côté ou en bas), la
    // direction du rayon bascule progressivement vers celle du doigt : 100 % épaule sous `fingerBlendStartDeg` d'écart, 100 % doigt au-delà de
    // `fingerBlendEndDeg`. Faux : le rayon ne dépend jamais de l'orientation des doigts.
    // Mode "shoulder" : filtre One Euro sur la direction du rayon. `shoulderSmoothHz` plus petit = plus stable a l'arret (0 = pas de filtre) ;
    // `shoulderSmoothBeta` plus grand = moins de retard quand la main bouge vite.
    double shoulderSmoothHz = 0.8;
    double shoulderSmoothBeta = 3.0;
    bool fingerBlend = true;
    double fingerBlendStartDeg = 30.0;
    double fingerBlendEndDeg = 70.0;
    // Mode "shoulder" : constante de temps (secondes) avec laquelle l'estimation du torse suit la rotation de la tête. Les épaules
    // restent en place quand on tourne la tête pour regarder un bouton sur le côté. 0 = les épaules suivent la tête instantanément.
    double torsoFollowS = 3.0;
    // Mode "palm" : articulation visée depuis la paume, "proximal", "intermediate" (défaut), "distal" ou "tip" du doigt choisi.
    std::string palmAim = "intermediate";
    // Mode "palm" : vitesse de suivi de la direction (Hz). Petit = très stable mais la direction réagit lentement à un changement de pose.
    double palmFollowHz = 0.6;
    // Doigt qui porte le pointeur : "index" (défaut) ou "majeur" (`middle`). Le majeur bouge moins quand l'index sert au clic ;
    // dans ce cas préférer click_mode = pinch (pouce sur le bout de l'index).
    std::string pointerFinger = "index";
    // Pointeur partagé : MSFS ne gère qu'un contrôleur actif à la fois. Quand c'est activé, la main « engagée » (celle dont le pouce
    // s'approche de la cible du clic, ou qui clique) fournit la pose et le clic aux DEUX emplacements du jeu : plus besoin de cacher
    // une main pour utiliser l'autre.
    bool sharedPointer = false;
    // Distance pouce-cible (mm) sous laquelle une main peut devenir « engagée » (doit dépasser click_on_mm).
    double engageMm = 34.0;
    // Réglage fin de la direction du pointeur, en degrés, dans le repère de la paume
    // (pitch positif = vers le haut, yaw positif = vers la gauche).
    double pointerPitchDeg = 0.0;
    double pointerYawDeg = 0.0;
    // Geste de clic : le pouce touche
    //   "side"   = le côté de l'index (articulation intermédiaire) : l'index reste fixe, le pointeur ne bouge pas ;
    //   "pinch"  = le bout de l'index (le pointeur bouge au moment du clic) ;
    //   "middle" = le bout du majeur.
    std::string clickMode = "any";
    // Masque les modèles de manettes du jeu (pose « grip » déclarée invalide). EXPÉRIMENTAL : si le jeu
    // cesse alors de réagir, remettre à false.
    bool hideControllerModel = false;
    // Distances pouce-cible (millimètres) : clic sous `clickOnMm`, relâchement au-dessus de `clickOffMm`.
    double clickOnMm = 20.0;
    double clickOffMm = 23.0;
    // Pendant un pincement, le pointeur reste ancré à la pose d'AVANT le pincement (le bout de l'index
    // bouge quand on pince) : on prend la pose telle qu'elle était `latchMs` millisecondes plus tôt.
    double latchMs = 150.0;
    // Le pointeur se fige des que le pouce est a moins de `clickOnMm + prelatchMm` de la cible et s'en rapproche (0 = seulement au clic),
    // puis reste dans une zone morte (degres / millimetres) tant que la main ne s'en eloigne pas : "ne pas bouger d'un iota" au clic.
    double prelatchMm = 14.0;
    double freezeDeadzoneDeg = 1.5;
    double freezeDeadzoneMm = 10.0;
    // N'utiliser que les articulations déclarées « suivies » (et non seulement « valides », donc
    // éventuellement extrapolées) : évite les faux appuis quand une main est perdue.
    bool requireTracked = true;
    // Le geste de clic doit-il n'utiliser que des articulations « suivies » ? Faux (défaut) : les positions
    // estimées par le runtime sont acceptées pour le pouce et la cible, ce qui permet de cliquer quand le
    // pouce est masqué (main au-dessus du pouce). La paume et le bout de l'index restent soumis à
    // `requireTracked`, donc une main perdue ne produit pas de faux clic.
    bool clickRequireTracked = false;
    // Lissage One Euro de l'orientation du pointeur (par rapport à la paume).
    bool smoothing = true;
    double smoothRotHz = 1.5;    // plus petit = plus stable à l'arrêt, mais plus de retard
    double smoothRotBeta = 0.8;  // plus grand = moins de retard en mouvement rapide

    // --- [touch] : contact avec les commandes, via la profondeur du jeu (en cours d'étude) ---
    // Sonde en lecture seule : copie quelques pixels des images de profondeur du jeu et les journalise avec la distance
    // des bouts de doigts, pour comprendre l'encodage de la profondeur. N'influence pas le jeu.
    bool depthProbe = true;
    int depthProbeIntervalMs = 500;
    // État dans lequel le jeu laisse ses images de profondeur : depth_write (défaut), depth_read, common, shader, copy_source.
    std::string depthProbeState = "depth_write";
    // Captures d'écran du rendu du jeu autour de la main (pour comprendre ce que MSFS dessine : flèche, rayon, point d'impact) :
    // au plus `captureMax` images par session (0 = aucune), enregistrées dans logs\captures, au plus une toutes les `captureIntervalMs`
    // (et à chaque début de clic).
    int captureMax = 0;
    int captureIntervalMs = 3000;
    std::string colorState = "render_target";  // état des images couleur du jeu (render_target, common, shader, copy_source)

    // --- [menu] : menu des réglages affiché dans le casque ---
    // Ouverture et fermeture : pincer les DEUX mains en même temps pendant `menuHoldMs`.
    // --- Zones (pochoir) : contact du bout de l'index avec des zones sphériques apprises (touche Ctrl+Alt+F9), qui déclenchent une touche de
    // StreamSim (POST /api/press). Sans zone apprise, ne fait rien. Voir layer/zones.cpp.
    // --- Mode « mains seules » : Virtual Desktop (VDXR) fabrique lui-même des manettes Touch à partir des mains suivies (profil
    // oculus/touch_controller actif sur les deux mains, pose aim = rayon, pincement = gâchette) : MSFS dessine alors des manettes fantômes, un
    // rayon, et prend les pincements pour des clics. Quand ce réglage est vrai (et que NOTRE émulation est coupée), la couche rend inactives les
    // poses et les entrées de manette d'une main tant que cette main est suivie ; les vraies manettes reviennent dès que les mains ne sont plus
    // suivies. Section [hands].
    bool hideHandControllers = true;
    double hideControllersHoldMs = 1000.0;   // la main doit rester non suivie ce temps (ms) avant que les manettes réapparaissent (anti-scintillement)
    // Déclare aussi « aucun profil d'interaction » sur la main masquée (xrGetCurrentInteractionProfile + évènement de changement synthétique) :
    // le jeu cesse alors de croire à une manette connectée.
    bool hideControllersProfile = true;
    bool zonesEnabled = true;
    double zoneRadiusMm = 20.0;   // rayon par défaut d'une zone (ancien format ; les zones ont maintenant une boîte)
    std::string zoneTeachMode = "pinch";  // pinch : pointer la commande puis pincer ; timer : compte a rebours (zone_teach_delay_ms)
    std::string zoneRecenterKey = "LSHIFT";  // touche de recentrage de MSFS : les zones suivent le cockpit (vide = aucune)
    double zoneRecenterDelayMs = 600.0;      // attente apres l'appui avant de relever la pose de la tete
    std::string zoneBoxFaces = "full";  // boîtes : full (12 arêtes) ou face (seulement la face avant : un carré / rectangle)
    std::string zoneShowBoxes = "all";  // boîtes en fil de fer : all (toutes), near (près des doigts), hidden (cachées)
    double zoneFingerMm = 6.0;    // taille du bout du doigt : la boîte est agrandie d'autant
    double zoneNearCm = 30.0;     // mode near : distance du bout du doigt en dessous de laquelle une boîte s'affiche
    double zoneDwellMs = 250.0;   // durée de maintien du doigt dans la zone avant de déclencher
    double zoneRearmMs = 400.0;   // durée hors de la zone avant de pouvoir redéclencher
    double zoneTeachDelayMs = 1500.0;  // delai entre Ctrl+Alt+F9 et l'enregistrement (le temps de poser le doigt)
    // Bouton de joystick / HOTAS (numerotation de joy.cpl, a partir de 1 ; 0 = aucun) qui memorise la zone / efface les zones. `zoneJoystick` :
    // morceau du nom du peripherique ("" = n'importe lequel). Se choisissent depuis le menu de la tray (appuyer sur le bouton voulu).
    std::string zoneJoystick;
    int zoneTeachButton = 0;
    int zoneClearButton = 0;
    int zoneServerPort = 3210;    // port de StreamSim (127.0.0.1)
    bool menuEnabled = true;
    // Applaudir (frapper les deux mains l'une contre l'autre) bascule le mode souris, sans passer par le menu.
    bool clapToggle = true;
    int menuHoldMs = 1200;
    // Autres facons d'afficher / cacher le menu : une touche du clavier (nom : LSHIFT, F9, M... ; vide = aucune) et / ou un bouton de manette / HOTAS.
    std::string menuToggleKey;
    int menuToggleButton = 0;            // numero du bouton (joy.cpl), 0 = aucun
    std::string menuToggleJoystick;      // morceau du nom du peripherique ("" = n'importe lequel)

    // --- [hands] (étape 3a) : squelette des mains dessiné en surimpression (Direct3D 12) ---
    std::string handsStyle = "skeleton";  // skeleton (cubes), glove (gant), bare (main nue), ghost (main translucide)
    double handsGhostAlpha = 0.35;        // opacité du mode ghost (0,05 à 1)
    bool handsEnabled = true;
    double reachScale = 1.0;         // portée des mains (0,5 à 2,5) : 1 = mains virtuelles = mains réelles
    double reachScaleForward = 1.0;  // facteurs par axe (0,5 à 2), multipliés avec reachScale
    double reachScaleSide = 1.0;
    double reachScaleUp = 1.0;
    double handsRenderScale = 0.75;  // résolution de la surimpression par rapport à celle du jeu
    double handsJointScale = 0.5;    // taille des articulations
    double handsBoneMm = 8.0;        // épaisseur des os (millimètres)

    static Config FromIni(const IniFile& ini);
};

// Vrai si `exePath` (chemin ou simple nom) figure dans la liste (comparaison du
// nom de fichier seul, insensible à la casse).
bool MatchesProcess(const std::vector<std::string>& allowlist, const std::string& exePath);

class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path file);
    ~ConfigStore();
    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    // Dernière configuration valide. Lecture sans verrou.
    std::shared_ptr<const Config> Get() const;

    // Relit le fichier maintenant. En cas d'échec (fichier absent/illisible),
    // l'ancienne configuration est conservée. `message` décrit le résultat.
    bool Reload(std::string* message = nullptr);

    // Lance un thread qui surveille la date de modification du fichier.
    // `onChange` est appelé (depuis ce thread) après chaque rechargement réussi.
    void StartWatching(std::chrono::milliseconds interval,
                       std::function<void(const Config&, const std::string&)> onChange);
    void StopWatching();

    const std::filesystem::path& File() const { return file_; }

private:
    struct Shared;  // état partagé avec le thread de surveillance
    std::filesystem::path file_;
    std::shared_ptr<Shared> shared_;
};

}  // namespace hands::config
