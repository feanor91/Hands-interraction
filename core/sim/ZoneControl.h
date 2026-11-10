// ZoneControl.h — type de commande d'une zone (comme les touches de StreamSim) et ses actions.
//
//   push    poussoir : un contact du doigt envoie la sequence `press`.
//   toggle  bascule  : idem, mais l'action est en mode « toggle » (etat reel lu dans le simulateur) ; presentation distincte dans l'editeur.
//   switch  interrupteur a N positions : chaque contact passe a la position suivante (en boucle ou aller-retour) et envoie `pos1`..`posN`.
//   dial    rotatif  : pincer la commande puis, selon `gesture`, tourner le poignet (twist : un cran tous les `step_deg` degres) ou glisser le long
//                      de la boite (slide : un cran tous les `step_deg` millimetres) envoie `inc` / `dec` ; pincer-relacher sans bouger envoie `press`.
//   slider  curseur  : deux points appris (debut et fin de course) ; pincer et glisser. Mode `value` : envoie `set` avec la valeur
//                      min..max correspondant a la position ; mode `steps` : envoie `inc` / `dec` par cran (`notches` crans sur la course).
//
// Chaque emplacement (press, pos1.., inc, dec, set) est une SEQUENCE d'actions ecrite sur une ligne de zones.ini :
//     press = input|toggle|1|number|||0|LANDING_GEAR_PARKINGBRAKE >> delay|set|300|number|||0| >> event|press|1|number|||0|PARKING_BRAKES
// Une action = type|mode|valeur|unite|min|max|boucle|cible (la cible, en dernier, peut contenir « | » ; les actions sont separees par « >> »).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "config/IniFile.h"
#include "sim/ZoneAction.h"

namespace hands::sim {

using ActionSeq = std::vector<ZoneAction>;

// --- Texte <-> actions ---------------------------------------------------------------------------------------------------------

inline std::string FormatAction(const ZoneAction& a) {
    return a.type + "|" + a.mode + "|" + Num(a.value) + "|" + a.unit + "|" + (a.hasMin ? Num(a.min) : "") + "|" + (a.hasMax ? Num(a.max) : "") + "|" +
           (a.wrap ? "1" : "0") + "|" + a.target;
}

inline bool ParseAction(const std::string& spec, ZoneAction* out) {
    ZoneAction a;
    size_t p = 0;
    std::string f[7];
    for (int i = 0; i < 7; ++i) {
        const size_t q = spec.find('|', p);
        if (q == std::string::npos) return false;
        f[i] = Trim(spec.substr(p, q - p));
        p = q + 1;
    }
    a.target = Trim(spec.substr(p));
    a.type = f[0];
    a.mode = f[1].empty() ? "set" : f[1];
    a.value = f[2].empty() ? 1.0 : std::atof(f[2].c_str());
    a.unit = f[3].empty() ? "number" : f[3];
    if (!f[4].empty()) { a.hasMin = true; a.min = std::atof(f[4].c_str()); }
    if (!f[5].empty()) { a.hasMax = true; a.max = std::atof(f[5].c_str()); }
    a.wrap = f[6] == "1";
    if (a.type.empty()) return false;
    *out = a;
    return true;
}

inline std::string FormatSeq(const ActionSeq& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) o += (i ? " >> " : "") + FormatAction(s[i]);
    return o;
}

inline ActionSeq ParseSeq(const std::string& text) {
    ActionSeq out;
    size_t p = 0;
    while (p <= text.size()) {
        const size_t q = text.find(" >> ", p);
        ZoneAction a;
        if (ParseAction(text.substr(p, q == std::string::npos ? std::string::npos : q - p), &a)) out.push_back(a);
        if (q == std::string::npos) break;
        p = q + 4;
    }
    return out;
}

// --- Type de commande ----------------------------------------------------------------------------------------------------------

struct ZoneControl {
    std::string kind = "push";  // push | toggle | switch | dial | slider
    std::map<std::string, ActionSeq> slots;
    // interrupteur a N positions
    int count = 3;
    std::string switchMode = "bounce";  // bounce | cycle
    std::string read;                   // « var|NOM|unite » ou « input|NOM » : variable qui donne la position reelle (facultatif)
    std::string readValues;             // valeurs de la variable pour chaque position, separees par des virgules (facultatif)
    // rotatif
    double stepDeg = 25.0;                // pas : degres (geste twist) ou millimetres (geste slide)
    bool invert = false;
    bool dual = false;                    // deux niveaux (molette concentrique : bague exterieure + bouton central) : emplacements inc2 / dec2 / press2
    double innerRatio = 0.5;              // taille de la boite du bouton central par rapport a la boite exterieure (X et Y)
    std::string gesture = "twist";        // twist : pincer et tourner le poignet ; slide : pincer et glisser le long de la boite (axe X local)
    // curseur
    std::string sliderMode = "steps";  // value | steps
    int notches = 10;
    double min = 0.0, max = 16383.0;
};

inline bool IsKind(const std::string& k) { return k == "push" || k == "toggle" || k == "switch" || k == "dial" || k == "slider"; }

// Emplacements d'actions d'un type de commande, dans l'ordre d'affichage.
inline std::vector<std::string> SlotNames(const ZoneControl& c) {
    if (c.kind == "switch") {
        std::vector<std::string> v;
        for (int i = 1; i <= std::max(2, std::min(12, c.count)); ++i) v.push_back("pos" + std::to_string(i));
        return v;
    }
    if (c.kind == "dial") return c.dual ? std::vector<std::string>{"inc", "dec", "press", "inc2", "dec2", "press2"} : std::vector<std::string>{"inc", "dec", "press"};
    if (c.kind == "slider") return c.sliderMode == "value" ? std::vector<std::string>{"set"} : std::vector<std::string>{"inc", "dec"};
    return {"press"};
}

inline const ActionSeq& Slot(const ZoneControl& c, const std::string& name) {
    static const ActionSeq none;
    auto it = c.slots.find(name);
    return it == c.slots.end() ? none : it->second;
}

inline std::string Join(const std::vector<std::string>& v, const char* sep) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) o += (i ? sep : "") + v[i];
    return o;
}

// Resume d'une zone pour les listes : « Poussoir : PARKING_BRAKES ».
inline std::string Summary(const ZoneControl& c) {
    static const std::map<std::string, std::string> names = {{"push", "Poussoir"}, {"toggle", "Bascule"}, {"switch", "Interrupteur"}, {"dial", "Rotatif"}, {"slider", "Curseur"}};
    std::string first;
    for (const std::string& s : SlotNames(c)) {
        const ActionSeq& q = Slot(c, s);
        if (!q.empty()) { first = q[0].target; break; }
    }
    return names.at(IsKind(c.kind) ? c.kind : "push") + " : " + first;
}

// Lit une section de zones.ini. Ancien format (action / target / mode / value / unit) accepte : c'est l'emplacement `press`.
inline ZoneControl ParseControl(const hands::config::IniFile& ini, const std::string& sec) {
    ZoneControl c;
    c.kind = ini.GetString(sec, "control", "push");
    if (!IsKind(c.kind)) c.kind = "push";
    c.count = std::max(2, std::min(12, ini.GetInt(sec, "positions", 3)));
    c.switchMode = ini.GetString(sec, "switch_mode", "bounce") == "cycle" ? "cycle" : "bounce";
    c.read = ini.GetString(sec, "read", "");
    c.readValues = ini.GetString(sec, "read_values", "");
    c.stepDeg = std::max(2.0, std::min(90.0, ini.GetDouble(sec, "step_deg", 25.0)));
    c.invert = ini.GetBool(sec, "invert", false);
    c.gesture = ini.GetString(sec, "gesture", "twist") == "slide" ? "slide" : "twist";
    c.dual = ini.GetInt(sec, "levels", 1) >= 2;
    c.innerRatio = std::max(0.2, std::min(0.9, ini.GetDouble(sec, "inner_ratio", 0.5)));
    c.sliderMode = ini.GetString(sec, "slider_mode", "steps") == "value" ? "value" : "steps";
    c.notches = std::max(1, std::min(100, ini.GetInt(sec, "notches", 10)));
    c.min = ini.GetDouble(sec, "min", 0.0);
    c.max = ini.GetDouble(sec, "max", 16383.0);
    for (const char* base : {"press", "inc", "dec", "set", "press2", "inc2", "dec2"}) {
        const ActionSeq q = ParseSeq(ini.GetString(sec, base, ""));
        if (!q.empty()) c.slots[base] = q;
    }
    for (int i = 1; i <= 12; ++i) {
        const std::string k = "pos" + std::to_string(i);
        const ActionSeq q = ParseSeq(ini.GetString(sec, k, ""));
        if (!q.empty()) c.slots[k] = q;
    }
    if (!c.slots.count("press") && ini.Has(sec, "action")) {  // ancien format
        ZoneAction a;
        a.type = ini.GetString(sec, "action", "");
        a.target = ini.GetString(sec, "target", "");
        a.unit = ini.GetString(sec, "unit", "number");
        a.value = ini.GetDouble(sec, "value", 1.0);
        a.mode = ini.GetString(sec, "mode", a.type == "event" || a.type == "code" ? "press" : "set");
        if (!a.type.empty()) c.slots["press"] = {a};
    }
    return c;
}

inline std::string SerializeControl(const ZoneControl& c) {
    std::string s = "control = " + c.kind + "\n";
    if (c.kind == "switch") {
        s += "positions = " + std::to_string(c.count) + "\nswitch_mode = " + c.switchMode + "\n";
        if (!c.read.empty()) s += "read = " + c.read + "\n";
        if (!c.readValues.empty()) s += "read_values = " + c.readValues + "\n";
    } else if (c.kind == "dial") {
        if (c.dual) s += "levels = 2\ninner_ratio = " + Num(c.innerRatio) + "\n";
        s += "gesture = " + c.gesture + "\nstep_deg = " + Num(c.stepDeg) + "\ninvert = " + (c.invert ? "true" : "false") + "\n";
    } else if (c.kind == "slider") {
        s += "slider_mode = " + c.sliderMode + "\nnotches = " + std::to_string(c.notches) + "\nmin = " + Num(c.min) + "\nmax = " + Num(c.max) + "\n";
    }
    for (const std::string& n : SlotNames(c)) {
        const ActionSeq& q = Slot(c, n);
        if (!q.empty()) s += n + " = " + FormatSeq(q) + "\n";
    }
    return s;
}

// Verifie qu'une commande est utilisable ; renvoie le message d'erreur, ou "" si tout va bien.
inline std::string Validate(const ZoneControl& c) {
    bool any = false;
    for (const std::string& n : SlotNames(c)) {
        const ActionSeq& q = Slot(c, n);
        for (const ZoneAction& a : q) {
            std::string err;
            if (a.type != "delay" && BuildBridgeRequest(a, &err).empty()) return n + " : " + err;
            any = true;
        }
    }
    if (c.kind == "dial" && (Slot(c, "inc").empty() || Slot(c, "dec").empty())) return "un rotatif a besoin des actions inc et dec";
    if (c.kind == "dial" && c.dual && (Slot(c, "inc2").empty() || Slot(c, "dec2").empty())) return "le bouton central a besoin des actions inc2 et dec2";
    if (c.kind == "slider" && c.sliderMode == "value" && Slot(c, "set").empty()) return "un curseur en mode valeur a besoin de l'action set";
    if (c.kind == "slider" && c.sliderMode == "steps" && (Slot(c, "inc").empty() || Slot(c, "dec").empty())) return "un curseur par crans a besoin de inc et dec";
    if (c.kind == "switch")
        for (const std::string& n : SlotNames(c))
            if (Slot(c, n).empty()) return "il manque l'action de " + n;
    return any ? std::string() : std::string("aucune action");
}

// --- Calculs de gestes (testes) --------------------------------------------------------------------------------------------------

// Position suivante d'un interrupteur a N positions : « cycle » 0..N-1 puis 0 ; « bounce » aller-retour (dir = sens memorise, +1 / -1).
inline void NextPosition(int count, const std::string& mode, int cur, int dir, int* pos, int* newDir) {
    const int n = std::max(2, count);
    const int c = std::max(0, std::min(n - 1, cur));
    if (mode == "bounce") {
        int d = dir < 0 ? -1 : 1;
        if (c + d > n - 1 || c + d < 0) d = -d;
        *pos = c + d;
        *newDir = d;
    } else {
        *pos = (c + 1) % n;
        *newDir = 1;
    }
}

// Valeur lue dans le simulateur -> numero de position (table `values` de n valeurs : la plus proche ; sinon la valeur arrondie).
inline int PositionOf(double value, const std::vector<double>& values, int count) {
    if (static_cast<int>(values.size()) == count) {
        int best = 0;
        for (int i = 1; i < count; ++i)
            if (std::fabs(values[static_cast<size_t>(i)] - value) < std::fabs(values[static_cast<size_t>(best)] - value)) best = i;
        return best;
    }
    return std::max(0, std::min(count - 1, static_cast<int>(std::lround(value))));
}

// Curseur : position 0..1 -> valeur envoyee, et nombre de crans entre deux positions.
inline double LevelToValue(double level, double min, double max) { return std::round(min + std::max(0.0, std::min(1.0, level)) * (max - min)); }
inline int NotchDelta(double from, double to, int notches) {
    const int n = std::max(1, std::min(100, notches));
    auto q = [&](double v) { return static_cast<int>(std::lround(std::max(0.0, std::min(1.0, v)) * n)); };
    return q(to) - q(from);
}

// Rotatif : transforme une variation d'angle (degres) en nombre de crans signes ; le reste est conserve pour la suite.
struct DialAccumulator {
    double acc = 0.0;
    int Feed(double deltaDeg, double stepDeg) {
        acc += deltaDeg;
        int steps = 0;
        while (acc >= stepDeg) { acc -= stepDeg; ++steps; }
        while (acc <= -stepDeg) { acc += stepDeg; --steps; }
        return steps;
    }
};

}  // namespace hands::sim
