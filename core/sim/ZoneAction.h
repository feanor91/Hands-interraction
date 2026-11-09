// ZoneAction.h — une action elementaire envoyee au simulateur par le pont SimConnect (HandsBridge.exe).
//
//   type  : event | var | input | code | delay
//   mode  : press (event, code) · set | toggle | add (var, input)
//           set = fixe `value` ; toggle = bascule entre 0 et `value` (etat reel lu dans le simulateur) ;
//           add = ajoute `value` (pas, negatif possible), borne par min / max ou boucle si `wrap` (ex. un cap de 0 a 360).
//   delay : pause de `value` millisecondes entre deux actions d'une sequence.
//
// Protocole du tube nomme du pont (une requete = une ligne, champs separes par une tabulation ; reponse « ok » ou « err<TAB>message ») :
//   event<TAB>NOM<TAB>valeur            setvar / togglevar<TAB>NOM<TAB>unite<TAB>valeur
//   setinput / toggleinput<TAB>NOM<TAB>valeur
//   addvar<TAB>NOM<TAB>unite<TAB>pas<TAB>min<TAB>max<TAB>boucle    addinput<TAB>NOM<TAB>pas<TAB>min<TAB>max<TAB>boucle   (min / max : « - » = sans borne)
//   getvar<TAB>NOM<TAB>unite  /  getinput<TAB>NOM   -> ok<TAB>valeur
//   code<TAB>TEXTE   inputs   status   quit
#pragma once

#include <cstdio>
#include <string>

#include "sim/SimNames.h"

namespace hands::sim {

struct ZoneAction {
    std::string type;    // event | var | input | code | delay ; vide = pas d'action
    std::string target;
    std::string unit = "number";
    double value = 1.0;
    std::string mode = "set";  // press | set | toggle | add
    bool hasMin = false, hasMax = false, wrap = false;
    double min = 0.0, max = 0.0;
};

inline std::string Num(double v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

// Construit la requete du pont ; renvoie "" et remplit `err` si l'action est invalide, absente ou si c'est une pause (geree par l'appelant).
inline std::string BuildBridgeRequest(const ZoneAction& a, std::string* err) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return std::string();
    };
    const std::string minS = a.hasMin ? Num(a.min) : "-", maxS = a.hasMax ? Num(a.max) : "-", wrapS = a.wrap ? "1" : "0";
    if (a.type.empty()) return fail("aucune action");
    if (a.type == "delay") return fail("pause");
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
        if (a.mode == "add") return "addvar\t" + n + "\t" + u + "\t" + Num(a.value) + "\t" + minS + "\t" + maxS + "\t" + wrapS;
        return std::string(a.mode == "toggle" ? "togglevar" : "setvar") + "\t" + n + "\t" + u + "\t" + Num(a.value);
    }
    if (a.type == "input") {
        const std::string n = Trim(a.target);
        if (!IsValidInputEvent(n)) return fail("nom d'Input Event invalide");
        if (a.mode == "add") return "addinput\t" + n + "\t" + Num(a.value) + "\t" + minS + "\t" + maxS + "\t" + wrapS;
        return std::string(a.mode == "toggle" ? "toggleinput" : "setinput") + "\t" + n + "\t" + Num(a.value);
    }
    if (a.type == "code") {
        const std::string c = NormalizeCode(a.target);
        if (c.empty() || IsCodeTooLong(c)) return fail("code avionique vide ou trop long");
        if (c.find('\t') != std::string::npos || c.find('\n') != std::string::npos) return fail("code invalide");
        return "code\t" + c;
    }
    return fail("type d'action inconnu (event, var, input, code ou delay)");
}

}  // namespace hands::sim
