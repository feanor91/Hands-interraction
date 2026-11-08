// HandPresence.h — « la main est-elle suivie ? » avec un maintien : le suivi des mains clignote quand une main sort un instant du champ des
// caméras. Pour masquer les manettes virtuelles issues des mains sans scintillement, on garde l'état « présente » pendant `holdMs` après le dernier
// suivi actif. Portable, testé. Temps en millisecondes.
#pragma once

namespace hands::emulation {

class HandPresence {
public:
    // `active` : le runtime déclare la main suivie à cet instant. Retourne l'état filtré : vrai dès que la main est suivie, faux seulement
    // après `holdMs` sans suivi.
    bool Update(bool active, double nowMs, double holdMs) {
        if (active) {
            lastActive_ = nowMs;
            present_ = true;
        } else if (present_ && nowMs - lastActive_ > holdMs) {
            present_ = false;
        }
        return present_;
    }
    bool Present() const { return present_; }
    void Reset() { *this = HandPresence(); }

private:
    bool present_ = false;
    double lastActive_ = 0;
};

}  // namespace hands::emulation
