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
    bool discoveryMode = true;
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
    //   "palm"   = comme l'interface du casque : le rayon part de la PAUME et suit l'axe du doigt choisi dans sa pose naturelle ;
    //              la direction est lissée lentement, donc plier le doigt brièvement (clic) ne la déplace pas ;
    //   "hand"   = rigide à la paume : le rayon suit l'axe de la main (origine à la base du doigt) ;
    //   "finger" = suit l'orientation du bout du doigt.
    std::string pointerMode = "palm";
    // Mode "palm" : articulation visée depuis la paume, "proximal", "intermediate" (défaut), "distal" ou "tip" du doigt choisi.
    std::string palmAim = "intermediate";
    // Mode "palm" : vitesse de suivi de la direction (Hz). Petit = très stable mais la direction réagit lentement à un changement de pose.
    double palmFollowHz = 0.6;
    // Doigt qui porte le pointeur : "index" (défaut) ou "majeur" (`middle`). Le majeur bouge moins quand l'index sert au clic ;
    // dans ce cas préférer click_mode = pinch (pouce sur le bout de l'index).
    std::string pointerFinger = "index";
    // Réglage fin de la direction du pointeur, en degrés, dans le repère de la paume
    // (pitch positif = vers le haut, yaw positif = vers la gauche).
    double pointerPitchDeg = 0.0;
    double pointerYawDeg = 0.0;
    // Geste de clic : le pouce touche
    //   "side"   = le côté de l'index (articulation intermédiaire) : l'index reste fixe, le pointeur ne bouge pas ;
    //   "pinch"  = le bout de l'index (le pointeur bouge au moment du clic) ;
    //   "middle" = le bout du majeur.
    std::string clickMode = "side";
    // Masque les modèles de manettes du jeu (pose « grip » déclarée invalide). EXPÉRIMENTAL : si le jeu
    // cesse alors de réagir, remettre à false.
    bool hideControllerModel = false;
    // Distances pouce-cible (millimètres) : clic sous `clickOnMm`, relâchement au-dessus de `clickOffMm`.
    double clickOnMm = 21.0;
    double clickOffMm = 27.0;
    // Pendant un pincement, le pointeur reste ancré à la pose d'AVANT le pincement (le bout de l'index
    // bouge quand on pince) : on prend la pose telle qu'elle était `latchMs` millisecondes plus tôt.
    double latchMs = 150.0;
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

    // --- [hands] (étape 3a) : squelette des mains dessiné en surimpression (Direct3D 12) ---
    bool handsEnabled = true;
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
