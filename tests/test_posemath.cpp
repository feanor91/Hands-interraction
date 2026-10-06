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
