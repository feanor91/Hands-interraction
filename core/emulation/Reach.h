// Reach.h — portée des mains : les mains VIRTUELLES vont plus (ou moins) loin que les vraies, à partir d'un centre proche du corps.
// Déplacement virtuel = k x déplacement réel, mesuré depuis une épaule estimée. k = 1 : comportement inchangé (décalage exactement nul).
// Portable, testé.
//
// Le centre (épaules) est calculé sur une ANCRE figée (position + lacet de la tête) et non sur la tête vivante : sinon baisser les yeux vers un
// bouton (la tête bouge de quelques cm) déplacerait la main virtuelle de (1 - k) x ce mouvement.
#pragma once

#include <cmath>
#include <cstdint>

#include "PoseMath.h"

namespace hands::emulation {

// Facteurs de portée : `k` global, multiplié par un facteur par axe du corps (avant, côté, haut).
struct ReachScale {
    float k = 1.0f;
    float kForward = 1.0f, kSide = 1.0f, kUp = 1.0f;
    float Forward() const { return k * kForward; }
    float Side() const { return k * kSide; }
    float Up() const { return k * kUp; }
    bool IsIdentity() const { return Forward() == 1.0f && Side() == 1.0f && Up() == 1.0f; }
};

// Décalage (à ajouter à TOUTES les articulations d'une main) qui amène le point réel `real` au point virtuel. `shoulder` = centre de la portée,
// `yaw` = lacet du corps (même convention que EstimateShoulderYaw). Décalage limité à `maxLen` mètres (garde-fou).
inline Vec3 ReachOffset(const Vec3& real, const Vec3& shoulder, float yaw, const ReachScale& s, float maxLen = 1.0f) {
    if (s.IsIdentity()) return {0.0f, 0.0f, 0.0f};
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 fh{-std::sin(yaw), 0.0f, -std::cos(yaw)};
    const Vec3 rightH = Cross(fh, up);
    const Vec3 d = real - shoulder;
    const float f = Dot(d, fh), r = Dot(d, rightH), u = Dot(d, up);
    Vec3 off = fh * (f * (s.Forward() - 1.0f)) + rightH * (r * (s.Side() - 1.0f)) + up * (u * (s.Up() - 1.0f));
    const float len = Length(off);
    if (len > maxLen) off = off * (maxLen / len);
    return off;
}

// Ancre figée (tête) du centre de portée, avec ses règles de recalage. Appeler Update() à chaque image où la tête est valide.
class ReachCenter {
public:
    static constexpr float kJumpPosM = 0.20f;                 // saut de position entre deux images = l'espace du jeu a bougé
    static constexpr float kJumpYawRad = 25.0f * 3.14159265f / 180.0f;
    static constexpr float kSteadyPosM = 0.03f;               // « tête immobile » : moins de 3 cm ...
    static constexpr float kSteadyYawRad = 5.0f * 3.14159265f / 180.0f;  // ... et 5 degrés ...
    static constexpr uint64_t kSteadyMs = 500;                // ... pendant 0,5 s
    static constexpr float kDriftM = 0.30f;                   // tête éloignée de l'ancre de plus de 30 cm ...
    static constexpr uint64_t kDriftMs = 4000;                // ... pendant plus de 4 s : recalage automatique

    void Update(const Pose& head, uint64_t nowMs) {
        const float yaw = HeadYaw(head.q, havePrev_ ? prevYaw_ : 0.0f);
        if (!valid_) {
            Capture(head.p, yaw, "premiere image");
        } else if (request_) {
            Capture(head.p, yaw, "demande");
        } else {
            if (havePrev_ && !jumpPending_ &&
                (Length(head.p - prevPos_) > kJumpPosM || std::fabs(WrapAngle(yaw - prevYaw_)) > kJumpYawRad)) {
                jumpPending_ = true;  // l'espace du jeu a sauté : on recapture quand la tête est stable
                steadyPos_ = head.p;
                steadyYaw_ = yaw;
                steadySince_ = nowMs;
            }
            if (jumpPending_) {
                if (Length(head.p - steadyPos_) > kSteadyPosM || std::fabs(WrapAngle(yaw - steadyYaw_)) > kSteadyYawRad) {
                    steadyPos_ = head.p;
                    steadyYaw_ = yaw;
                    steadySince_ = nowMs;
                } else if (nowMs - steadySince_ >= kSteadyMs) {
                    Capture(head.p, yaw, "saut de l'espace du jeu");
                }
            } else if (Length(head.p - anchorPos_) > kDriftM) {
                if (!drifting_) {
                    drifting_ = true;
                    driftSince_ = nowMs;
                } else if (nowMs - driftSince_ > kDriftMs) {
                    Capture(head.p, yaw, "deplacement durable de la tete");
                }
            } else {
                drifting_ = false;
            }
        }
        prevPos_ = head.p;
        prevYaw_ = yaw;
        havePrev_ = true;
    }

    void RequestRecapture() { request_ = true; }
    bool Valid() const { return valid_; }
    const Vec3& AnchorPos() const { return anchorPos_; }
    float AnchorYaw() const { return anchorYaw_; }

    // Épaule estimée (repère du jeu) à partir de l'ancre.
    Vec3 Shoulder(bool rightHand, float side, float drop, float back) const {
        return EstimateShoulderYaw(anchorPos_, anchorYaw_, rightHand, side, drop, back);
    }

    // Raison de la dernière capture, une seule fois (nullptr si rien de nouveau) : pour la journalisation.
    const char* TakeCaptureReason() {
        const char* r = reason_;
        reason_ = nullptr;
        return r;
    }

private:
    void Capture(const Vec3& pos, float yaw, const char* why) {
        anchorPos_ = pos;
        anchorYaw_ = yaw;
        valid_ = true;
        request_ = false;
        jumpPending_ = false;
        drifting_ = false;
        reason_ = why;
    }

    bool valid_ = false, request_ = false, jumpPending_ = false, drifting_ = false, havePrev_ = false;
    Vec3 anchorPos_, prevPos_, steadyPos_;
    float anchorYaw_ = 0.0f, prevYaw_ = 0.0f, steadyYaw_ = 0.0f;
    uint64_t steadySince_ = 0, driftSince_ = 0;
    const char* reason_ = nullptr;
};

}  // namespace hands::emulation
