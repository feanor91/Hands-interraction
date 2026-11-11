#include <cmath>
#include <cstdio>
#include <vector>

#include "TestFramework.h"
#include "servo/GainProbe.h"

using namespace hands::servo;

namespace {
struct ProbePlant {
    int L = 2, r = 1;
    double gx = 0.8 / 1193.7, gy = -0.8 / 1193.7;
    double sigma = 0.0006, drop = 0.03;
    double wallMaxYaw = 1e9;  // butee a droite
    bool frozen = false;      // le pointeur ne bouge pas du tout
    uint64_t rng = 88172645463325252ull;
    double U() {
        rng ^= rng >> 12;
        rng ^= rng << 25;
        rng ^= rng >> 27;
        return static_cast<double>((rng * 2685821657736338717ull) >> 11) / 9007199254740992.0;
    }
    double G() {
        double s = 0;
        for (int i = 0; i < 12; ++i) s += U();
        return s - 6.0;
    }
};

GainProbe::Result RunProbe(ProbePlant& pl, uint64_t startSeq = 5) {
    GainProbe probe;
    probe.Start(startSeq);
    std::vector<int> cx(2000, 0), cy(2000, 0);
    std::vector<V2> pos(2000);
    V2 cur{0.1, -0.05};
    for (uint64_t k = 0; k < 1500; ++k) {
        if (k >= static_cast<uint64_t>(pl.L) && !pl.frozen) {
            cur.a += pl.gx * cx[k - static_cast<uint64_t>(pl.L)];
            cur.b += pl.gy * cy[k - static_cast<uint64_t>(pl.L)];
            cur.a = std::min(cur.a, pl.wallMaxYaw);
        }
        pos[k] = cur;
        if (k >= static_cast<uint64_t>(pl.r) && k >= startSeq && pl.U() > pl.drop) {
            const V2 t = pos[k - static_cast<uint64_t>(pl.r)];
            probe.OnMeasurement(k - static_cast<uint64_t>(pl.r), {t.a + pl.sigma * pl.G(), t.b + pl.sigma * pl.G()});
        }
        int dx = 0, dy = 0;
        const bool going = k >= startSeq ? probe.Step(k, &dx, &dy) : true;
        cx[k] = dx;
        cy[k] = dy;
        if (!going && probe.Done()) break;
    }
    return probe.Final();
}
}  // namespace

TEST(GainProbe_mesure_le_gain_et_la_latence) {
    for (int L : {1, 2, 3}) {
        for (double ratio : {0.5, 0.8, 1.0, 1.6}) {
            ProbePlant pl;
            pl.L = L;
            pl.gx = ratio / 1193.7;
            pl.gy = -ratio / 1193.7;
            const auto r = RunProbe(pl);
            std::printf("      L=%d gain x%.2f -> ok=%d gain x %.3f / y %.3f, latence %d, salves %d %s\n", L, ratio, r.ok, r.radPerCountX * 1193.7, -r.radPerCountY * 1193.7,
                        r.latencyFrames, r.burstsOk, r.why);
            CHECK(r.ok);
            CHECK(std::fabs(r.radPerCountX * 1193.7 / ratio - 1.0) < 0.08);
            CHECK(std::fabs(-r.radPerCountY * 1193.7 / ratio - 1.0) < 0.08);
            CHECK(std::abs(r.latencyFrames - L) <= 1);
        }
    }
}

TEST(GainProbe_butee_a_droite_le_gain_vient_de_l_autre_sens) {
    ProbePlant pl;
    pl.wallMaxYaw = 0.1;  // le pointeur est deja contre la butee a droite : la salve +X ne bouge pas
    const auto r = RunProbe(pl);
    std::printf("      butee : ok=%d gain x %.3f %s\n", r.ok, r.radPerCountX * 1193.7, r.why);
    CHECK(r.ok);
    CHECK(std::fabs(r.radPerCountX * 1193.7 / 0.8 - 1.0) < 0.1);
}

TEST(GainProbe_pointeur_immobile_echec_propre) {
    ProbePlant pl;
    pl.frozen = true;
    const auto r = RunProbe(pl);
    CHECK(!r.ok);
    CHECK(r.why[0] != 0);
}

TEST(GainProbe_sans_mesure_echec_apres_delai) {
    GainProbe p;
    p.Start(0);
    int dx = 0, dy = 0;
    bool going = true;
    for (uint64_t k = 0; k < 400 && going; ++k) going = p.Step(k, &dx, &dy);
    CHECK(!going);
    CHECK(p.Done() && !p.Final().ok);
}
