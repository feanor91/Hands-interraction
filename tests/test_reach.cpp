#include <cmath>

#include "TestFramework.h"
#include "emulation/Reach.h"

using namespace hands::emulation;

namespace {
bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }
bool Near(Vec3 a, Vec3 b, float eps = 1e-4f) { return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps); }
Quat RotX(float deg) {
    const float h = deg * 3.14159265f / 180.0f / 2.0f;
    return {std::sin(h), 0, 0, std::cos(h)};
}
// Tête regardant vers -Z (lacet 0), à la position p.
Pose Head(Vec3 p) { return {Quat{0, 0, 0, 1}, p}; }
// Virtuel = épaule + d' ; `yaw` 0 : avant = -Z, droite = +X.
Vec3 Virtual(const Vec3& real, const Vec3& sh, const ReachScale& s) { return real + ReachOffset(real, sh, 0.0f, s); }
}  // namespace

TEST(Reach_k1_decalage_nul) {
    ReachScale s;
    const Vec3 off = ReachOffset({0.3f, 1.1f, -0.5f}, {0.2f, 1.2f, 0.0f}, 0.7f, s);
    CHECK(off.x == 0.0f && off.y == 0.0f && off.z == 0.0f);
}

TEST(Reach_k15_main_a_40cm_devant_l_epaule_passe_a_60cm) {
    ReachScale s;
    s.k = 1.5f;
    const Vec3 sh{0.2f, 1.2f, 0.0f};
    const Vec3 real{0.2f, 1.2f, -0.4f};  // 0,4 m devant l'épaule (avant = -Z)
    CHECK(Near(Virtual(real, sh, s), {0.2f, 1.2f, -0.6f}));
}

TEST(Reach_facteurs_par_axe_n_agissent_que_sur_leur_axe) {
    const Vec3 sh{0, 1.2f, 0};
    const Vec3 real{0.1f, 1.4f, -0.4f};  // droite 0,1 ; haut 0,2 ; avant 0,4
    ReachScale f;
    f.kForward = 2.0f;
    CHECK(Near(Virtual(real, sh, f), {0.1f, 1.4f, -0.8f}));
    ReachScale r;
    r.kSide = 2.0f;
    CHECK(Near(Virtual(real, sh, r), {0.2f, 1.4f, -0.4f}));
    ReachScale u;
    u.kUp = 2.0f;
    CHECK(Near(Virtual(real, sh, u), {0.1f, 1.6f, -0.4f}));
}

TEST(Reach_translation_rigide_conserve_les_distances) {
    ReachScale s;
    s.k = 1.8f;
    const Vec3 sh{0, 1.2f, 0};
    const Vec3 a{0.1f, 1.3f, -0.3f}, b{0.12f, 1.31f, -0.36f};  // deux articulations voisines
    const Vec3 off = ReachOffset(a, sh, 0.3f, s);  // meme decalage pour toute la main
    CHECK(Near(Length((a + off) - (b + off)), Length(a - b)));
    CHECK(Length(off) > 0.1f);
}

TEST(Reach_decalage_limite_a_1m) {
    ReachScale s;
    s.k = 2.5f;
    const Vec3 off = ReachOffset({0, 1.2f, -5.0f}, {0, 1.2f, 0}, 0.0f, s);
    CHECK(Near(Length(off), 1.0f));
}

TEST(Reach_inclinaison_de_la_tete_apres_capture_sans_effet) {
    ReachCenter c;
    c.Update(Head({0, 1.2f, 0}), 0);
    CHECK(c.Valid());
    ReachScale s;
    s.k = 1.5f;
    const Vec3 real{0.3f, 1.0f, -0.4f};
    const Vec3 before = real + ReachOffset(real, c.Shoulder(true, 0.18f, 0.17f, 0.05f), c.AnchorYaw(), s);
    c.Update({RotX(-50), {0, 1.15f, -0.05f}}, 16);  // regard vers le bas, tête qui bouge de quelques cm
    const Vec3 after = real + ReachOffset(real, c.Shoulder(true, 0.18f, 0.17f, 0.05f), c.AnchorYaw(), s);
    CHECK(Near(before, after, 1e-6f));
}

TEST(Reach_saut_de_0_5m_recapture_apres_stabilisation) {
    ReachCenter c;
    c.Update(Head({0, 1.2f, 0}), 0);
    c.TakeCaptureReason();
    c.Update(Head({0.5f, 1.2f, 0}), 16);  // saut
    CHECK(Near(c.AnchorPos().x, 0.0f));
    c.Update(Head({0.5f, 1.2f, 0}), 300);
    CHECK(Near(c.AnchorPos().x, 0.0f));  // pas encore : 0,5 s non ecoulee
    c.Update(Head({0.5f, 1.2f, 0}), 600);
    CHECK(Near(c.AnchorPos().x, 0.5f));
    CHECK(c.TakeCaptureReason() != nullptr);
    CHECK(c.TakeCaptureReason() == nullptr);
}

TEST(Reach_derive_de_35cm_pendant_4s_recapture_pas_pendant_2s) {
    ReachCenter c;
    c.Update(Head({0, 1.2f, 0}), 0);
    // la tete s'eloigne doucement (pas de saut) jusqu'a 0,35 m
    uint64_t t = 0;
    for (int i = 1; i <= 7; ++i) c.Update(Head({0.05f * i, 1.2f, 0}), t += 16);
    for (; t < 2000; t += 100) c.Update(Head({0.35f, 1.2f, 0}), t);
    CHECK(Near(c.AnchorPos().x, 0.0f));  // 2 s : pas de recalage
    for (; t < 4600; t += 100) c.Update(Head({0.35f, 1.2f, 0}), t);
    CHECK(Near(c.AnchorPos().x, 0.35f));  // plus de 4 s : recalage
}

TEST(Reach_derive_interrompue_ne_recapture_pas) {
    ReachCenter c;
    c.Update(Head({0, 1.2f, 0}), 0);
    uint64_t t = 0;
    for (int i = 1; i <= 7; ++i) c.Update(Head({0.05f * i, 1.2f, 0}), t += 16);
    for (; t < 3000; t += 100) c.Update(Head({0.35f, 1.2f, 0}), t);
    for (int i = 6; i >= 0; --i) c.Update(Head({0.05f * i, 1.2f, 0}), t += 16);  // revient pres de l'ancre
    for (; t < 9000; t += 100) c.Update(Head({0.0f, 1.2f, 0}), t);
    CHECK(Near(c.AnchorPos().x, 0.0f));
}

TEST(Reach_demande_de_recalage) {
    ReachCenter c;
    c.Update(Head({0, 1.2f, 0}), 0);
    c.TakeCaptureReason();
    c.RequestRecapture();
    c.Update(Head({0.1f, 1.25f, 0.05f}), 16);
    CHECK(Near(c.AnchorPos(), {0.1f, 1.25f, 0.05f}));
    CHECK(c.TakeCaptureReason() != nullptr);
}
