// ServoMath.h - projection maths shared by the pointer servo (portable, header-only, no OpenXR / D3D12 types).
// Proposed location in the repo: core/servo/ServoMath.h (hands_core is a static lib with `core` as PUBLIC include dir, tests link it).
//
// Conventions (checked against the OpenXR 1.1 spec pages for XR_KHR_D3D12_enable and the layer's own CaptureFrame):
//  * Eye frame = OpenXR view space: +X right, +Y up, -Z forward. View pose is expressed in the game's reference space (game->space).
//  * fov angles in radians with OpenXR signs: left < 0, down < 0, right > 0, up > 0. The layer image is the PLANAR projection of the
//    tangent rectangle [tan(left), tan(right)] x [tan(down), tan(up)] onto imageRect.
//  * Texture / swapchain image: origin at the TOP-LEFT texel, +x right, +y DOWN (so v grows when the world y shrinks).
//  * All pixel positions here are continuous "texel-edge" coordinates in the TEXTURE: texel i covers [i, i+1), its centre is i + 0.5.
//    (The detector must return centroids with the same convention: centre of texel (ix,iy) = (ix + 0.5, iy + 0.5).)
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "emulation/PoseMath.h"

namespace hands::servo {
namespace em = hands::emulation;

constexpr double kPi = 3.14159265358979323846;

struct Fov {
    float left = -0.8f, right = 0.8f, up = 0.8f, down = -0.8f;  // radians, OpenXR signs
};

// XrRect2Di of subImage.imageRect, in texels of the swapchain image (NOT assumed to be {0,0,texW,texH}).
struct ImageRect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct EyeView {
    em::Pose pose;  // eye in the game's reference space
    Fov fov;
    ImageRect rect;
};

struct Px {
    double x = 0, y = 0;
};

// World point -> texture pixel. False when the point is not in front of the eye (depth <= minDepth) - same rule as CaptureFrame (0.05 m).
inline bool Project(const EyeView& v, const em::Vec3& world, Px* out, double* depth = nullptr, double minDepth = 0.02) {
    const em::Vec3 e = em::Rotate(em::Conjugate(v.pose.q), world - v.pose.p);
    const double z = -static_cast<double>(e.z);
    if (z <= minDepth || v.rect.w <= 0 || v.rect.h <= 0) return false;
    const double tl = std::tan(static_cast<double>(v.fov.left)), tr = std::tan(static_cast<double>(v.fov.right));
    const double tu = std::tan(static_cast<double>(v.fov.up)), td = std::tan(static_cast<double>(v.fov.down));
    const double u = (static_cast<double>(e.x) / z - tl) / (tr - tl);
    const double w = (tu - static_cast<double>(e.y) / z) / (tu - td);
    out->x = v.rect.x + u * v.rect.w;
    out->y = v.rect.y + w * v.rect.h;
    if (depth) *depth = z;
    return true;
}

// Texture pixel -> unit direction in the game's reference space (the ray starts at v.pose.p).
inline em::Vec3 RayDir(const EyeView& v, const Px& p) {
    const double tl = std::tan(static_cast<double>(v.fov.left)), tr = std::tan(static_cast<double>(v.fov.right));
    const double tu = std::tan(static_cast<double>(v.fov.up)), td = std::tan(static_cast<double>(v.fov.down));
    const double u = (p.x - v.rect.x) / v.rect.w, w = (p.y - v.rect.y) / v.rect.h;
    const em::Vec3 e{static_cast<float>(tl + u * (tr - tl)), static_cast<float>(tu - w * (tu - td)), -1.0f};
    return em::Rotate(v.pose.q, em::Normalized(e));
}

// Pointer / fingertip direction as (yaw, pitch) in game axes, the convention of the existing mouse code (mouse_click.cpp Angles()):
// yaw = atan2(x, -z) grows to the RIGHT, pitch = asin(y) grows UP. 1 OS pixel of the real mouse = 1/1193.7 rad (mouse.ini fit).
struct Ang {
    double yaw = 0, pitch = 0;
};
inline Ang AnglesOf(const em::Vec3& d) {
    return {std::atan2(static_cast<double>(d.x), static_cast<double>(-d.z)), std::asin(std::max(-1.0, std::min(1.0, static_cast<double>(d.y))))};
}
inline em::Vec3 DirOf(const Ang& a) {
    return {static_cast<float>(std::sin(a.yaw) * std::cos(a.pitch)), static_cast<float>(std::sin(a.pitch)), static_cast<float>(-std::cos(a.yaw) * std::cos(a.pitch))};
}

// Closest points of two rays (oL + s*dL, oR + t*dR), unit directions. Returns false for (nearly) parallel rays.
// `mid` = midpoint of the shortest segment, `gap` = its length (metres, ~0 when both detections see the same 3D point).
inline bool Triangulate(const em::Vec3& oL, const em::Vec3& dL, const em::Vec3& oR, const em::Vec3& dR, em::Vec3* mid, double* gap) {
    const em::Vec3 w0 = oL - oR;
    const double a = em::Dot(dL, dL), b = em::Dot(dL, dR), c = em::Dot(dR, dR), d = em::Dot(dL, w0), e = em::Dot(dR, w0);
    const double den = a * c - b * b;
    if (den < 1e-9) return false;
    const double s = (b * e - c * d) / den, t = (a * e - b * d) / den;
    if (s <= 0 || t <= 0) return false;  // behind an eye
    const em::Vec3 pL = oL + dL * static_cast<float>(s), pR = oR + dR * static_cast<float>(t);
    *mid = (pL + pR) * 0.5f;
    *gap = em::Length(pL - pR);
    return true;
}

// ---- readback region of interest --------------------------------------------------------------------------------------------------
struct Roi {
    int x = 0, y = 0, w = 0, h = 0;
};

// Bytes the layer's DepthReader / a readback ring needs for a w x h RGBA8 box: rows are padded to 256 bytes (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT).
inline long long RoiBytes(int w, int h) { return static_cast<long long>((static_cast<long long>(w) * 4 + 255) / 256 * 256) * h; }

// Box (texture texels) around `n` points + `margin`, at least minW x minH, at most maxW x maxH and `budgetBytes` (DepthReader: 4 MiB), shifted to stay
// inside `img` (the sub-image rect). Returns {0,0,0,0} when `img` is empty. Centre is kept on the points' bounding box.
inline Roi MakeRoi(const ImageRect& img, const Px* pts, int n, int margin, int minW, int minH, int maxW, int maxH, long long budgetBytes = 4ll << 20) {
    if (img.w <= 0 || img.h <= 0 || n <= 0) return {};
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    for (int i = 0; i < n; ++i) {
        x0 = std::min(x0, pts[i].x);
        y0 = std::min(y0, pts[i].y);
        x1 = std::max(x1, pts[i].x);
        y1 = std::max(y1, pts[i].y);
    }
    int w = std::max(minW, static_cast<int>(std::ceil(x1 - x0)) + 2 * margin), h = std::max(minH, static_cast<int>(std::ceil(y1 - y0)) + 2 * margin);
    w = std::min(std::min(w, maxW), img.w);
    h = std::min(std::min(h, maxH), img.h);
    while (RoiBytes(w, h) > budgetBytes && h > 1) --h;  // rows are the cheap dimension to give up
    const double cx = 0.5 * (x0 + x1), cy = 0.5 * (y0 + y1);
    Roi r;
    r.w = w;
    r.h = h;
    r.x = std::max(img.x, std::min(static_cast<int>(std::lround(cx - w * 0.5)), img.x + img.w - w));
    r.y = std::max(img.y, std::min(static_cast<int>(std::lround(cy - h * 0.5)), img.y + img.h - h));
    return r;
}

}  // namespace hands::servo
