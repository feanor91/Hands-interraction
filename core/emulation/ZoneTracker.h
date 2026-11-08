// ZoneTracker.h — contact d'un bout de doigt avec une zone sphérique (pochoir) : déclenche une fois après un court maintien.
// Portable, testé. Distances en mètres, temps en millisecondes.
#pragma once

#include "emulation/PoseMath.h"

namespace hands::emulation {

class ZoneTracker {
public:
    // `tipValid` faux : doigt perdu (l'état de maintien est abandonné). Retourne vrai UNE fois quand le doigt est resté dans la zone
    // `dwellMs`. Il faut ensuite sortir de la zone (rayon x 1,5) pendant `rearmMs` pour pouvoir redéclencher.
    bool Update(Vec3 tip, bool tipValid, Vec3 center, float radius, double nowMs, double dwellMs, double rearmMs) {
        if (!tipValid) {
            insideSince_ = -1;
            outsideSince_ = nowMs;
            return false;
        }
        const float d = Length(tip - center);
        if (armed_) {
            if (d <= radius) {
                if (insideSince_ < 0) insideSince_ = nowMs;
                if (nowMs - insideSince_ >= dwellMs) {
                    armed_ = false;
                    insideSince_ = -1;
                    outsideSince_ = -1;
                    return true;
                }
            } else {
                insideSince_ = -1;
            }
        } else {
            if (d > radius * 1.5f) {
                if (outsideSince_ < 0) outsideSince_ = nowMs;
                if (nowMs - outsideSince_ >= rearmMs) armed_ = true;
            } else {
                outsideSince_ = -1;
            }
        }
        return false;
    }
    void Reset() { *this = ZoneTracker(); }
    // Desarme sans declencher : le doigt doit d'abord sortir de la zone (apres un apprentissage, le doigt s'y trouve deja).
    void Disarm() {
        armed_ = false;
        insideSince_ = -1;
        outsideSince_ = -1;
    }
    bool Armed() const { return armed_; }

private:
    bool armed_ = true;
    double insideSince_ = -1, outsideSince_ = -1;
};

}  // namespace hands::emulation
