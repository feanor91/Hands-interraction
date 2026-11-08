// ZoneFile.h — lecture / ecriture de zones.ini (modele partage par la fenetre de configuration de la tray).
// Meme format que celui ecrit par la couche (layer/zones.cpp) : une section par zone, action + position apprise.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "config/IniFile.h"
#include "sim/ZoneAction.h"

namespace hands::sim {

struct ZoneRecord {
    std::string id;
    ZoneAction action;
    bool hasPos = false;
    double x = 0, y = 0, z = 0;
    double radiusMm = 0;  // 0 = rayon par defaut de [zones] radius_mm
};

inline std::vector<ZoneRecord> ParseZones(const std::string& text) {
    std::vector<ZoneRecord> out;
    const auto ini = hands::config::IniFile::Parse(text);
    for (const std::string& sec : ini.Sections()) {
        ZoneRecord r;
        r.id = sec;
        r.action.type = ini.GetString(sec, "action", "");
        r.action.target = ini.GetString(sec, "target", "");
        r.action.unit = ini.GetString(sec, "unit", "number");
        r.action.value = ini.GetDouble(sec, "value", 1.0);
        r.action.mode = ini.GetString(sec, "mode", r.action.type == "event" || r.action.type == "code" ? "press" : "set");
        if (ini.Has(sec, "x")) {
            r.hasPos = true;
            r.x = ini.GetDouble(sec, "x", 0);
            r.y = ini.GetDouble(sec, "y", 0);
            r.z = ini.GetDouble(sec, "z", 0);
            r.radiusMm = ini.GetDouble(sec, "radius_mm", 0);
        }
        out.push_back(std::move(r));
    }
    return out;
}

inline std::string SerializeZones(const std::vector<ZoneRecord>& zones) {
    std::string s =
        "; Zones du pochoir. Une section par zone. Position apprise (espace du jeu, metres) = x, y, z ; l'action est envoyee au pont SimConnect.\n"
        "; action = event | var | input | code     target = nom (PARKING_BRAKES, L:AZP_RAF_X, nom d'Input Event, ou code avionique)\n"
        "; mode = press | set | toggle              unit = number (variables)      value = 1\n"
        "; Une zone sans x/y/z attend d'etre apprise : le bouton de memorisation apprend la premiere de la liste qui n'a pas de position.\n\n";
    char b[160];
    for (const ZoneRecord& z : zones) {
        s += "[" + z.id + "]\naction = " + z.action.type + "\ntarget = " + z.action.target + "\nmode = " + z.action.mode + "\n";
        if (z.action.type == "var") s += "unit = " + z.action.unit + "\n";
        std::snprintf(b, sizeof(b), "value = %g\n", z.action.value);
        s += b;
        if (z.hasPos) {
            std::snprintf(b, sizeof(b), "x = %.4f\ny = %.4f\nz = %.4f\nradius_mm = %.0f\n", z.x, z.y, z.z, z.radiusMm);
            s += b;
        }
        s += "\n";
    }
    return s;
}

// Nom de zone valide : minuscules, chiffres et tiret bas (pas de point : c'est le separateur interne de l'INI).
inline std::string SanitizeZoneId(const std::string& in) {
    std::string o;
    for (char c : in) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') o += c;
        else if (c == ' ' || c == '-') o += '_';
    }
    return o;
}

}  // namespace hands::sim
