// Clap.h — detection d'un « applaudissement » : les deux mains se frappent l'une contre l'autre (bascule du mode souris, sans menu).
//
// Entree : a chaque image, la distance entre les deux paumes (metres) quand les DEUX mains sont suivies, sinon `valid = false`.
// Un applaudissement = les mains etaient ECARTEES (>= kApartM) dans les ~0.7 s qui precedent, elles se rapprochent VITE (>= kClosingMps sur ~120 ms)
// et finissent COLLEES (<= kTouchM). Quand le suivi perd une main au moment du choc (les mains se cachent l'une l'autre), le dernier etat valide proche compte
// si la perte survient dans les 250 ms. Delai de repos kCooldownMs entre deux applaudissements, et il faut avoir ecarte les mains (>= kRearmM) pour le suivant.
#pragma once

#include <cmath>
#include <cstdint>

namespace hands::emulation {

class ClapDetector {
public:
    static constexpr double kApartM = 0.26;      // distance a avoir atteinte avant le choc
    static constexpr double kTouchM = 0.11;      // distance paume-paume au contact (paumes a plat ~4-8 cm, bouts des doigts ~10 cm)
    static constexpr double kClosingMps = 0.9;   // vitesse de rapprochement minimale
    static constexpr double kWindowMs = 700.0;   // memoire de la distance maximale
    static constexpr double kSpeedSpanMs = 120.0;
    static constexpr double kLostMs = 250.0;     // perte d'une main apres un rapprochement : compte comme choc
    static constexpr double kCooldownMs = 1500.0;
    static constexpr double kRearmM = 0.20;

    // Retourne vrai UNE fois, a l'image du choc.
    bool Update(uint64_t nowMs, bool valid, double distM) {
        const double t = static_cast<double>(nowMs);
        if (!valid) {
            bool fire = false;
            if (haveLast_ && t - lastT_ <= kLostMs && lastDist_ <= kLostNearM && armed_ && t - firedT_ >= kCooldownMs && Candidate(lastT_, lastDist_)) {
                fire = true;
                Fire(t);
            }
            haveLast_ = false;
            return fire;
        }
        Push(t, distM);
        haveLast_ = true;
        lastT_ = t;
        lastDist_ = distM;
        if (distM >= kRearmM) armed_ = true;
        if (!armed_ || t - firedT_ < kCooldownMs || distM > kTouchM) return false;
        if (!Candidate(t, distM)) return false;
        Fire(t);
        return true;
    }

    void Reset() { n_ = 0; haveLast_ = false; armed_ = true; firedT_ = -1e9; }

private:
    static constexpr double kLostNearM = 0.17;
    static constexpr int kN = 64;
    double ts_[kN] = {}, ds_[kN] = {};
    int n_ = 0, head_ = 0;
    bool haveLast_ = false, armed_ = true;
    double lastT_ = 0, lastDist_ = 0, firedT_ = -1e9;

    void Push(double t, double d) {
        ts_[head_] = t;
        ds_[head_] = d;
        head_ = (head_ + 1) % kN;
        if (n_ < kN) ++n_;
    }
    void Fire(double t) {
        firedT_ = t;
        armed_ = false;
    }
    // Les mains etaient ecartees, et le rapprochement jusqu'a (t, d) etait rapide.
    bool Candidate(double t, double d) const {
        double maxD = 0, farT = 0, maxClose = 0;
        for (int i = 0; i < n_; ++i) {
            const int k = (head_ - 1 - i + 2 * kN) % kN;
            if (t - ts_[k] > kWindowMs) break;
            if (ts_[k] > t) continue;
            if (ds_[k] > maxD) {
                maxD = ds_[k];
                farT = ts_[k];
            }
        }
        if (maxD < kApartM) return false;
        // vitesse de rapprochement : meilleure pente d(t-dt) - d(t) sur une fenetre de ~kSpeedSpanMs
        for (int i = 0; i < n_; ++i) {
            const int k = (head_ - 1 - i + 2 * kN) % kN;
            const double dt = t - ts_[k];
            if (dt > kWindowMs) break;
            if (dt < 25.0 || dt > kSpeedSpanMs * 2.0) continue;
            const double sp = (ds_[k] - d) / (dt / 1000.0);
            if (sp > maxClose) maxClose = sp;
        }
        (void)farT;
        return maxClose >= kClosingMps;
    }
};

}  // namespace hands::emulation
