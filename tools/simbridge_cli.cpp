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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "SimBridge.h"

using hands::win::SimBridge;

namespace {

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
