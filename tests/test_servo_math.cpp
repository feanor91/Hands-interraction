// Proposed repo location: tests/test_servo_math.cpp (add to core_tests in tests/CMakeLists.txt).
#include <cmath>

#include "TestFramework.h"
#include "servo/ServoMath.h"

using namespace hands::servo;
using hands::emulation::Pose;
using hands::emulation::Quat;
using hands::emulation::Vec3;

namespace {
bool Near(double a, double b, double eps) { return std::fabs(a - b) < eps; }
bool Near(const Vec3& a, const Vec3& b, float eps) { return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps; }
Quat Rot(float ax, float ay, float az, float deg) {  // axis-angle
    const float h = deg * 3.14159265f / 360.0f, n = std::sqrt(ax * ax + ay * ay + az * az), s = std::sin(h) / n;
    return {ax * s, ay * s, az * s, std::cos(h)};
}
// The left eye of the user's log: image 2688x2880, fov (-0.94, 0.70, 0.77, -0.96) (rounded to 0.01 rad in the log).
EyeView LogLeftEye() {
    EyeView v;
    v.fov = {-0.94f, 0.70f, 0.77f, -0.96f};
    v.rect = {0, 0, 2688, 2880};
    return v;
}
}  // namespace

TEST(Servo_Project_axe_optique_asymetrique) {
    // Reference values computed independently (numpy): axis pixel and an off-axis point.
    EyeView v = LogLeftEye();
    Px p;
    CHECK(Project(v, {0, 0, -1}, &p));
    CHECK(Near(p.x, 1664.239, 0.01) && Near(p.y, 1164.560, 0.01));
    CHECK(Project(v, {0.10f, 0.05f, -0.5f}, &p));
    CHECK(Near(p.x, 1907.329, 0.01) && Near(p.y, 1044.461, 0.01));
}

TEST(Servo_Project_conventions_d_axes) {
    // right of the axis -> larger x ; above the axis -> SMALLER y (image origin is the top-left texel)
    EyeView v = LogLeftEye();
    Px c, r, u;
    CHECK(Project(v, {0, 0, -1}, &c) && Project(v, {0.2f, 0, -1}, &r) && Project(v, {0, 0.2f, -1}, &u));
    CHECK(r.x > c.x && Near(r.y, c.y, 1e-6));
    CHECK(u.y < c.y && Near(u.x, c.x, 1e-6));
}

TEST(Servo_Project_imageRect_decalage_et_etendue) {
    // Sub-image {100,50,1000,500}, symmetric fov 0.7: centre -> (600,300) ; point at tan = (+0.5t, -0.25t) -> (850, 362.5)
    EyeView v;
    v.fov = {-0.7f, 0.7f, 0.7f, -0.7f};
    v.rect = {100, 50, 1000, 500};
    const float t = std::tan(0.7f);
    Px p;
    CHECK(Project(v, {0, 0, -1}, &p) && Near(p.x, 600.0, 1e-3) && Near(p.y, 300.0, 1e-3));
    CHECK(Project(v, {0.5f * t, -0.25f * t, -1}, &p) && Near(p.x, 850.0, 0.01) && Near(p.y, 362.5, 0.01));
    // a rect that is NOT the whole texture changes the answer : this is what CaptureFrame (offset 0, extent = texture) cannot express
    EyeView full = v;
    full.rect = {0, 0, 2000, 1200};
    Px q;
    CHECK(Project(full, {0.5f * t, -0.25f * t, -1}, &q) && !Near(q.x, p.x, 1.0));
}

TEST(Servo_Project_pose_oeil_et_aller_retour) {
    EyeView v = LogLeftEye();
    v.pose = {Rot(0.3f, 1.0f, -0.2f, 33.0f), {0.12f, 1.31f, -0.4f}};
    for (const Vec3& w : {Vec3{0.3f, 1.2f, -1.4f}, Vec3{-0.25f, 1.5f, -0.9f}, Vec3{0.9f, 1.0f, -2.2f}}) {
        Px p;
        double depth = 0;
        const bool ok = Project(v, w, &p, &depth);
        if (!ok) continue;  // not in front: skipped
        const Vec3 d = RayDir(v, p);
        const Vec3 expect = hands::emulation::Normalized(w - v.pose.p);
        CHECK(Near(d, expect, 2e-5f));  // ray through the pixel points back at the world point
    }
}

TEST(Servo_Project_derriere_l_oeil) {
    EyeView v = LogLeftEye();
    Px p;
    CHECK(!Project(v, {0, 0, 1}, &p));
    CHECK(!Project(v, {0, 0, -0.01f}, &p));  // closer than the minimum depth
}

TEST(Servo_Gauche_droite_meme_fonction) {
    // right eye of the log: fov (-0.70, 0.94, 0.77, -0.96). A point straight ahead of the HEAD (eyes at x = -+0.032) is slightly right of
    // the left-eye axis pixel in the left image... and left of the right-eye axis pixel in the right image (positive disparity at 0.6 m).
    EyeView L = LogLeftEye(), R = L;
    R.fov = {-0.70f, 0.94f, 0.77f, -0.96f};
    L.pose.p = {-0.032f, 0, 0};
    R.pose.p = {0.032f, 0, 0};
    Px pl, pr, al, ar;
    CHECK(Project(L, {0, 0, -0.6f}, &pl) && Project(R, {0, 0, -0.6f}, &pr));
    CHECK(Project(L, {0, 0, -100.0f}, &al) && Project(R, {0, 0, -100.0f}, &ar));  // ~infinity
    CHECK(pl.x > al.x && pr.x < ar.x);  // parallax sign: left eye sees the near point to the right of the far direction
    // the disparity (pl - pr) in tangent units is 0.064/0.6 = 0.1067 -> check via ray directions
    const Vec3 dl = RayDir(L, pl), dr = RayDir(R, pr);
    Vec3 mid;
    double gap = 0;
    CHECK(Triangulate(L.pose.p, dl, R.pose.p, dr, &mid, &gap));
    CHECK(Near(mid, {0, 0, -0.6f}, 1e-3f) && gap < 1e-4);
}

TEST(Servo_Triangulate_paralleles_refuse) {
    Vec3 mid;
    double gap;
    CHECK(!Triangulate({-0.03f, 0, 0}, {0, 0, -1}, {0.03f, 0, 0}, {0, 0, -1}, &mid, &gap));  // pointer at infinity: no depth
}

TEST(Servo_Angles_convention_de_mouse_click) {
    const Ang a = AnglesOf(hands::emulation::Normalized({0.5f, 0.2f, -1.0f}));
    CHECK(a.yaw > 0 && a.pitch > 0);  // right and up are positive, like Angles() in mouse_click.cpp
    CHECK(Near(DirOf(a), hands::emulation::Normalized({0.5f, 0.2f, -1.0f}), 1e-5f));
    const Ang z = AnglesOf({0, 0, -1});
    CHECK(Near(z.yaw, 0, 1e-9) && Near(z.pitch, 0, 1e-9));
}

TEST(Servo_MakeRoi_bornes_et_budget) {
    const ImageRect img{0, 0, 2688, 2880};
    Px pts[2] = {{1500, 1200}, {1700, 1250}};
    Roi r = MakeRoi(img, pts, 2, 96, 256, 192, 1024, 768);
    CHECK(r.w == 392 && r.h == 242);  // bounding box 200x50 + 2*96
    CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= 2688 && r.y + r.h <= 2880);
    CHECK(r.x <= 1500 && r.x + r.w >= 1700);  // both points inside
    // near the border: shifted, never clipped
    Px corner[1] = {{5, 5}};
    r = MakeRoi(img, corner, 1, 96, 256, 192, 1024, 768);
    CHECK(r.x == 0 && r.y == 0 && r.w == 256 && r.h == 192);
    // huge request: capped at 1024x768 = 3 MiB (fits the 4 MiB DepthReader buffer), and the byte budget is honoured
    Px far[2] = {{0, 0}, {2600, 2800}};
    r = MakeRoi(img, far, 2, 96, 256, 192, 1024, 768);
    CHECK(r.w == 1024 && r.h == 768 && RoiBytes(r.w, r.h) == 3ll * 1024 * 1024);
    r = MakeRoi(img, far, 2, 96, 256, 192, 1280, 1400);
    CHECK(RoiBytes(r.w, r.h) <= (4ll << 20));
    // sub-image smaller than the texture: the ROI stays inside the sub-image
    r = MakeRoi({100, 50, 600, 400}, pts, 0, 0, 1, 1, 10, 10);
    CHECK(r.w == 0);
    Px inside[1] = {{650, 420}};
    r = MakeRoi({100, 50, 600, 400}, inside, 1, 96, 256, 192, 1024, 768);
    CHECK(r.x >= 100 && r.y >= 50 && r.x + r.w <= 700 && r.y + r.h <= 450);
}
