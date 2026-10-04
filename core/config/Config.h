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
