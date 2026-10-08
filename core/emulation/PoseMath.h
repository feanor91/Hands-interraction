// PoseMath.h — petites maths de poses (position + quaternion) et filtre One Euro. Portable, testé.
//
// Convention OpenXR : quaternion (x, y, z, w), rotation d'un vecteur v : q * v * q^-1.
// Une pose `T` envoie un point du repère local vers le repère parent : p_parent = T.p + rot(T.q, p_local).
// Composition `a * b` = « b exprimée dans le repère de a » (b d'abord, puis a).
#pragma once

#include <cmath>

namespace hands::emulation {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

struct Pose {
    Quat q;
    Vec3 p;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Length(Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

inline Quat Conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

inline Quat Multiply(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline Quat Normalize(Quat q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n < 1e-8f) return {0, 0, 0, 1};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

inline float Dot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

inline Vec3 Rotate(Quat q, Vec3 v) {
    // v' = v + 2w(u x v) + 2 u x (u x v), avec u = (x, y, z)
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    const Vec3 ut{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x};
    return {v.x + q.w * t.x + ut.x, v.y + q.w * t.y + ut.y, v.z + q.w * t.z + ut.z};
}

inline Pose Multiply(const Pose& a, const Pose& b) { return {Normalize(Multiply(a.q, b.q)), a.p + Rotate(a.q, b.p)}; }

inline Pose Inverse(const Pose& a) {
    const Quat qi = Conjugate(a.q);
    return {qi, Rotate(qi, a.p) * -1.0f};
}

inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Normalized(Vec3 v) {
    const float n = Length(v);
    return n < 1e-8f ? Vec3{0, 0, 0} : v * (1.0f / n);
}

// Quaternion de la rotation dont les colonnes sont les axes (x, y, z) (orthonormés, repère direct).
inline Quat QuatFromBasis(Vec3 x, Vec3 y, Vec3 z) {
    const float m00 = x.x, m01 = y.x, m02 = z.x, m10 = x.y, m11 = y.y, m12 = z.y, m20 = x.z, m21 = y.z, m22 = z.z;
    const float tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return Normalize(q);
}

// Orientation OpenXR dont l'avant (-Z) pointe vers `forward` et dont le haut (+Y) est au plus près de `up`.
inline Quat LookAlong(Vec3 forward, Vec3 up) {
    const Vec3 z = Normalized(forward * -1.0f);
    if (Length(z) < 0.5f) return {0, 0, 0, 1};
    Vec3 x = Cross(up, z);
    if (Length(x) < 1e-4f) x = Cross(Vec3{0, 1, 0}, z);  // `up` parallèle à l'avant : axe de secours
    if (Length(x) < 1e-4f) x = Cross(Vec3{1, 0, 0}, z);
    x = Normalized(x);
    return QuatFromBasis(x, Cross(z, x), z);
}

// Interpolation linéaire normalisée, du côté le plus court (suffisante pour de petits angles).
inline Quat Nlerp(Quat a, Quat b, float t) {
    if (Dot(a, b) < 0) b = {-b.x, -b.y, -b.z, -b.w};
    return Normalize({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
}

// Angle (radians) entre deux orientations.
inline float AngleBetween(Quat a, Quat b) {
    const float d = std::fabs(Dot(Normalize(a), Normalize(b)));
    return 2.0f * std::acos(d > 1.0f ? 1.0f : d);
}

// Filtre One Euro : lisse fortement quand ça bouge peu (stabilité), peu quand ça bouge vite (réactivité).
// Paramètres : `minCutoffHz` (plus petit = plus lisse à l'arrêt), `beta` (plus grand = moins de retard en mouvement).
class OneEuro {
public:
    // Position (mètres) : `speed` en m/s.
    Vec3 Filter(Vec3 raw, float dt, float minCutoffHz, float beta) {
        if (!init_ || dt <= 0) {
            init_ = true;
            posHat_ = raw;
            speed_ = 0;
            return raw;
        }
        const float inst = Length(raw - posHat_) / dt;
        speed_ += Alpha(1.0f, dt) * (inst - speed_);
        const float a = Alpha(minCutoffHz + beta * speed_, dt);
        posHat_ = posHat_ + (raw - posHat_) * a;
        return posHat_;
    }
    // Orientation : `speed` en rad/s.
    Quat Filter(Quat raw, float dt, float minCutoffHz, float beta) {
        if (!qInit_ || dt <= 0) {
            qInit_ = true;
            qHat_ = Normalize(raw);
            qSpeed_ = 0;
            return qHat_;
        }
        const float inst = AngleBetween(raw, qHat_) / dt;
        qSpeed_ += Alpha(1.0f, dt) * (inst - qSpeed_);
        const float a = Alpha(minCutoffHz + beta * qSpeed_, dt);
        qHat_ = Nlerp(qHat_, raw, a);
        return qHat_;
    }
    void Reset() { init_ = qInit_ = false; }

private:
    static float Alpha(float cutoffHz, float dt) {
        const float tau = 1.0f / (2.0f * 3.14159265f * (cutoffHz > 1e-3f ? cutoffHz : 1e-3f));
        return 1.0f / (1.0f + tau / dt);
    }
    bool init_ = false, qInit_ = false;
    Vec3 posHat_;
    Quat qHat_;
    float speed_ = 0, qSpeed_ = 0;
};

}  // namespace hands::emulation
