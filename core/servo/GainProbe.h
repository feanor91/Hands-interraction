// GainProbe.h — identification du pointeur de MSFS au demarrage du mode souris : de combien tourne-t-il (radians) pour un compte de souris, et combien
// d'images separent l'envoi d'une commande de son effet dans l'image du jeu ? (portable, sans Windows / OpenXR / D3D12)
//
// Principe : quatre salves de comptes connus (+X, -X, +Y, -Y : 4 images de 10 comptes), chacune suivie d'une attente ; la difference entre la position
// mesuree du pointeur (mediane) avant et apres, divisee par le nombre de comptes, donne le gain ; l'instant ou la mesure commence a bouger donne la latence.
// Si MSFS suit l'accelerateur de pointeur de Windows plutot que les comptes bruts, le gain depend de la taille des evenements : l'injecteur envoie par
// evenements de 8 comptes au plus (zone ou l'acceleration vaut ~1) et la salve est mesuree dans ces memes conditions.
//
// Utilisation : Start(seq) ; a chaque image : OnMeasurement(imageSeq, angles) pour chaque detection, puis Step(seq, &dx, &dy) ; quand Done() : Result().
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "servo/ServoController.h"  // V2

namespace hands::servo {

class GainProbe {
public:
    struct Result {
        bool ok = false;               // faux : on garde les valeurs a priori
        double radPerCountX = 0;       // dx > 0 -> yaw augmente (vers la droite)
        double radPerCountY = 0;       // dy > 0 (vers le bas) -> pitch diminue : valeur negative
        int latencyFrames = 2;
        int burstsOk = 0;              // salves exploitables (sur 4)
        bool sent = false;             // au moins une salve de comptes a vraiment ete envoyee (sinon l'echec ne dit rien sur la cible)
        bool responded = false;        // au moins une salve a fait bouger la cible dans le bon sens : faux = la cible ne repond pas aux comptes (eclat, decor...)
        double est[4] = {0, 0, 0, 0};  // gain mesure par salve (rad / compte, signe inclus), pour le journal
        int lat[4] = {0, 0, 0, 0};
        const char* why = "";          // raison de l'echec (journal)
    };

    static constexpr int kBurstFrames = 4;
    static constexpr int kPerFrame = 10;
    static constexpr int kSettleFrames = 18;
    static constexpr int kMaxWaitFrames = 240;  // attente d'un pointeur stable
    static constexpr double kMinGain = 2e-5;    // rad / compte : en dessous, la salve n'a rien deplace (~0.02 px par compte)

    void Start(uint64_t seq) {
        *this = GainProbe();
        started_ = true;
        startSeq_ = seq;
    }
    bool Active() const { return started_ && !done_; }
    bool Done() const { return done_; }
    const Result& Final() const { return res_; }
    int BurstIndex() const { return burst_; }

    void OnMeasurement(uint64_t imageSeq, V2 ptr) {
        if (!started_) return;
        m_.push_back({static_cast<int64_t>(imageSeq), ptr});
        if (m_.size() > 400) m_.erase(m_.begin(), m_.begin() + 100);
    }

    // Comptes a envoyer a l'appel `seq` (0, 0 hors salve). Retourne vrai tant que l'identification n'est pas terminee.
    bool Step(uint64_t seq, int* dx, int* dy) {
        *dx = *dy = 0;
        if (!started_ || done_) return false;
        const int64_t sq = static_cast<int64_t>(seq);
        if (phase_ == Phase::Wait) {
            if (Stable(sq)) {
                phase_ = Phase::Burst;
                burstStart_ = sq;
            } else if (sq - static_cast<int64_t>(startSeq_) > kMaxWaitFrames) {
                return Fail("pointeur introuvable ou instable");
            } else {
                return true;
            }
        }
        if (phase_ == Phase::Burst) {
            const int axis = burst_ / 2, sign = (burst_ % 2 == 0) ? 1 : -1;
            if (sq - burstStart_ < kBurstFrames) {
                (axis == 0 ? *dx : *dy) = sign * kPerFrame;
                sent_ = true;
                return true;
            }
            phase_ = Phase::Settle;
            settleStart_ = sq;
        }
        if (phase_ == Phase::Settle) {
            if (sq - burstStart_ < kBurstFrames + kSettleFrames) return true;
            Evaluate(burstStart_);
            ++burst_;
            if (burst_ >= 4) return Finish();
            phase_ = Phase::Wait;  // on laisse le pointeur se stabiliser avant la salve suivante
            startSeq_ = seq;
            // Stable() est reevalue a l'appel suivant
            return true;
        }
        return true;
    }

private:
    enum class Phase { Wait, Burst, Settle };
    struct M {
        int64_t seq;
        V2 p;
    };
    struct Est {
        bool valid = false;
        double gain = 0;  // rad / compte (signe inclus : deplacement mesure / comptes envoyes)
        int latency = 2;
    };

    // Les 4 dernieres mesures (images deja assez anciennes pour ne plus contenir une salve precedente) se tiennent dans 2 mrad.
    bool Stable(int64_t sq) const {
        std::vector<V2> v;
        for (auto it = m_.rbegin(); it != m_.rend() && v.size() < 4; ++it)
            if (it->seq <= sq && it->seq >= sq - 12) v.push_back(it->p);
        if (v.size() < 4) return false;
        double a0 = 1e9, a1 = -1e9, b0 = 1e9, b1 = -1e9;
        for (const V2& p : v) {
            a0 = std::min(a0, p.a);
            a1 = std::max(a1, p.a);
            b0 = std::min(b0, p.b);
            b1 = std::max(b1, p.b);
        }
        return (a1 - a0) < 0.002 && (b1 - b0) < 0.002;
    }

    static double Median(std::vector<double> v) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

    void Evaluate(int64_t start) {
        const int axis = burst_ / 2, sign = (burst_ % 2 == 0) ? 1 : -1;
        std::vector<double> before, after;
        std::vector<M> series;
        const int64_t endBurst = start + kBurstFrames;
        for (const M& m : m_) {
            const double v = axis == 0 ? m.p.a : m.p.b;
            if (m.seq <= start && m.seq >= start - 10) before.push_back(v);
            if (m.seq >= endBurst + 8 && m.seq <= start + kBurstFrames + kSettleFrames) after.push_back(v);
            if (m.seq > start && m.seq <= start + kBurstFrames + kSettleFrames) series.push_back(m);
        }
        Est e;
        if (before.size() >= 2 && after.size() >= 2) {
            const double b = Median(before), a = Median(after);
            const double counts = static_cast<double>(sign * kPerFrame * kBurstFrames);
            e.gain = (a - b) / counts;
            e.valid = true;
            // latence : premiere mesure qui a parcouru 15 % du deplacement final (la premiere image de salve en apporte 25 %)
            const double thr = std::max(0.15 * std::fabs(a - b), 0.0012);
            for (const M& m : series) {
                const double v = axis == 0 ? m.p.a : m.p.b;
                if (std::fabs(v - b) >= thr) {
                    e.latency = static_cast<int>(m.seq - start);
                    break;
                }
            }
        }
        est_[burst_] = e;
    }

    bool Fail(const char* why) {
        const bool responded = res_.responded;
        double est[4];
        int lat[4];
        for (int i = 0; i < 4; ++i) {
            est[i] = res_.est[i];
            lat[i] = res_.lat[i];
        }
        res_ = Result();
        res_.sent = sent_;
        res_.responded = responded;
        for (int i = 0; i < 4; ++i) {
            res_.est[i] = est[i];
            res_.lat[i] = lat[i];
        }
        res_.why = why;
        done_ = true;
        return false;
    }

    bool Finish() {
        // Gain par axe. Une butee de MSFS ne peut que REDUIRE le deplacement d'une salve : si les deux sens ne concordent pas (rapport hors 0.6..1.67), on garde celui
        // qui a le plus bouge. Une salve qui va dans le mauvais sens est du bruit (ou un decor qui bouge avec la tete) : ignoree. Aucune salve valide sur un axe : on
        // y reprend l'echelle de l'autre (pixels carres : un compte vaut le meme angle en x et en y).
        const double sgn[2] = {1.0, -1.0};  // dx > 0 -> yaw augmente ; dy > 0 (vers le bas) -> pitch diminue
        double g[2] = {0, 0};
        bool known[2] = {false, false};
        std::vector<double> lat;
        int nOk = 0;
        for (int b = 0; b < 4; ++b) {
            res_.est[b] = est_[b].valid ? est_[b].gain : 0.0;
            res_.lat[b] = est_[b].latency;
        }
        for (int axis = 0; axis < 2; ++axis) {
            double v[2];
            int n = 0;
            for (int k = 0; k < 2; ++k) {
                const Est& e = est_[axis * 2 + k];
                const double signed_ = e.gain * sgn[axis];  // positif = bon sens
                if (e.valid && signed_ > kMinGain) {
                    v[n++] = signed_;
                    lat.push_back(e.latency);
                    ++nOk;
                }
            }
            if (n == 2) {
                const double r = v[0] / v[1];
                g[axis] = (r >= 0.6 && r <= 1.67) ? 0.5 * (v[0] + v[1]) : std::max(v[0], v[1]);
                known[axis] = true;
            } else if (n == 1) {
                g[axis] = v[0];
                known[axis] = true;
            }
        }
        res_.sent = sent_;
        res_.burstsOk = nOk;
        res_.responded = nOk > 0;
        if (!known[0] && !known[1]) return Fail("la cible ne repond pas aux comptes de souris");
        if (!known[0]) g[0] = g[1];
        if (!known[1]) g[1] = g[0];
        const double prior = ServoParams().radPerCountX;
        if (g[0] < 0.1 * prior || g[0] > 10 * prior || g[1] < 0.1 * prior || g[1] > 10 * prior) return Fail("gain hors de la plage plausible");
        res_.ok = true;
        res_.radPerCountX = g[0];
        res_.radPerCountY = -g[1];
        std::sort(lat.begin(), lat.end());
        res_.latencyFrames = std::max(1, std::min(4, static_cast<int>(lat.empty() ? 2 : lat[lat.size() / 2])));
        done_ = true;
        return false;
    }

    bool started_ = false, done_ = false, sent_ = false;
    uint64_t startSeq_ = 0;
    int64_t burstStart_ = 0, settleStart_ = 0;
    int burst_ = 0;
    Phase phase_ = Phase::Wait;
    std::vector<M> m_;
    Est est_[4];
    Result res_;
};

}  // namespace hands::servo
