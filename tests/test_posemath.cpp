#include <cmath>

#include "TestFramework.h"
#include "emulation/PoseMath.h"

using namespace hands::emulation;

namespace {
bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }
bool Near(Vec3 a, Vec3 b, float eps = 1e-4f) { return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps); }
// Rotation de `deg` degrés autour de l'axe Y.
Quat RotY(float deg) {
    const float h = deg * 3.14159265f / 180.0f / 2.0f;
    return {0, std::sin(h), 0, std::cos(h)};
}
}  // namespace

TEST(PoseMath_rotation_90_autour_de_Y) {
    // 90° autour de Y : +Z devient +X.
    CHECK(Near(Rotate(RotY(90), {0, 0, 1}), {1, 0, 0}));
}

TEST(PoseMath_inverse_compose_donne_identite) {
    Pose a{RotY(37), {0.3f, -0.2f, 0.9f}};
    Pose id = Multiply(a, Inverse(a));
    CHECK(Near(id.p, {0, 0, 0}));
    CHECK(Near(std::fabs(id.q.w), 1.0f));
}

TEST(PoseMath_pointe_dans_la_paume_suit_la_paume) {
    // Principe du verrouillage : relatif = inverse(paume0) * pointe0 ; sortie = paume1 * relatif.
    Pose palm0{RotY(0), {0, 1, 0}};
    Pose tip0{RotY(0), {0, 1, -0.1f}};  // 10 cm devant la paume (-Z)
    Pose rel = Multiply(Inverse(palm0), tip0);
    Pose palm1{RotY(90), {1, 1, 0}};     // la paume a tourné de 90° et s'est déplacée
    Pose out = Multiply(palm1, rel);
    // -Z tourné de 90° autour de Y devient -X : la pointe est 10 cm « devant » la nouvelle paume.
    CHECK(Near(out.p, {0.9f, 1.0f, 0.0f}));
}

TEST(PoseMath_nlerp_et_angle) {
    CHECK(Near(AngleBetween(RotY(0), RotY(30)), 30.0f * 3.14159265f / 180.0f, 1e-3f));
    Quat m = Nlerp(RotY(0), RotY(40), 0.5f);
    CHECK(Near(AngleBetween(m, RotY(20)), 0.0f, 1e-3f));
}

TEST(OneEuro_lisse_le_bruit_a_l_arret) {
    OneEuro f;
    Quat out{};
    float maxDev = 0;
    for (int i = 0; i < 300; ++i) {
        const float noise = (i % 2 ? 1.0f : -1.0f) * 3.0f;  // ±3° de bruit
        out = f.Filter(RotY(noise), 1.0f / 90.0f, 1.0f, 0.5f);
        if (i > 100) maxDev = std::fmax(maxDev, AngleBetween(out, RotY(0)));
    }
    CHECK(maxDev < 3.0f * 3.14159265f / 180.0f * 0.5f);  // au moins 2x plus calme que le bruit brut
}

TEST(OneEuro_suit_un_mouvement_rapide) {
    OneEuro f;
    Quat out{};
    for (int i = 0; i <= 90; ++i) out = f.Filter(RotY(i * 1.0f), 1.0f / 90.0f, 1.0f, 2.0f);  // 90°/s pendant 1 s
    CHECK(AngleBetween(out, RotY(90)) < 15.0f * 3.14159265f / 180.0f);  // retard limité
}

TEST(OneEuro_position_premiere_valeur_passe_telle_quelle) {
    OneEuro f;
    CHECK(Near(f.Filter(Vec3{1, 2, 3}, 0.011f, 3.0f, 10.0f), {1, 2, 3}));
}

TEST(PoseMath_LookAlong_avant_et_haut) {
    // Avant -Z, haut +Y : identité
    CHECK(Near(Rotate(LookAlong({0, 0, -1}, {0, 1, 0}), {0, 0, -1}), {0, 0, -1}));
    CHECK(Near(Rotate(LookAlong({0, 0, -1}, {0, 1, 0}), {0, 1, 0}), {0, 1, 0}));
    // Avant +X : -Z doit devenir +X et +Y rester vers le haut
    const Quat q = LookAlong({1, 0, 0}, {0, 1, 0});
    CHECK(Near(Rotate(q, {0, 0, -1}), {1, 0, 0}));
    CHECK(Near(Rotate(q, {0, 1, 0}), {0, 1, 0}));
    // Avant incliné vers le bas : le haut reste du côté de `up`
    const Vec3 f = Normalized({0, -1, -1});
    const Quat q2 = LookAlong(f, {0, 1, 0});
    CHECK(Near(Rotate(q2, {0, 0, -1}), f));
    CHECK(Rotate(q2, {0, 1, 0}).y > 0.5f);
}

TEST(PoseMath_LookAlong_cas_degeneres) {
    // `up` parallèle à l'avant : doit rester une rotation valide dont -Z pointe vers l'avant
    const Quat q = LookAlong({0, 1, 0}, {0, 1, 0});
    CHECK(Near(Rotate(q, {0, 0, -1}), {0, 1, 0}));
    CHECK(Near(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w), 1.0f));
    // avant nul : identité
    CHECK(Near(LookAlong({0, 0, 0}, {0, 1, 0}).w, 1.0f));
}

TEST(PoseMath_QuatFromBasis_aller_retour) {
    for (float deg : {0.0f, 33.0f, 90.0f, 135.0f, 180.0f, -120.0f}) {
        const Quat q = Normalize(Multiply(RotY(deg), Quat{0.3f, 0.0f, 0.0f, 0.95f}));
        const Vec3 x = Rotate(q, {1, 0, 0}), y = Rotate(q, {0, 1, 0}), z = Rotate(q, {0, 0, 1});
        const Quat r = QuatFromBasis(x, y, z);
        CHECK(Near(AngleBetween(q, r), 0.0f, 1e-3f));
    }
}

TEST(PoseMath_EstimateShoulder_cote_hauteur_recul) {
    const Pose head{{0, 0, 0, 1}, {0, 1.6f, 0}};  // tete a 1,60 m, regard vers -Z
    const Vec3 r = EstimateShoulder(head, true, 0.18f, 0.17f, 0.05f);
    const Vec3 l = EstimateShoulder(head, false, 0.18f, 0.17f, 0.05f);
    CHECK(Near(r, {0.18f, 1.43f, 0.05f}));   // droite : +X, plus bas, derriere (+Z)
    CHECK(Near(l, {-0.18f, 1.43f, 0.05f}));  // gauche : -X
}

TEST(PoseMath_EstimateShoulder_suit_le_lacet_de_la_tete) {
    // Tete tournee de 90 degres : elle regarde vers -X, sa droite est -Z, son arriere est +X
    const Pose head{RotY(90), {1, 1.6f, 2}};
    CHECK(Near(Rotate(head.q, {0, 0, -1}), {-1, 0, 0}));
    const Vec3 r = EstimateShoulder(head, true, 0.18f, 0.17f, 0.05f);
    CHECK(Near(r, {1.05f, 1.43f, 1.82f}));
}

TEST(PoseMath_EstimateShoulder_ignore_le_tangage) {
    // Regard vers le bas (tete penchee de 50 degres autour de X) : l'epaule ne bouge pas
    const float h = 50.0f * 3.14159265f / 180.0f / 2.0f;
    const Pose flat{{0, 0, 0, 1}, {0, 1.6f, 0}};
    const Pose down{{-std::sin(h), 0, 0, std::cos(h)}, {0, 1.6f, 0}};
    CHECK(Near(EstimateShoulder(flat, true, 0.18f, 0.17f, 0.05f), EstimateShoulder(down, true, 0.18f, 0.17f, 0.05f)));
    // Regard vertical : repli sur l'avant du monde, sans NaN
    const Pose up{{-0.7071068f, 0, 0, 0.7071068f}, {0, 1.6f, 0}};
    const Vec3 s = EstimateShoulder(up, true, 0.18f, 0.17f, 0.05f);
    CHECK(Near(s.y, 1.43f));
}

TEST(PoseMath_rayon_epaule_vers_contact_pointe_vers_l_avant) {
    // Main droite a 30 cm devant, a hauteur de poitrine : le rayon epaule -> main doit pointer vers l'avant (-Z) et un peu vers la gauche
    const Pose head{{0, 0, 0, 1}, {0, 1.6f, 0}};
    const Vec3 shoulder = EstimateShoulder(head, true, 0.18f, 0.17f, 0.05f);
    const Vec3 contact{0.15f, 1.40f, -0.30f};
    const Vec3 d = Normalized(contact - shoulder);
    CHECK(d.z < -0.7f);
    CHECK(d.x < 0.0f);
    CHECK(Near(Rotate(LookAlong(d, {0, 1, 0}), {0, 0, -1}), d));
}

TEST(PoseMath_RayQuadHit_panneau_en_face) {
    // Panneau a 0,6 m devant (-Z), face a l'utilisateur (son +Z regarde vers +Z du monde = vers l'utilisateur)
    const Pose quad{{0, 0, 0, 1}, {0.0f, 1.5f, -0.6f}};
    float lx = 0, ly = 0, t = 0;
    // Rayon droit devant depuis (0.1, 1.4, 0) : touche en (+0.1 droite, -0.1 bas)
    CHECK(RayQuadHit({0.1f, 1.4f, 0.0f}, {0, 0, -1}, quad, &lx, &ly, &t));
    CHECK(Near(lx, 0.1f) && Near(ly, -0.1f) && Near(t, 0.6f));
    // Rayon incline : depuis l'origine vers le centre du panneau
    const Vec3 o{0, 1.5f, 0};
    CHECK(RayQuadHit(o, Normalized(quad.p - o), quad, &lx, &ly, &t));
    CHECK(Near(lx, 0.0f) && Near(ly, 0.0f));
}

TEST(PoseMath_RayQuadHit_cas_limites) {
    const Pose quad{{0, 0, 0, 1}, {0, 1.5f, -0.6f}};
    float lx = 0, ly = 0, t = 0;
    CHECK(!RayQuadHit({0, 1.5f, 0}, {0, 0, 1}, quad, &lx, &ly, &t));    // le rayon s'eloigne du panneau
    CHECK(!RayQuadHit({0, 1.5f, 0}, {1, 0, 0}, quad, &lx, &ly, &t));    // parallele au panneau
    CHECK(!RayQuadHit({0, 1.5f, -1.0f}, {0, 0, -1}, quad, &lx, &ly, &t));  // panneau derriere l'origine
    // Panneau tourne de 90 degres autour de Y : ses axes locaux tournent avec lui
    const Pose side{RotY(90), {-0.6f, 1.5f, 0.0f}};  // +Z local -> +X monde : regarde vers l'utilisateur place a droite (+X)
    CHECK(RayQuadHit({0, 1.5f, -0.1f}, {-1, 0, 0}, side, &lx, &ly, &t));
    CHECK(Near(t, 0.6f));
    CHECK(Near(std::fabs(lx), 0.1f));
}

TEST(PoseMath_HeadYaw_et_WrapAngle) {
    CHECK(Near(HeadYaw({0, 0, 0, 1}, 9.0f), 0.0f));                                  // regard vers -Z
    CHECK(Near(HeadYaw(RotY(90), 9.0f), 3.14159265f / 2.0f, 1e-3f));                 // tete tournee a gauche de 90 degres
    CHECK(Near(HeadYaw(RotY(-90), 9.0f), -3.14159265f / 2.0f, 1e-3f));
    CHECK(Near(HeadYaw({-0.7071068f, 0, 0, 0.7071068f}, 9.0f), 9.0f));               // regard vertical : repli
    CHECK(Near(WrapAngle(4.0f), 4.0f - 2.0f * 3.14159265f, 1e-4f));
    CHECK(Near(WrapAngle(-4.0f), -4.0f + 2.0f * 3.14159265f, 1e-4f));
}

TEST(PoseMath_EstimateShoulderYaw_suit_le_torse_pas_la_tete) {
    const Vec3 head{0, 1.6f, 0};
    // Torse de face : comme avant
    CHECK(Near(EstimateShoulderYaw(head, 0.0f, true, 0.18f, 0.17f, 0.05f), {0.18f, 1.43f, 0.05f}));
    // La tete se tourne de 90 degres mais le torse reste de face : l'epaule droite ne bouge pas
    const Pose headTurned{RotY(90), head};
    const Vec3 fromHead = EstimateShoulder(headTurned, true, 0.18f, 0.17f, 0.05f);
    const Vec3 fromTorso = EstimateShoulderYaw(head, 0.0f, true, 0.18f, 0.17f, 0.05f);
    CHECK(!Near(fromHead, fromTorso, 0.05f));  // l'ancienne methode deplacait l'epaule de ~25 cm
    CHECK(Near(fromTorso, {0.18f, 1.43f, 0.05f}));
    // Torse tourne de 90 degres a gauche : meme resultat que la tete tournee
    CHECK(Near(EstimateShoulderYaw(head, 3.14159265f / 2.0f, true, 0.18f, 0.17f, 0.05f), fromHead, 1e-4f));
}
