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

#include "emulation/CockpitAnchor.h"
#include "util/KeyNames.h"

TEST(CockpitAnchor_les_zones_suivent_le_recentrage) {
    using namespace hands::emulation;
    const Pose head0{QuatFromEulerDeg(0, 0, 0), {0.1f, 1.2f, 0.3f}};
    const CockpitAnchor a0 = CockpitAnchor::FromHead(head0);
    const Vec3 world = {0.1f, 1.1f, -0.2f};
    const Vec3 rel = a0.ToRel(world);
    CHECK(Length(a0.ToWorld(rel) - world) < 1e-5f);
    // Nouveau recentrage : tete deplacee de 40 cm et tournee de 90 degres -> le bouton suit.
    const Pose head1{QuatFromEulerDeg(90, 10, 5), {0.5f, 1.25f, -0.4f}};
    const CockpitAnchor a1 = CockpitAnchor::FromHead(head1);
    const Vec3 w1 = a1.ToWorld(rel);
    const Vec3 d0 = world - head0.p, d1 = w1 - head1.p;
    CHECK(std::fabs(std::sqrt(d0.x * d0.x + d0.z * d0.z) - std::sqrt(d1.x * d1.x + d1.z * d1.z)) < 1e-4f);
    CHECK(std::fabs(d0.y - d1.y) < 1e-5f);
    CHECK(Length(a1.ToRel(w1) - rel) < 1e-5f);
}

TEST(KeyNames_touches) {
    using hands::util::ParseVirtualKey;
    CHECK(ParseVirtualKey("LSHIFT") == 0xA0 && ParseVirtualKey(" lshift ") == 0xA0 && ParseVirtualKey("RCTRL") == 0xA3);
    CHECK(ParseVirtualKey("F9") == 0x78 && ParseVirtualKey("F24") == 0x87 && ParseVirtualKey("F25") == 0);
    CHECK(ParseVirtualKey("a") == 'A' && ParseVirtualKey("7") == '7');
    CHECK(ParseVirtualKey("0xA0") == 0xA0 && ParseVirtualKey("160") == 160);
    CHECK(ParseVirtualKey("") == 0 && ParseVirtualKey("none") == 0 && ParseVirtualKey("n importe quoi") == 0);
}
