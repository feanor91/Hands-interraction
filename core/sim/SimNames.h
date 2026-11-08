// SimNames.h — noms de commandes du simulateur : validation et mise en forme (portable, testé). Repris de StreamSim (shared/msfs.js).
//
// Quatre sortes de commandes :
//  - événement : un événement clavier/SimConnect de MSFS (« PARKING_BRAKES », « A32NX.FCU_HDG_PUSH ») ;
//  - variable  : une SimVar standard (« GEAR HANDLE POSITION ») ou une variable locale d'avion préfixée « L: » (« L:AZP_RAF_EFB_ON ») ;
//  - input     : une commande de cockpit de MSFS 2024 (Input Event), propre à l'avion chargé ;
//  - code      : du code avionique (RPN) exécuté par le module MobiFlight WASM, pour les événements « H: » et « B: ».
#pragma once

#include <string>

namespace hands::sim {

inline std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

inline std::string Upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

inline bool IsAlnum(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

// Événement : 2 à 64 caractères parmi A-Z 0-9 _ . # - (comme StreamSim).
inline bool IsValidEventName(const std::string& name) {
    if (name.size() < 2 || name.size() > 64) return false;
    for (char c : name)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '#' || c == '-')) return false;
    return true;
}

inline bool IsLocalVar(const std::string& name) {
    const std::string v = Trim(name);
    return v.size() >= 2 && (v[0] == 'L' || v[0] == 'l') && v[1] == ':';
}

// Variable : « L:NOM » (1 à 96 caractères parmi lettres, chiffres, _ . : -) ou SimVar standard « GEAR HANDLE POSITION » / « NAV OBS:1 ».
inline bool IsValidSimvar(const std::string& name) {
    const std::string v = Trim(name);
    if (IsLocalVar(v)) {
        const size_t n = v.size() - 2;
        if (n < 1 || n > 96) return false;
        for (size_t i = 2; i < v.size(); ++i) {
            const char c = v[i];
            if (!(IsAlnum(c) || c == '_' || c == '.' || c == ':' || c == '-')) return false;
        }
        return true;
    }
    // SimVar standard : majuscules, chiffres, espaces, _ ; suffixe facultatif « :N » (1 ou 2 chiffres).
    std::string base = v;
    const size_t colon = v.find(':');
    if (colon != std::string::npos) {
        const std::string idx = v.substr(colon + 1);
        if (idx.empty() || idx.size() > 2) return false;
        for (char c : idx)
            if (c < '0' || c > '9') return false;
        base = v.substr(0, colon);
    }
    if (base.size() < 2 || base.size() > 64) return false;
    for (char c : base)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_')) return false;
    return true;
}

// Forme canonique : SimVar standard en majuscules, variable « L: » telle quelle (préfixe en majuscule).
inline std::string NormalizeVar(const std::string& name) {
    const std::string v = Trim(name);
    if (IsLocalVar(v)) return "L:" + v.substr(2);
    return Upper(v);
}

// Unité : 1 à 40 caractères parmi lettres, chiffres, espace et / _ . -
inline bool IsValidUnit(const std::string& unit) {
    if (unit.empty() || unit.size() > 40) return false;
    for (char c : unit)
        if (!(IsAlnum(c) || c == ' ' || c == '/' || c == '_' || c == '.' || c == '-')) return false;
    return true;
}

// Input Event : 2 à 128 caractères parmi lettres, chiffres, _ . : # -
inline bool IsValidInputEvent(const std::string& name) {
    if (name.size() < 2 || name.size() > 128) return false;
    for (char c : name)
        if (!(IsAlnum(c) || c == '_' || c == '.' || c == ':' || c == '#' || c == '-')) return false;
    return true;
}

// Nom de l'avion à partir du chemin de son fichier de configuration. Dans MSFS 2024 ce fichier est rangé dans des sous-dossiers génériques
// (« ...\rafale-c\config\aircraft.cfg ») : on les saute.
inline std::string AircraftFromPath(const std::string& path) {
    std::string parts[64];
    int n = 0;
    std::string cur;
    for (char c : path) {
        if (c == '\\' || c == '/') {
            if (!cur.empty() && n < 64) parts[n++] = cur;
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty() && n < 64) parts[n++] = cur;
    auto lowerEq = [](std::string a, const char* b) {
        for (char& c : a)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return a == b;
    };
    if (n < 2) return path;
    const std::string& last = parts[n - 1];
    const size_t dot = last.rfind('.');
    const std::string ext = dot == std::string::npos ? "" : Upper(last.substr(dot + 1));
    if (ext != "AIR" && ext != "CFG" && ext != "FLT") return path;
    static const char* const kGeneric[] = {"config", "presets", "common", "model", "models", "texture", "airplanes", "simobjects", "attachments"};
    for (int i = n - 2; i >= 0; --i) {
        bool generic = false;
        for (const char* g : kGeneric)
            if (lowerEq(parts[i], g)) generic = true;
        if (!generic) return parts[i];
    }
    return parts[n - 2];
}

// Code avionique : espaces réduits ; au plus 1023 octets une fois préfixé par « MF.SimVars.Set. » (messages de 1024 octets du module MobiFlight).
inline std::string NormalizeCode(const std::string& code) {
    std::string out;
    bool space = false;
    for (char c : code) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}
inline bool IsCodeTooLong(const std::string& normalizedCode) { return std::string("MF.SimVars.Set.").size() + normalizedCode.size() >= 1024; }

}  // namespace hands::sim
