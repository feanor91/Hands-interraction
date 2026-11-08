// ZoneAction.h — action d'une zone du pochoir : ce que le pont SimConnect doit faire quand le doigt touche la zone.
//
// Dans zones.ini :   action = event | var | input | code
//                    target = PARKING_BRAKES | L:AZP_RAF_X | AZP_RAF_INPUT | (>H:AZP_RAF_X)
//                    unit   = number (var seulement)       value = 1       mode = press | set | toggle
// - event : envoie l'evenement SimConnect `target` (value facultative).      mode ignore (toujours un appui).
// - var   : ecrit la variable `target` ; mode `set` = value, `toggle` = bascule entre 0 et value (lecture de l'etat reel par le pont).
// - input : idem pour un Input Event de l'avion (MSFS 2024).
// - code  : code avionique (RPN), ex. « (>H:AZP_RAF_ALARMS_ACKNOWLEDGE) ».
//
// Protocole du tube nomme du pont (une requete = une ligne, champs separes par une tabulation ; reponse « ok » ou « err<TAB>message ») :
//   event<TAB>NOM<TAB>valeur   setvar<TAB>NOM<TAB>unite<TAB>valeur   togglevar<TAB>NOM<TAB>unite<TAB>valeur
//   setinput<TAB>NOM<TAB>valeur   toggleinput<TAB>NOM<TAB>valeur   code<TAB>TEXTE   status
#pragma once

#include <cstdio>
#include <string>

#include "sim/SimNames.h"

namespace hands::sim {

struct ZoneAction {
    std::string type;    // event | var | input | code ; vide = zone sans action
    std::string target;
    std::string unit = "number";
    double value = 1.0;
    std::string mode = "set";  // press | set | toggle
};

inline std::string Num(double v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

// Construit la requete du pont ; renvoie "" et remplit `err` si l'action est invalide ou absente.
inline std::string BuildBridgeRequest(const ZoneAction& a, std::string* err) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return std::string();
    };
    if (a.type.empty()) return fail("aucune action");
    if (a.type == "event") {
        const std::string n = Upper(Trim(a.target));
        if (!IsValidEventName(n)) return fail("nom d'evenement invalide");
        return "event\t" + n + "\t" + std::to_string(static_cast<long long>(a.value));
    }
    if (a.type == "var") {
        const std::string n = NormalizeVar(Trim(a.target));
        if (!IsValidSimvar(n)) return fail("nom de variable invalide");
        const std::string u = a.unit.empty() ? "number" : a.unit;
        if (!IsValidUnit(u)) return fail("unite invalide");
        return std::string(a.mode == "toggle" ? "togglevar" : "setvar") + "\t" + n + "\t" + u + "\t" + Num(a.value);
    }
    if (a.type == "input") {
        const std::string n = Trim(a.target);
        if (!IsValidInputEvent(n)) return fail("nom d'Input Event invalide");
        return std::string(a.mode == "toggle" ? "toggleinput" : "setinput") + "\t" + n + "\t" + Num(a.value);
    }
    if (a.type == "code") {
        const std::string c = NormalizeCode(a.target);
        if (c.empty() || IsCodeTooLong(c)) return fail("code avionique vide ou trop long");
        if (c.find('\t') != std::string::npos || c.find('\n') != std::string::npos) return fail("code invalide");
        return "code\t" + c;
    }
    return fail("type d'action inconnu (event, var, input ou code)");
}

}  // namespace hands::sim
