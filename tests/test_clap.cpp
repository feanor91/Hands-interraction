#include <algorithm>
#include "TestFramework.h"
#include "emulation/Clap.h"

using hands::emulation::ClapDetector;

namespace {
// distance(t) piecewise-lineaire : ecartees a 0.40 m, choc a la 600e ms, a 30 images/s
struct Run {
    ClapDetector c;
    int fires = 0;
    uint64_t t = 1000;
    void Feed(double d, bool valid = true) {
        if (c.Update(t, valid, d)) ++fires;
        t += 33;
    }
    void Apart(int ms, double d = 0.40) { for (int i = 0; i < ms / 33; ++i) Feed(d); }
    void Close(double from, double to, int ms) {
        const int n = std::max(1, ms / 33);
        for (int i = 1; i <= n; ++i) Feed(from + (to - from) * i / n);
    }
};
}  // namespace

TEST(Clap_applaudissement_franc) {
    Run r;
    r.Apart(500);
    r.Close(0.40, 0.06, 200);  // 1.7 m/s
    CHECK_EQ(r.fires, 1);
    r.Close(0.06, 0.40, 300);
    r.Apart(300);
    CHECK_EQ(r.fires, 1);  // un seul declenchement
}

TEST(Clap_mains_qui_se_rejoignent_doucement_ne_comptent_pas) {
    Run r;
    r.Apart(500);
    r.Close(0.40, 0.06, 1500);  // 0.23 m/s : geste lent (saisie a deux mains)
    CHECK_EQ(r.fires, 0);
}

TEST(Clap_mains_deja_proches_ne_comptent_pas) {
    Run r;
    r.Apart(800, 0.15);
    r.Close(0.15, 0.05, 100);
    CHECK_EQ(r.fires, 0);  // jamais ecartees
}

TEST(Clap_perte_de_suivi_au_choc) {
    Run r;
    r.Apart(500);
    r.Close(0.40, 0.13, 150);  // dernier echantillon valide a 13 cm
    r.Feed(0, false);          // les mains se cachent l'une l'autre
    CHECK_EQ(r.fires, 1);
    r.Feed(0, false);
    CHECK_EQ(r.fires, 1);
}

TEST(Clap_pas_de_double_declenchement_et_repos) {
    Run r;
    r.Apart(500);
    r.Close(0.40, 0.06, 200);
    r.Close(0.06, 0.40, 150);
    r.Close(0.40, 0.06, 200);  // deuxieme choc < 1.5 s apres
    CHECK_EQ(r.fires, 1);
    r.Close(0.06, 0.40, 300);
    r.Apart(1500);
    r.Close(0.40, 0.06, 200);
    CHECK_EQ(r.fires, 2);
}

TEST(Clap_mains_qui_ne_se_touchent_pas) {
    Run r;
    r.Apart(500);
    r.Close(0.40, 0.18, 150);  // rapide mais s'arrete a 18 cm
    r.Apart(500, 0.18);
    CHECK_EQ(r.fires, 0);
}
