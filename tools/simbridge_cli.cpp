// simbridge_cli — essai en ligne de commande du pont SimConnect (win/SimBridge). MSFS doit être lancé.
//
//   simbridge_cli status
//   simbridge_cli event PARKING_BRAKES [valeur]
//   simbridge_cli get-var "GEAR HANDLE POSITION" [unite]
//   simbridge_cli set-var L:AZP_RAF_EFB_ON number 1
//   simbridge_cli inputs [filtre]              liste les commandes de cockpit de l'avion chargé
//   simbridge_cli get-input NOM
//   simbridge_cli set-input NOM valeur
//   simbridge_cli code "(>H:AZP_RAF_ALARMS_ACKNOWLEDGE)"
//   simbridge_cli dump [filtre]                valeur actuelle de tous les Input Events (decouverte des commandes)
//   simbridge_cli lvars [filtre]               liste les variables L: de l'avion (module MobiFlight WASM)
//   simbridge_cli watchl [secondes] [filtre]   comme watch, pour les variables L: (valeur numerique)
//   simbridge_cli watchs [secondes]            comme watch, pour une liste de SimVars standard (verriere, APU, feux, train, volets...)
//   simbridge_cli watch [secondes] [filtre]    affiche chaque Input Event dont la valeur change : manipulez une commande dans le cockpit, son nom apparait
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "SimBridge.h"

using hands::win::SimBridge;

namespace {

// Filtre : mots separes par des virgules ; un mot commencant par « ! » exclut. Ex. « AZP_RAF,!AFTERBURNER,!SHAKE » : contient AZP_RAF et aucun des deux autres.
bool Matches(const std::string& name, const std::string& filter) {
    size_t pos = 0;
    bool anyInclude = false, included = false;
    while (pos <= filter.size()) {
        size_t e = filter.find(',', pos);
        if (e == std::string::npos) e = filter.size();
        std::string w = filter.substr(pos, e - pos);
        pos = e + 1;
        if (w.empty()) continue;
        if (w[0] == '!') {
            if (name.find(w.substr(1)) != std::string::npos) return false;
        } else {
            anyInclude = true;
            if (name.find(w) != std::string::npos) included = true;
        }
    }
    return !anyInclude || included;
}

std::mutex g_out;
void Say(const std::string& s) {
    std::lock_guard<std::mutex> l(g_out);
    std::fprintf(stderr, "%s\n", s.c_str());
}

int Usage() {
    std::fprintf(stderr,
                 "usage : simbridge_cli <commande> [arguments]\n"
                 "  status\n  event NOM [valeur]\n  get-var NOM [unite]\n  set-var NOM unite valeur\n  inputs [filtre]\n  get-input NOM\n"
                 "  set-input NOM valeur\n  code \"(>H:...)\"\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return Usage();
    const std::string cmd = argv[1];
    SimBridge bridge(Say);
    bridge.Start();

    // Attend la connexion (20 s) : MSFS doit être lancé.
    for (int i = 0; i < 200 && !bridge.Status().connected; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto st = bridge.Status();
    if (!st.connected) {
        std::fprintf(stderr, "Non connecte : %s\n", st.reason.c_str());
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // laisse arriver le nom de l'avion
    std::string err;
    int rc = 0;
    if (cmd == "status") {
        const auto s = bridge.Status();
        std::printf("connecte : %s\navion : %s\nMobiFlight : %s\n", s.simName.c_str(), s.aircraft.empty() ? "?" : s.aircraft.c_str(),
                    s.mobiFlight ? "oui" : "non (inconnu tant qu'aucun code n'est envoye)");
    } else if (cmd == "event" && argc >= 3) {
        if (!bridge.SendEvent(argv[2], argc >= 4 ? std::atoi(argv[3]) : 0, &err)) rc = 1;
        else std::this_thread::sleep_for(std::chrono::milliseconds(300));
    } else if (cmd == "get-var" && argc >= 3) {
        double v = 0;
        if (!bridge.ReadVar(argv[2], argc >= 4 ? argv[3] : "number", &v, 3000, &err)) rc = 1;
        else std::printf("%s = %g\n", argv[2], v);
    } else if (cmd == "set-var" && argc >= 5) {
        if (!bridge.SetVar(argv[2], argv[3], std::atof(argv[4]), &err)) rc = 1;
        else std::this_thread::sleep_for(std::chrono::milliseconds(300));
    } else if (cmd == "inputs") {
        std::vector<hands::win::InputEventInfo> list;
        if (!bridge.ListInputs(&list, true, 8000, &err)) {
            rc = 1;
        } else {
            const std::string filter = argc >= 3 ? argv[2] : "";
            int n = 0;
            for (const auto& i : list) {
                if (!filter.empty() && i.name.find(filter) == std::string::npos) continue;
                std::printf("%s%s\n", i.name.c_str(), i.isString ? "  (texte)" : "");
                ++n;
            }
            std::fprintf(stderr, "%d commande(s) sur %zu.\n", n, list.size());
        }
    } else if (cmd == "get-input" && argc >= 3) {
        double v = 0;
        if (!bridge.ReadInput(argv[2], &v, 3000, &err)) rc = 1;
        else std::printf("%s = %g\n", argv[2], v);
    } else if (cmd == "set-input" && argc >= 4) {
        if (!bridge.SetInput(argv[2], std::atof(argv[3]), &err)) rc = 1;
        else std::this_thread::sleep_for(std::chrono::milliseconds(300));
    } else if (cmd == "watchs") {
        static const char* const kVars[][2] = {
            {"CANOPY OPEN", "percent"}, {"EXIT OPEN:0", "percent"}, {"EXIT OPEN:1", "percent"}, {"EXIT OPEN:2", "percent"}, {"EXIT OPEN:3", "percent"},
            {"APU SWITCH", "bool"}, {"APU PCT RPM", "percent"}, {"APU GENERATOR SWITCH:1", "bool"}, {"GEAR HANDLE POSITION", "bool"}, {"BRAKE PARKING POSITION", "bool"},
            {"LIGHT NAV", "bool"}, {"LIGHT BEACON", "bool"}, {"LIGHT STROBE", "bool"}, {"LIGHT LANDING", "bool"}, {"LIGHT TAXI", "bool"}, {"LIGHT PANEL", "bool"},
            {"LIGHT CABIN", "bool"}, {"ELECTRICAL MASTER BATTERY", "bool"}, {"TAILHOOK HANDLE", "bool"}, {"SPOILERS HANDLE POSITION", "percent"}, {"FLAPS HANDLE INDEX", "number"},
            {"AUTOPILOT MASTER", "bool"}, {"GENERAL ENG COMBUSTION:1", "bool"}, {"GENERAL ENG COMBUSTION:2", "bool"}, {"FUEL TANK SELECTOR:1", "number"},
            {"PITOT HEAT SWITCH:1", "bool"}, {"STRUCTURAL DEICE SWITCH", "bool"}, {"EXTERNAL POWER AVAILABLE:1", "bool"}, {"EXTERNAL POWER ON:1", "bool"}};
        const int secs = argc >= 3 ? std::atoi(argv[2]) : 0;
        std::map<std::string, double> last;
        const auto t0 = std::chrono::steady_clock::now();
        bool first = true;
        for (;;) {
            for (const auto& v : kVars) {
                double val = 0;
                if (!bridge.ReadVar(v[0], v[1], &val, 800, &err)) continue;
                const auto it = last.find(v[0]);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (!first && it != last.end() && it->second != val) std::printf("%7.1f s  %s : %g -> %g\n", ms / 1000.0, v[0], it->second, val);
                last[v[0]] = val;
            }
            std::fflush(stdout);
            if (secs > 0 && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(secs)) break;
            first = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));  // un relevé ne doit pas saturer le simulateur
        }
    } else if (cmd == "lvars" || cmd == "watchl") {
        std::vector<std::string> names;
        if (!bridge.ListLVars(&names, 15000, &err)) {
            rc = 1;
        } else {
            const bool watch = cmd == "watchl";
            const int secs = watch && argc >= 3 ? std::atoi(argv[2]) : 0;
            const std::string filter = argc >= (watch ? 4 : 3) ? argv[watch ? 3 : 2] : "";
            if (!watch) {
                int n = 0;
                for (const auto& nm : names)
                    if (Matches(nm, filter)) { std::printf("L:%s\n", nm.c_str()); ++n; }
                std::fprintf(stderr, "%d variable(s) sur %zu.\n", n, names.size());
            } else {
                std::map<std::string, double> last;
                const auto t0 = std::chrono::steady_clock::now();
                bool first = true;
                for (;;) {
                    for (const auto& nm : names) {
                        if (!Matches(nm, filter)) continue;
                        double v = 0;
                        if (!bridge.ReadVar("L:" + nm, "number", &v, 800, &err)) continue;
                        const auto it = last.find(nm);
                        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                        if (!first && it != last.end() && it->second != v) std::printf("%7.1f s  L:%s : %g -> %g\n", ms / 1000.0, nm.c_str(), it->second, v);
                        last[nm] = v;
                    }
                    std::fflush(stdout);
                    if (secs > 0 && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(secs)) break;
                    first = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));  // un relevé ne doit pas saturer le simulateur
                }
            }
        }
    } else if (cmd == "dump" || cmd == "watch") {
        std::vector<hands::win::InputEventInfo> list;
        if (!bridge.ListInputs(&list, true, 8000, &err)) {
            rc = 1;
        } else {
            const bool watch = cmd == "watch";
            const int secs = watch && argc >= 3 ? std::atoi(argv[2]) : 0;
            const std::string filter = argc >= (watch ? 4 : 3) ? argv[watch ? 3 : 2] : "";
            std::map<std::string, double> last;
            const auto t0 = std::chrono::steady_clock::now();
            bool first = true;
            for (;;) {
                for (const auto& i : list) {
                    if (i.isString || !Matches(i.name, filter)) continue;
                    double v = 0;
                    if (!bridge.ReadInput(i.name, &v, 1500, &err)) continue;
                    const auto it = last.find(i.name);
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    if (!watch) std::printf("%s = %g\n", i.name.c_str(), v);
                    else if (!first && it != last.end() && it->second != v) std::printf("%7.1f s  %s : %g -> %g\n", ms / 1000.0, i.name.c_str(), it->second, v);
                    last[i.name] = v;
                }
                std::fflush(stdout);
                if (!watch || (secs > 0 && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(secs))) break;
                first = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));  // un relevé ne doit pas saturer le simulateur
            }
        }
    } else if (cmd == "code" && argc >= 3) {
        if (!bridge.ExecCode(argv[2], &err)) rc = 1;
        else std::this_thread::sleep_for(std::chrono::milliseconds(800));  // laisse partir la file MobiFlight (35 ms par commande)
    } else {
        return Usage();
    }
    if (rc != 0) std::fprintf(stderr, "Erreur : %s\n", err.c_str());
    bridge.Stop();
    return rc;
}
