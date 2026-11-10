// ZoneBox.h — boite orientee d'une zone du pochoir (comme les boites d'interaction d'Archan XR) : sa taille depend de la commande
// (petite pour un bouton, grande pour un ecran). Portable, testee. Distances en metres, angles en degres.
#pragma once

#include <algorithm>
#include <cmath>

#include "emulation/PoseMath.h"

namespace hands::emulation {

struct ZoneBox {
    Vec3 center;
    Quat q{0, 0, 0, 1};
    Vec3 size{0.03f, 0.03f, 0.04f};  // dimensions totales selon les axes locaux X, Y, Z
};

// Orientation : lacet (autour de Y, vers le haut), tangage (autour de X), roulis (autour de Z), appliques dans cet ordre.
inline Quat QuatFromEulerDeg(double yawDeg, double pitchDeg, double rollDeg) {
    auto axis = [](float x, float y, float z, double deg) {
        const float h = static_cast<float>(deg * 3.14159265358979323846 / 360.0);
        const float s = std::sin(h);
        return Quat{x * s, y * s, z * s, std::cos(h)};
    };
    return Normalize(Multiply(Multiply(axis(0, 1, 0, yawDeg), axis(1, 0, 0, pitchDeg)), axis(0, 0, 1, rollDeg)));
}

// Point dans le repere de la boite.
inline Vec3 ToLocal(const ZoneBox& b, Vec3 p) { return Rotate(Conjugate(b.q), p - b.center); }

// Vrai si `p` est dans la boite, agrandie de `margin` de chaque cote (taille du bout du doigt) puis multipliee par `scale`.
inline bool BoxContains(const ZoneBox& b, Vec3 p, float margin = 0.0f, float scale = 1.0f) {
    const Vec3 l = ToLocal(b, p);
    return std::fabs(l.x) <= (b.size.x * 0.5f + margin) * scale && std::fabs(l.y) <= (b.size.y * 0.5f + margin) * scale &&
           std::fabs(l.z) <= (b.size.z * 0.5f + margin) * scale;
}

// Coin i (0..7) : le bit 0 de i = signe de X, bit 1 = Y, bit 2 = Z.
inline Vec3 BoxCorner(const ZoneBox& b, int i) {
    const Vec3 l{(i & 1 ? 0.5f : -0.5f) * b.size.x, (i & 2 ? 0.5f : -0.5f) * b.size.y, (i & 4 ? 0.5f : -0.5f) * b.size.z};
    return b.center + Rotate(b.q, l);
}

// Boite d'un curseur : axe Z local le long du segment [a, b] (course), section `width` x `height`, allongee de `width` aux deux bouts.
inline ZoneBox SegmentBox(Vec3 a, Vec3 b, float width, float height) {
    ZoneBox z;
    z.center = (a + b) * 0.5f;
    const Vec3 d = b - a;
    const float len = Length(d);
    z.q = len > 1e-4f ? LookAlong(d * (1.0f / len), Vec3{0, 1, 0}) : Quat{0, 0, 0, 1};
    z.size = {width, height, len + width};
    return z;
}

}  // namespace hands::emulation
