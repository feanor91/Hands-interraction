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

#include "emulation/ZoneBox.h"

TEST(ZoneBox_contact_et_orientation) {
    using namespace hands::emulation;
    ZoneBox b;
    b.center = {0, 1, -0.5f};
    b.size = {0.04f, 0.02f, 0.10f};  // plate : large en X, longue en Z
    CHECK(BoxContains(b, {0.019f, 1.0f, -0.5f}));
    CHECK(!BoxContains(b, {0.025f, 1.0f, -0.5f}));
    CHECK(BoxContains(b, {0.025f, 1.0f, -0.5f}, 0.006f));        // marge = taille du bout du doigt
    CHECK(BoxContains(b, {0.0f, 1.0f, -0.46f}) && !BoxContains(b, {0.0f, 1.0f, -0.40f}));
    b.q = QuatFromEulerDeg(90, 0, 0);                              // tourne de 90 degres autour de Y : le long cote passe sur X
    CHECK(BoxContains(b, {0.045f, 1.0f, -0.5f}) && !BoxContains(b, {0.0f, 1.0f, -0.46f}));
    const Vec3 c0 = BoxCorner(b, 0), c7 = BoxCorner(b, 7);
    CHECK(std::fabs((c0 + c7).x * 0.5f - b.center.x) < 1e-5f);     // les coins sont symetriques autour du centre
    const ZoneBox s = SegmentBox({0, 1, 0}, {0, 1, -0.2f}, 0.03f, 0.03f);
    CHECK(std::fabs(s.size.z - 0.23f) < 1e-5f && BoxContains(s, {0, 1, -0.1f}) && BoxContains(s, {0, 1, -0.2f}) && !BoxContains(s, {0.1f, 1, -0.1f}));
}

TEST(ZoneTracker_boite_declenche_apres_maintien) {
    hands::emulation::ZoneTracker t;
    CHECK(!t.UpdateInside(true, true, true, 0, 250, 400));
    CHECK(!t.UpdateInside(true, true, true, 200, 250, 400));
    CHECK(t.UpdateInside(true, true, true, 260, 250, 400));
    CHECK(!t.UpdateInside(true, true, true, 600, 250, 400));        // desarme tant qu'on ne sort pas de la zone agrandie
    CHECK(!t.UpdateInside(true, false, false, 700, 250, 400));
    CHECK(!t.UpdateInside(true, false, false, 1200, 250, 400));
    CHECK(!t.UpdateInside(true, true, true, 1300, 250, 400));       // rearme : maintien a recommencer
    CHECK(t.UpdateInside(true, true, true, 1600, 250, 400));
}
