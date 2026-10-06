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
