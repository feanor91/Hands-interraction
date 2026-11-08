#include "TestFramework.h"
#include "emulation/ZoneTracker.h"

using hands::emulation::Vec3;
using hands::emulation::ZoneTracker;

TEST(ZoneTracker_declenche_apres_le_maintien) {
    ZoneTracker z;
    const Vec3 c{0, 0, 0}, in{0.005f, 0, 0}, out{0.1f, 0, 0};
    CHECK(!z.Update(out, true, c, 0.02f, 0, 250, 300));
    CHECK(!z.Update(in, true, c, 0.02f, 100, 250, 300));
    CHECK(!z.Update(in, true, c, 0.02f, 300, 250, 300));
    CHECK(z.Update(in, true, c, 0.02f, 360, 250, 300));  // 260 ms dedans
    CHECK(!z.Update(in, true, c, 0.02f, 700, 250, 300));  // une seule fois
}

TEST(ZoneTracker_effleurement_ne_declenche_pas) {
    ZoneTracker z;
    const Vec3 c{0, 0, 0}, in{0, 0, 0}, out{0.1f, 0, 0};
    CHECK(!z.Update(in, true, c, 0.02f, 0, 250, 300));
    CHECK(!z.Update(out, true, c, 0.02f, 100, 250, 300));  // sorti avant la fin du maintien
    CHECK(!z.Update(in, true, c, 0.02f, 150, 250, 300));
    CHECK(!z.Update(in, true, c, 0.02f, 350, 250, 300));   // 200 ms seulement depuis le retour
}

TEST(ZoneTracker_rearmement_apres_sortie) {
    ZoneTracker z;
    const Vec3 c{0, 0, 0}, in{0, 0, 0}, out{0.1f, 0, 0};
    z.Update(in, true, c, 0.02f, 0, 100, 300);
    CHECK(z.Update(in, true, c, 0.02f, 120, 100, 300));
    CHECK(!z.Update(in, true, c, 0.02f, 500, 100, 300));   // reste dedans : jamais de redeclenchement
    CHECK(!z.Update(out, true, c, 0.02f, 600, 100, 300));
    CHECK(!z.Update(out, true, c, 0.02f, 800, 100, 300));  // dehors depuis 200 ms : pas encore rearme
    CHECK(!z.Update(out, true, c, 0.02f, 950, 100, 300));
    CHECK(!z.Update(in, true, c, 0.02f, 1000, 100, 300));
    CHECK(z.Update(in, true, c, 0.02f, 1120, 100, 300));   // rearme (dehors > 300 ms) puis maintien
}

TEST(ZoneTracker_doigt_perdu_abandonne) {
    ZoneTracker z;
    const Vec3 c{0, 0, 0};
    z.Update(c, true, c, 0.02f, 0, 250, 300);
    z.Update(c, false, c, 0.02f, 100, 250, 300);
    CHECK(!z.Update(c, true, c, 0.02f, 300, 250, 300));  // le compte repart de zero
}

TEST(ZoneTracker_Disarm_exige_une_sortie) {
    ZoneTracker z;
    const Vec3 c{0, 0, 0}, in{0, 0, 0}, out{0.1f, 0, 0};
    z.Disarm();
    CHECK(!z.Armed());
    CHECK(!z.Update(in, true, c, 0.02f, 0, 100, 300));
    CHECK(!z.Update(in, true, c, 0.02f, 500, 100, 300));   // reste dedans : jamais de declenchement
    CHECK(!z.Update(out, true, c, 0.02f, 600, 100, 300));
    CHECK(!z.Update(out, true, c, 0.02f, 950, 100, 300));  // dehors depuis 350 ms : rearme
    CHECK(z.Armed());
    CHECK(!z.Update(in, true, c, 0.02f, 1000, 100, 300));
    CHECK(z.Update(in, true, c, 0.02f, 1120, 100, 300));
}
