// JoyPoll.h — lecture des boutons de joystick / HOTAS (DirectInput, arrière-plan, sans exclusivité : le jeu garde ses assignations).
// Numérotation des boutons comme dans joy.cpl : à partir de 1 (jusqu'à 128). Windows seulement ; sert à la couche et à la tray.
#pragma once

#include <string>
#include <vector>

namespace hands::win {

struct JoyEvent {
    std::string device;  // nom du périphérique
    int button = 0;      // 1..128
};

class JoyPoll {
public:
    JoyPoll();
    ~JoyPoll();
    JoyPoll(const JoyPoll&) = delete;
    JoyPoll& operator=(const JoyPoll&) = delete;

    // Cherche un front montant (bouton qui vient d'être enfoncé) depuis le dernier appel. `deviceFilter` : morceau du nom du périphérique, sans
    // tenir compte des majuscules ("" = tous) ; `buttonFilter` : numéro voulu (0 = n'importe lequel). Retourne vrai et remplit `ev` si trouvé.
    // Un périphérique branché après coup est repris dans les 10 secondes.
    bool PollPressed(const std::string& deviceFilter, int buttonFilter, JoyEvent* ev);

    // Tous les fronts montants depuis le dernier appel (à utiliser quand plusieurs boutons sont surveillés : chaque appel consomme les fronts).
    std::vector<JoyEvent> PollAllPressed(const std::string& deviceFilter);

    // Noms des périphériques actuellement lus.
    std::vector<std::string> Devices();

    // Libère tout (sans attendre le destructeur).
    void Close();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace hands::win
