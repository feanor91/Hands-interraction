// Proposed repo location: tests/test_servo_controller.cpp (add to core_tests in tests/CMakeLists.txt).
// Controller vs simulated plant: integrator driven by integer counts, input latency L, readback latency r, measurement noise, dropouts, optional wall.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "TestFramework.h"
#include "servo/ServoController.h"

using namespace hands::servo;

namespace {

struct Plant {
    int L = 2, r = 1;
    double gx = 1.15 / 1193.7, gy = -1.15 / 1193.7;  // TRUE gain: 15 % above the controller's prior
    double sigma = 0.0006;                           // detection noise (rad) ~ 0.7 px
    double drop = 0.03;                              // share of failed detections
    bool wall = false;
    double wallYaw = 0.4;
    bool measure = true;
    uint64_t rng = 88172645463325252ull;
    std::vector<int> cx, cy;
    std::vector<V2> pos;
    V2 cur{0.2, -0.1};  // where the pointer happens to be (unknown to the controller)

    double U() {  // xorshift64* -> [0,1)
        rng ^= rng >> 12;
        rng ^= rng << 25;
        rng ^= rng >> 27;
        return static_cast<double>((rng * 2685821657736338717ull) >> 11) / 9007199254740992.0;
    }
    double G() {  // ~N(0,1): sum of 12 uniforms - 6
        double s = 0;
        for (int i = 0; i < 12; ++i) s += U();
        return s - 6.0;
    }
};

struct Result {
    std::vector<double> err;  // |true pointer - target| (rad) per call
    std::vector<int> dx;      // counts sent per call (x axis)
    int saturatedFlags = 0;
    long long sumAbsDx = 0;
};

using TargetFn = std::function<V2(int)>;

template <class Step>
Result Run(Plant& pl, int n, const TargetFn& target, Step&& step) {
    Result res;
    pl.cx.assign(static_cast<size_t>(n) + 4, 0);
    pl.cy.assign(static_cast<size_t>(n) + 4, 0);
    pl.pos.assign(static_cast<size_t>(n) + 4, V2{});
    res.err.assign(static_cast<size_t>(n), 0.0);
    res.dx.assign(static_cast<size_t>(n), 0);
    for (int k = 0; k < n; ++k) {
        if (k - pl.L >= 0) {
            pl.cur.a += pl.gx * pl.cx[static_cast<size_t>(k - pl.L)];
            pl.cur.b += pl.gy * pl.cy[static_cast<size_t>(k - pl.L)];
            if (pl.wall) pl.cur.a = std::min(pl.cur.a, pl.wallYaw);
        }
        pl.pos[static_cast<size_t>(k)] = pl.cur;
        const int m = k - pl.r;
        V2 meas{};
        bool have = false;
        if (pl.measure && m >= 0 && pl.U() > pl.drop) {
            const V2 t = pl.pos[static_cast<size_t>(m)];
            meas = {t.a + pl.sigma * pl.G(), t.b + pl.sigma * pl.G()};
            have = true;
        }
        const V2 tg = target(k);
        int dx = 0, dy = 0;
        bool sat = false;
        step(static_cast<uint64_t>(k), have, static_cast<uint64_t>(m < 0 ? 0 : m), meas, tg, &dx, &dy, &sat);
        pl.cx[static_cast<size_t>(k)] = dx;
        pl.cy[static_cast<size_t>(k)] = dy;
        res.sumAbsDx += std::abs(dx);
        res.dx[static_cast<size_t>(k)] = dx;
        res.saturatedFlags += sat ? 1 : 0;
        res.err[static_cast<size_t>(k)] = Norm(pl.pos[static_cast<size_t>(k)] - tg);
    }
    return res;
}

double MaxErr(const Result& r, int from, int to) {
    double m = 0;
    for (int i = from; i < to && i < static_cast<int>(r.err.size()); ++i) m = std::max(m, r.err[static_cast<size_t>(i)]);
    return m;
}

Result RunServo(Plant& pl, ServoController& c, int n, const TargetFn& target) {
    return Run(pl, n, target, [&](uint64_t seq, bool have, uint64_t m, V2 meas, V2 tg, int* dx, int* dy, bool* sat) {
        if (have) c.OnMeasurement(m, meas);
        const ServoController::Out o = c.Step(seq, true, tg);
        *dx = o.dx;
        *dy = o.dy;
        *sat = o.saturated;
    });
}

const double kPx = 1.0 / 1215.0;  // rad per image pixel near the optical axis (2688 px over tan range 2.21)

double MinJerk(double s) { return s <= 0 ? 0 : (s >= 1 ? 1 : 10 * s * s * s - 15 * s * s * s * s + 6 * s * s * s * s * s); }

}  // namespace

TEST(Servo_Controller_repos_converge) {
    Plant pl;
    ServoController c;
    const Result r = RunServo(pl, c, 160, [](int) { return V2{0.12, 0.05}; });
    std::printf("      repos : erreur max frames 60..160 = %.2f px, finale %.2f px\n", MaxErr(r, 60, 160) / kPx, r.err.back() / kPx);
    CHECK(MaxErr(r, 60, 160) < 4.0 * kPx);  // jitter of detection + integer counts
    CHECK(c.HaveModel());
}

TEST(Servo_Controller_aucune_commande_sans_modele) {
    ServoController c;
    for (uint64_t s = 0; s < 5; ++s) {
        const auto o = c.Step(s, true, {0.1, 0.1});
        CHECK(o.dx == 0 && o.dy == 0 && !o.haveModel);
    }
}

TEST(Servo_Controller_robuste_a_un_L_faux_et_a_un_gain_faux) {
    for (int dL : {-1, 0, 1}) {
        for (double gk : {0.8, 1.0, 1.25}) {
            Plant pl;
            ServoParams p;
            p.latencyFrames = pl.L + dL;
            p.radPerCountX *= gk;
            p.radPerCountY *= gk;
            ServoController c(p);
            const Result r = RunServo(pl, c, 400, [](int n) {  // three moves of ~0.15 rad (180 px) in 13 frames, rests in between
                const double a = n < 100 ? 0.0 : MinJerk((n - 100) / 13.0), b = n < 200 ? 0.0 : MinJerk((n - 200) / 13.0), d = n < 300 ? 0.0 : MinJerk((n - 300) / 13.0);
                return V2{0.1 + 0.15 * a - 0.12 * b, -0.05 + 0.10 * d - 0.1 * a};
            });
            std::printf("      L%+d gain x%.2f : repos final %.2f px, pire erreur %.0f px\n", dL, gk, MaxErr(r, 370, 400) / kPx, MaxErr(r, 60, 400) / kPx);
            CHECK(MaxErr(r, 370, 400) < 8.0 * kPx);  // converges whatever the mismatch
            CHECK(MaxErr(r, 20, 400) < 0.6);         // and never diverges
        }
    }
}

TEST(Servo_Controller_la_boucle_naive_a_gain_1_diverge_avec_ce_retard) {
    // Documents WHY the controller is an observer : the same plant (L=2, r=1 -> d=3 frames) with c = (target - last measured pointer)/g, gain 1.
    Plant pl;
    V2 last{};
    bool have = false;
    const double g = 1.0 / 1193.7;
    const Result r = Run(pl, 120, [](int) { return V2{0.12, 0.05}; }, [&](uint64_t, bool h, uint64_t, V2 meas, V2 tg, int* dx, int* dy, bool* sat) {
        if (h) {
            last = meas;
            have = true;
        }
        *sat = false;
        *dx = *dy = 0;
        if (!have) return;
        *dx = std::max(-150, std::min(150, static_cast<int>(std::lround((tg.a - last.a) / g))));
        *dy = std::max(-150, std::min(150, static_cast<int>(std::lround((tg.b - last.b) / -g))));
    });
    std::printf("      naive K=1 : erreur max frames 60..120 = %.0f px (bornee par le plafond de 150 comptes : elle orbite)\n", MaxErr(r, 60, 120) / kPx);
    CHECK(MaxErr(r, 60, 120) > 40.0 * kPx);  // never settles
}

TEST(Servo_Controller_poursuite_et_anticipation) {
    auto move = [](int n) {  // 0.15 rad (~180 px) in 13 frames (0.45 s at 29 fps) starting at frame 60
        return V2{0.0 + 0.15 * MinJerk((n - 60) / 13.0), 0.0};
    };
    Plant pl;
    ServoController c;
    const Result with = RunServo(pl, c, 140, move);
    Plant pl2;
    ServoParams p;
    p.lead = 0.0;
    ServoController c2(p);
    const Result without = RunServo(pl2, c2, 140, move);
    std::printf("      poursuite : pire erreur pendant le geste %.0f px avec anticipation, %.0f px sans ; repos 25 images apres %.1f px\n", MaxErr(with, 60, 80) / kPx,
                MaxErr(without, 60, 80) / kPx, MaxErr(with, 100, 140) / kPx);
    CHECK(MaxErr(with, 60, 80) < MaxErr(without, 60, 80));  // feed-forward helps
    CHECK(MaxErr(with, 100, 140) < 4.0 * kPx);
}

TEST(Servo_Controller_mur_pas_d_emballement) {
    Plant pl;
    pl.wall = true;
    pl.wallYaw = 0.4;
    ServoController c;
    // Target beyond the wall for 150 frames, then back inside.
    const Result r = RunServo(pl, c, 230, [](int n) { return n < 150 ? V2{0.7, 0.0} : V2{0.2, 0.0}; });
    long long pushed = 0;
    for (int k = 100; k < 150; ++k) pushed += std::abs(r.dx[static_cast<size_t>(k)]);
    std::printf("      mur : indicateurs 'sature' = %d ; somme |dx| frames 100..150 = %lld comptes ; erreur 30 images apres le retour = %.1f px\n", r.saturatedFlags, pushed,
                MaxErr(r, 200, 230) / kPx);
    CHECK(pushed < 600);  // without wall handling the controller keeps hammering the wall (150 counts/frame cap -> thousands)
    CHECK(r.saturatedFlags >= 1);
    CHECK(MaxErr(r, 200, 230) < 6.0 * kPx);  // comes back as soon as the target is reachable (no wind-up)
}

TEST(Servo_Controller_sans_mesure_le_modele_tient) {
    Plant pl;
    pl.gx = 1.0 / 1193.7;  // exact gain so that dead reckoning is meaningful
    pl.gy = -1.0 / 1193.7;
    ServoController c;
    // converge first, then the pointer is "lost" (no detections) for 60 frames while the target moves by 0.1 rad
    const Result r = Run(pl, 200, [](int n) { return n < 100 ? V2{0.0, 0.0} : V2{0.1, 0.0}; }, [&](uint64_t seq, bool have, uint64_t m, V2 meas, V2 tg, int* dx, int* dy, bool* sat) {
        if (have && seq < 100) c.OnMeasurement(m, meas);  // no more measurements after frame 100
        const auto o = c.Step(seq, true, tg);
        *dx = o.dx;
        *dy = o.dy;
        *sat = o.saturated;
    });
    std::printf("      sans mesure : erreur 60 images apres = %.1f px (dead reckoning, gain exact)\n", MaxErr(r, 170, 200) / kPx);
    CHECK(MaxErr(r, 170, 200) < 8.0 * kPx);
}

TEST(Servo_Controller_cible_perdue_aucune_commande_puis_reprise_douce) {
    ServoController c;
    Plant pl;
    const Result r = Run(pl, 160, [](int) { return V2{0.1, 0.0}; }, [&](uint64_t seq, bool have, uint64_t m, V2 meas, V2 tg, int* dx, int* dy, bool* sat) {
        if (have) c.OnMeasurement(m, meas);
        const bool valid = !(seq >= 80 && seq < 100);  // hand lost for 20 frames
        const auto o = c.Step(seq, valid, tg);
        if (!valid) CHECK(o.dx == 0 && o.dy == 0);
        CHECK(std::abs(o.dx) <= 150 && std::abs(o.dy) <= 150);
        CHECK(std::isfinite(o.error.a) && std::isfinite(o.predicted.a));
        *dx = o.dx;
        *dy = o.dy;
        *sat = o.saturated;
    });
    CHECK(MaxErr(r, 130, 160) < 4.0 * kPx);
}

TEST(Servo_Controller_trous_dans_les_appels) {
    // xrEndFrame calls without a projection layer are skipped by the caller: seq jumps, the controller keeps its history consistent.
    ServoController c;
    c.Step(10, true, {0, 0});
    c.OnMeasurement(10, {0.0, 0.0});
    const auto a = c.Step(14, true, {0.05, 0.0});
    CHECK(a.haveModel);
    const auto dup = c.Step(14, true, {0.05, 0.0});
    CHECK(dup.dx == 0 && dup.dy == 0);  // duplicate seq ignored
}

TEST(Servo_Controller_saut_du_pointeur_reancrage_rapide) {
    // MSFS re-ancre le pointeur dans le champ de vision apres un mouvement de tete : il saute de 0.15 rad (~180 px) sans qu'aucune commande ne l'explique.
    auto run = [&](bool reanchor) {
        Plant pl;
        ServoParams p;
        if (!reanchor) p.jumpStreak = 1000000;
        ServoController c(p);
        return Run(pl, 200, [](int) { return V2{0.12, 0.05}; }, [&](uint64_t seq, bool have, uint64_t m, V2 meas, V2 tg, int* dx, int* dy, bool* sat) {
            if (seq == 100) pl.cur.a += 0.15;  // le saut
            if (have) c.OnMeasurement(m, meas);
            const auto o = c.Step(seq, true, tg);
            *dx = o.dx;
            *dy = o.dy;
            *sat = o.saturated;
        });
    };
    const Result fast = run(true), slow = run(false);
    auto settle = [](const Result& r) {  // premiere image apres le saut a partir de laquelle l'erreur reste < 4 px
        for (int k = 100; k < 200; ++k) {
            bool ok = true;
            for (int j = k; j < 200 && ok; ++j) ok = r.err[static_cast<size_t>(j)] < 4.0 * kPx;
            if (ok) return k - 100;
        }
        return 100;
    };
    std::printf("      saut : retour a < 4 px en %d images avec re-ancrage, %d sans\n", settle(fast), settle(slow));
    CHECK(settle(fast) <= settle(slow));
    CHECK(settle(fast) < 40);
    CHECK(MaxErr(fast, 170, 200) < 4.0 * kPx);
}
