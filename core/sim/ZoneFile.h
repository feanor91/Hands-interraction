// ZoneFile.h — lecture / ecriture de zones.ini (modele partage par la fenetre de configuration de la tray).
// Meme format que celui ecrit par la couche (layer/zones.cpp) : une section par zone, type de commande + actions + positions apprises.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "config/IniFile.h"
#include "sim/ZoneControl.h"

namespace hands::sim {

struct ZoneRecord {
    std::string id;
    ZoneControl control;
    bool hasPos = false;
    double x = 0, y = 0, z = 0;
    bool hasPos2 = false;  // curseur : fin de course
    double x2 = 0, y2 = 0, z2 = 0;
    double radiusMm = 0;  // 0 = rayon par defaut de [zones] radius_mm
};

// Position complete = premier point, et pour un curseur aussi le second.
inline bool Complete(const ZoneRecord& r) { return r.hasPos && (r.control.kind != "slider" || r.hasPos2); }

inline std::vector<ZoneRecord> ParseZones(const std::string& text) {
    std::vector<ZoneRecord> out;
    const auto ini = hands::config::IniFile::Parse(text);
    for (const std::string& sec : ini.Sections()) {
        ZoneRecord r;
        r.id = sec;
        r.control = ParseControl(ini, sec);
        if (ini.Has(sec, "x")) {
            r.hasPos = true;
            r.x = ini.GetDouble(sec, "x", 0);
            r.y = ini.GetDouble(sec, "y", 0);
            r.z = ini.GetDouble(sec, "z", 0);
            r.radiusMm = ini.GetDouble(sec, "radius_mm", 0);
        }
        if (ini.Has(sec, "x2")) {
            r.hasPos2 = true;
            r.x2 = ini.GetDouble(sec, "x2", 0);
            r.y2 = ini.GetDouble(sec, "y2", 0);
            r.z2 = ini.GetDouble(sec, "z2", 0);
        }
        out.push_back(std::move(r));
    }
    return out;
}

inline std::string SerializeZones(const std::vector<ZoneRecord>& zones) {
    std::string s =
        "; Zones du pochoir. Une section par zone : type de commande (control), actions, position apprise (espace du jeu, metres).\n"
        "; control = push | toggle | switch | dial | slider ; les actions sont des sequences : type|mode|valeur|unite|min|max|boucle|cible >> ...\n"
        "; type = event | var | input | code | delay ; mode = press | set | toggle | add. Voir core/sim/ZoneControl.h.\n"
        "; Une zone sans x/y/z attend d'etre apprise : le bouton de memorisation apprend la premiere de la liste qui n'a pas de position.\n\n";
    char b[200];
    for (const ZoneRecord& z : zones) {
        s += "[" + z.id + "]\n" + SerializeControl(z.control);
        if (z.hasPos) {
            std::snprintf(b, sizeof(b), "x = %.4f\ny = %.4f\nz = %.4f\nradius_mm = %.0f\n", z.x, z.y, z.z, z.radiusMm);
            s += b;
        }
        if (z.hasPos2) {
            std::snprintf(b, sizeof(b), "x2 = %.4f\ny2 = %.4f\nz2 = %.4f\n", z.x2, z.y2, z.z2);
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
