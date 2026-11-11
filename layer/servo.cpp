// servo.cpp — adaptateur de la couche pour l'asservissement visuel (la logique est dans core/servo/ServoBrain). Voir servo.h.
#include "servo.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#include "depth_reader.h"
#include "log/Logger.h"
#include "mouse_click.h"
#include "servo/ServoBrain.h"
#include "state.h"

namespace layer {
namespace sv = hands::servo;
namespace em = hands::emulation;
using hands::log::LogLevel;
using hands::log::Logger;

namespace {

constexpr int kIndexTip = 10, kDistal = 9, kKnuckle = 7, kWrist = 1;

struct Adapter {
    std::unique_ptr<DepthReader> reader;
    bool readerFailed = false;
    sv::ServoBrain brain;
    bool wired = false;
    sv::BrainRoi pending;  // requete en vol
    bool pendingActive = false;
    int diagSaved = 0;
    uint64_t diagMs = 0;
};

Adapter& A() {
    static Adapter* a = new Adapter;  // jamais detruit (pas de COM dans un destructeur statique)
    return *a;
}

void DrawCross(std::vector<uint8_t>& rgb, int w, int h, int cx, int cy, int arm, uint8_t r, uint8_t g, uint8_t b) {
    auto put = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint8_t* p = &rgb[(static_cast<size_t>(y) * w + x) * 3];
        p[0] = r;
        p[1] = g;
        p[2] = b;
    };
    for (int k = -arm; k <= arm; ++k) {
        put(cx + k, cy);
        put(cx, cy + k);
    }
}

// Recadrage annote (BMP 24 bits) : croix rouge = pointeur detecte, jaune = bout du doigt, vert = position attendue
void SaveDiag(const char* tag, const uint8_t* rgba, const sv::BrainRoi& r, int detX, int detY, int fingerX, int fingerY) {
    Adapter& a = A();
    if (a.diagSaved >= 24) return;
    const uint64_t now = GetTickCount64();
    if (now - a.diagMs < 1500) return;
    a.diagMs = now;
    std::vector<uint8_t> rgb(static_cast<size_t>(r.w) * r.h * 3);
    for (int i = 0; i < r.w * r.h; ++i) {
        rgb[static_cast<size_t>(i) * 3] = rgba[static_cast<size_t>(i) * 4];
        rgb[static_cast<size_t>(i) * 3 + 1] = rgba[static_cast<size_t>(i) * 4 + 1];
        rgb[static_cast<size_t>(i) * 3 + 2] = rgba[static_cast<size_t>(i) * 4 + 2];
    }
    if (detX >= 0) DrawCross(rgb, r.w, r.h, detX, detY, 14, 255, 0, 0);
    if (fingerX > -100000) DrawCross(rgb, r.w, r.h, fingerX, fingerY, 10, 255, 255, 0);
    if (r.expX >= 0) DrawCross(rgb, r.w, r.h, static_cast<int>(r.expX) - r.x, static_cast<int>(r.expY) - r.y, 8, 0, 255, 0);
    char name[96];
    std::snprintf(name, sizeof(name), "servo_%02d_%s.bmp", a.diagSaved + 1, tag);
    const std::filesystem::path file = LayerBaseDir() / "logs" / "captures" / name;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") != 0 || !f) return;
    const uint32_t stride = (static_cast<uint32_t>(r.w) * 3 + 3) & ~3u, imgSize = stride * static_cast<uint32_t>(r.h), fileSize = 54 + imgSize, offset = 54, dib = 40;
    uint8_t hd[54] = {'B', 'M'};
    std::memcpy(hd + 2, &fileSize, 4);
    std::memcpy(hd + 10, &offset, 4);
    std::memcpy(hd + 14, &dib, 4);
    const int32_t sw = r.w, sh = -r.h;  // hauteur negative : lignes de haut en bas
    std::memcpy(hd + 18, &sw, 4);
    std::memcpy(hd + 22, &sh, 4);
    const uint16_t planes = 1, bpp = 24;
    std::memcpy(hd + 26, &planes, 2);
    std::memcpy(hd + 28, &bpp, 2);
    std::memcpy(hd + 34, &imgSize, 4);
    std::fwrite(hd, 1, 54, f);
    std::vector<uint8_t> row(stride, 0);
    for (int y = 0; y < r.h; ++y) {
        for (int x = 0; x < r.w; ++x) {
            const uint8_t* p = &rgb[(static_cast<size_t>(y) * r.w + x) * 3];
            row[static_cast<size_t>(x) * 3] = p[2];
            row[static_cast<size_t>(x) * 3 + 1] = p[1];
            row[static_cast<size_t>(x) * 3 + 2] = p[0];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    ++a.diagSaved;
    Logger::Get().Log(LogLevel::Info, "servo : capture de diagnostic %s (%s), ROI (%d,%d) %dx%d", name, tag, r.x, r.y, r.w, r.h);
}

sv::EyeView MakeView(const XrCompositionLayerProjectionView& v) {
    sv::EyeView ev;
    ev.pose = {{v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w}, {v.pose.position.x, v.pose.position.y, v.pose.position.z}};
    ev.fov = {v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown};
    ev.rect = {v.subImage.imageRect.offset.x, v.subImage.imageRect.offset.y, v.subImage.imageRect.extent.width, v.subImage.imageRect.extent.height};
    return ev;
}

sv::BrainHand MakeHand(const HandPoseData& h) {
    sv::BrainHand b;
    b.usable = h.valid && h.jointValid[kIndexTip] && h.jointValid[kDistal] && h.jointValid[kKnuckle];
    if (!b.usable) return b;
    b.pinch = h.pressed;
    b.knuckle = h.joint[kKnuckle].p;
    if (h.jointValid[kWrist] && h.jointValid[12] && h.jointValid[0]) {  // roulis du poignet (comme les rotatifs des zones)
        b.wristAxis = em::Normalized(h.joint[12].p - h.joint[kWrist].p);
        b.palmUp = em::Rotate(h.joint[0].q, em::Vec3{0.0f, 1.0f, 0.0f});
    }
    // bout reel du doigt : l'articulation du bout + son rayon dans le sens de la derniere phalange
    const em::Vec3 a = h.joint[kIndexTip].p, d = a - h.joint[kDistal].p;
    const float len = em::Length(d), r = h.radius[kIndexTip] > 0.001f ? h.radius[kIndexTip] : 0.006f;
    b.tip = len > 1e-4f ? a + d * (r * 0.8f / len) : a;
    b.extended = !h.jointValid[kWrist] || em::Length(h.joint[kIndexTip].p - h.joint[kWrist].p) - em::Length(h.joint[kKnuckle].p - h.joint[kWrist].p) > 0.045f;
    return b;
}

void Wire(Adapter& a) {
    if (a.wired) return;
    a.wired = true;
    a.brain.SetLogger([](int level, const std::string& m) { Logger::Get().Log(level == 0 ? LogLevel::Info : LogLevel::Warn, "%s", m.c_str()); });
    a.brain.SetDiag([](const char* tag, const uint8_t* rgba, const sv::BrainRoi& r, int dx, int dy, int fx, int fy) { SaveDiag(tag, rgba, r, dx, dy, fx, fy); });
}

}  // namespace

void Servo_Stop() {
    Adapter& a = A();
    if (a.reader) a.reader->WaitIdle();
    a.reader.reset();
    a.readerFailed = false;
    a.pendingActive = false;
    MouseInject_ReleaseIfHeld();
    a.brain.Reset();
}

void Servo_OnSwapchainDestroy() {
    Adapter& a = A();
    if (a.reader) a.reader->WaitIdle();
    a.pendingActive = false;
}

void Servo_Recalibrate() { A().brain.Recalibrate(); }

std::string Servo_StatusText() { return A().brain.StatusText(); }

void Servo_Frame(const XrCompositionLayerProjection* game, const HandsFrame& frame, bool menuOpen, ID3D12Device* device, ID3D12CommandQueue* queue) {
    Adapter& a = A();
    if (!game || !device || !queue) return;
    Wire(a);
    const int eye = Mouse_Eye();
    EyeImage img;
    const bool haveImg = Overlay_GetEyeImage(game, eye, &img);

    if (!a.reader && !a.readerFailed) {
        auto r = std::make_unique<DepthReader>();
        const char* err = nullptr;
        if (r->Init(device, queue, &err)) {
            a.reader = std::move(r);
        } else {
            a.readerFailed = true;
            Logger::Get().Log(LogLevel::Error, "servo : lecture de l'image du jeu impossible (%s)", err ? err : "?");
        }
    }

    // pixels de la requete precedente
    if (a.reader && a.pendingActive) {
        std::vector<DepthResultEye> res;
        if (a.reader->TryCollect(&res)) {
            a.pendingActive = false;
            if (!res.empty() && !res[0].boxes.empty() && res[0].boxes[0].size() == static_cast<size_t>(a.pending.w) * static_cast<size_t>(a.pending.h))
                a.brain.OnPixels(reinterpret_cast<const uint8_t*>(res[0].boxes[0].data()), a.pending);
        }
    }

    sv::BrainFrame in;
    in.nowMs = GetTickCount64();
    in.view = MakeView(game->views[eye]);
    in.menuOpen = menuOpen;
    for (int k = 0; k < 2; ++k) in.hand[k] = MakeHand(frame.hand[k]);
    in.readerFree = a.reader && haveImg && !a.pendingActive;
    a.brain.SetWheel(Mouse_WheelStepDeg(), Mouse_WheelInvert());
    const sv::BrainOutput out = a.brain.Frame(in);
    if (out.wheelSteps != 0) MouseInject_Wheel(out.wheelSteps);

    if (out.dx != 0 || out.dy != 0) MouseInject_Move(out.dx, out.dy);
    if (out.buttonEvent >= 0) MouseInject_Button(out.buttonEvent == 1);

    if (out.request.valid && a.reader && haveImg) {
        const sv::BrainRoi& r = out.request;
        std::vector<DepthRequestEye> q(1);
        q[0].depth = img.tex;
        q[0].state = img.state;
        q[0].format = DXGI_FORMAT_R8G8B8A8_UNORM;
        q[0].boxes.push_back({static_cast<uint32_t>(r.x), static_cast<uint32_t>(r.y), static_cast<uint32_t>(r.w), static_cast<uint32_t>(r.h)});
        if (a.reader->Submit(q)) {
            a.pending = r;
            a.pendingActive = true;
        }
    }
}

}  // namespace layer
