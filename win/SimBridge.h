// SimBridge.h — pont vers Microsoft Flight Simulator (2020 / 2024) par SimConnect. Port en C++ de la liaison de StreamSim (server/msfs.js).
//
// Un fil de travail se connecte au simulateur (nouvel essai toutes les 5 s tant qu'il n'est pas lancé), s'y reconnecte après un changement de
// vol et exécute les commandes. Toutes les fonctions publiques peuvent être appelées depuis n'importe quel fil. SimConnect est intégré au
// simulateur : aucun fichier de configuration n'est nécessaire quand le programme tourne sur le même PC.
//
// Commandes : événement (SendEvent), variable SimVar ou « L: » (SetVar / ReadVar), Input Event de MSFS 2024 (SetInput / ReadInput / ListInputs),
// code avionique pour les événements « H: » et « B: » (ExecCode, par le module MobiFlight WASM, souvent fourni avec les avions complexes).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hands::win {

struct SimStatus {
    bool connected = false;
    std::string simName;   // « Microsoft Flight Simulator 2024 »...
    std::string aircraft;  // nom de l'avion chargé (vide si inconnu)
    std::string reason;    // pourquoi le pont n'est pas connecté
    bool mobiFlight = false;  // le module MobiFlight WASM a répondu à un ping
};

struct InputEventInfo {
    std::string name;
    uint64_t hash = 0;
    bool isString = false;
};

class SimBridge {
public:
    using LogFn = std::function<void(const std::string&)>;
    explicit SimBridge(LogFn log = nullptr);
    ~SimBridge();
    SimBridge(const SimBridge&) = delete;
    SimBridge& operator=(const SimBridge&) = delete;

    void Start();
    void Stop();
    SimStatus Status() const;

    // --- Commandes (envoi asynchrone : retourne vrai si la commande est acceptée) -------------------------------------------------
    // Événement SimConnect (ex. « PARKING_BRAKES »), avec une valeur facultative.
    bool SendEvent(const std::string& name, int value, std::string* err);
    // Écrit une variable (« L:AZP_RAF_EFB_ON » ou SimVar modifiable) ; `unit` vide = « number ».
    bool SetVar(const std::string& name, const std::string& unit, double value, std::string* err);
    // Écrit un Input Event de l'avion chargé (liste lue au besoin : peut attendre quelques secondes).
    bool SetInput(const std::string& name, double value, std::string* err);
    // Code avionique (RPN), ex. « (>H:AZP_RAF_ALARMS_ACKNOWLEDGE) 1 (>L:AZP_RAF_VTLG_PAGE_SWITCH_L, Boolean) ».
    bool ExecCode(const std::string& code, std::string* err);

    // --- Lectures (bloquantes, avec délai) ----------------------------------------------------------------------------------------
    bool ReadVar(const std::string& name, const std::string& unit, double* out, int timeoutMs, std::string* err);
    bool ListInputs(std::vector<InputEventInfo>* out, bool force, int timeoutMs, std::string* err);
    bool ReadInput(const std::string& name, double* out, int timeoutMs, std::string* err);
    // Liste des variables « L: » de l'avion, par le module MobiFlight WASM (installe dans Community ; 1000 noms au plus, tries, sans « L: »).
    bool ListLVars(std::vector<std::string>* out, int timeoutMs, std::string* err);

private:
    struct Impl;
    Impl* p_;
};

}  // namespace hands::win
