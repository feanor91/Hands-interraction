// ServoController.h - pointer servo core (portable, header-only, no Windows / OpenXR / D3D12). Proposed: core/servo/ServoController.h.
//
// Plant (what the layer controls): MSFS's VR pointer is an INTEGRATOR driven by relative mouse counts. A count sent at xrEndFrame call n shows up in
// the game image of call n+L (L = input->image latency, 1..3 frames); the readback of image m reaches the servo at call m+r (r = 1..2). So the
// measurement is always old news and a loop closed on it (naive proportional) is unstable as soon as L+r >= 2 with gain 1 (bound K < 2 sin(pi/(2(2d+1)))).
//
// Design (validated in sim/control_sim.py, see REPORT.md): an OBSERVER, not a Smith predictor.
//   x_hat(n) = x0 + bias + g * cum(n-1)          open-loop model: where the pointer WILL be once every command already sent has landed
//   on each measurement of image m:  nu = z_m - (x0 + bias + g * cum(m-L));  bias += kBias * clip(nu)
//   command  c(n) = kControl * (target_ahead - x_hat) / g          (no delay inside this loop; the delayed part is only the slow bias loop)
// A wrong L (+-1 frame) or gain (+-25 %) degrades tracking a little but never destabilises it, unlike the textbook Smith predictor.
// All angles are (yaw, pitch) in radians in GAME axes (see ServoMath.h AnglesOf): yaw grows right, pitch grows up.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

namespace hands::servo {

struct V2 {
    double a = 0, b = 0;  // (yaw, pitch)
};
inline V2 operator+(V2 x, V2 y) { return {x.a + y.a, x.b + y.b}; }
inline V2 operator-(V2 x, V2 y) { return {x.a - y.a, x.b - y.b}; }
inline V2 operator*(V2 x, double s) { return {x.a * s, x.b * s}; }
inline double Norm(V2 x) { return std::sqrt(x.a * x.a + x.b * x.b); }
// Difference of two (yaw, pitch) directions with the yaw difference wrapped to (-pi, pi]: yaw is an angle, so near +-180 deg (looking backwards, or next to the pole) two directions
// 0.06 rad apart differ by ~6.2 rad (the 6000 px "error" and the -150 / +150 counts for 3.7 s of the 12 Nov. trace).
inline V2 Diff(V2 x, V2 y) {
    V2 d{x.a - y.a, x.b - y.b};
    while (d.a > 3.14159265358979) d.a -= 6.28318530717959;
    while (d.a < -3.14159265358979) d.a += 6.28318530717959;
    return d;
}

// Local geometry of the plant, given by the caller each frame: MSFS moves its pointer by a constant number of IMAGE pixels per mouse count (measured 1.05 px/count from
// 20 to 80 deg below the horizon), so the angle (yaw, pitch) change per count depends on where the pointer is and where the head looks (1/cos(pitch) in yaw: x2.8 at -69 deg,
// the reason for the "dance" when looking down). [dYaw; dPitch] = [a b; c d] * [dx; dy] counts. gx, gy = pixels per count, pxPerRad converts them to radians.
struct Jac {
    double a = 0, b = 0, c = 0, d = 0;
    double gx = 1, gy = 1, pxPerRad = 1215.0;
    bool valid = false;
    // EXACT image-plane quantities (the linear J above is only a fallback): near the pole (finger at -75 deg of site) one pixel is many degrees of yaw, so an error taken as a
    // difference of angles and mapped back through J blows up (essai du 12 nov. : 150 counts / frame en sens inverse pendant 3.7 s). The caller projects the fingertip and the
    // model pointer into the CURRENT image and gives their pixel difference; the plant is linear in those pixels (constant px per count).
    bool pxValid = false;
    double ex = 0, ey = 0;                   // fingertip - model pointer, pixels (y down)
    double vx = 0, vy = 0;                   // fingertip displacement since the previous call, pixels, measured in the same view (head motion cancels)
    std::function<V2(double, double)> inc;   // exact (yaw, pitch) increment of the model pointer for (dx, dy) counts sent now
};

struct ServoParams {
    // Plant gain prior: radians per mouse count. mouse.ini fit: px = 1280.7 + 1193.7*yaw (1 OS pixel = 1/1193.7 rad) - valid if MSFS sees 1 count = 1 px.
    // dy > 0 moves the pointer DOWN, i.e. decreases pitch. Calibrate / refine with the calibration pulse.
    double radPerCountX = 1.0 / 1193.7;
    double radPerCountY = -1.0 / 1193.7;
    int latencyFrames = 2;      // L, calibrated at start (pulse) and logged
    double kControl = 0.8;      // share of the predicted error corrected per frame (0.5..1)
    double kBias = 0.5;         // innovation gain per measurement (0.1..0.6): the detector is now selective (black-outline test), so measurements can be trusted more ;
                                // large = follows a pointer that drifts (head-anchored, snapping to controls), small = robust to bad L and to detection noise
    double lead = 1.3;          // share of the extrapolated fingertip motion fed forward (0 = none)
    double velSmooth = 0.4;     // fingertip velocity filter (per frame)
    double ffSpeedLo = 0.0012, ffSpeedHi = 0.0030;  // rad/frame : feed-forward gate (0 below Lo, full above Hi)
    double leadClip = 0.08;     // max feed-forward (rad)
    double innovClip = 0.08;    // max |innovation| absorbed per measurement (rad ~ 4.6 deg): outlier / false detection rejection
    int maxCounts = 60;         // per axis and frame (also what SendInput gets): 60 counts at 90 fps already sweep ~4 rad/s, far above a hand
    double deadband = 0.0015;   // rad: once correcting, an axis keeps correcting until |error| falls below this (~1.8 px) ...
    double deadStart = 0.005;  // ... and an idle axis only starts correcting above this (~6 px) : hysteresis, so hand-tracking tremor at rest sends nothing at all (no "dance")
    double satInnov = 0.03;     // |innovation| (rad) that counts towards a wall detection ...
    int satStreak = 5;          // ... when seen this many measurements in a row with the same sign
    int satHoldFrames = 90;     // after a wall is detected: no pushing into it for this many frames (3 s at 29 fps) ...
    double satReleaseRad = 0.05; // ... unless the target moves by more than this (rad, ~60 px) : a different goal deserves a new attempt
    int jumpStreak = 3;          // a LARGE innovation (> innovClip) seen this many measurements in a row with the same value (a real jump of the pointer, e.g. MSFS
                                 // re-anchoring it into the field of view after a head turn) re-anchors the model at once instead of waiting for the slow bias loop
    double jumpSameRad = 0.02;   // "same value": consecutive large innovations closer than this
};

class ServoController {
public:
    struct Out {
        int dx = 0, dy = 0;     // counts to inject now (relative mouse move)
        V2 predicted;           // x_hat: pointer direction once everything sent has landed
        V2 error;               // target_ahead - x_hat
        double errUnits = 0;    // |error| in the control chart: pixels when a Jac was given, radians otherwise
        bool haveModel = false; // false until the first measurement: nothing is sent (caller may run its acquisition / wake-up logic)
        bool saturated = false; // a wall was detected on this frame
    };

    explicit ServoController(const ServoParams& p = ServoParams()) : p_(p) {}
    void SetParams(const ServoParams& p) { p_ = p; }
    void SetMaxCounts(int n) { p_.maxCounts = n; }
    void ResetTarget() { haveT_ = false; haveVpx_ = false; }  // the target jumped (hand switch, lock / unlock): forget its velocity, no feed-forward spike
    const ServoParams& Params() const { return p_; }
    void Reset() { *this = ServoController(p_); }

    // Call BEFORE Step() of the call that received the readback. imageSeq = call index of the image the pointer was detected in.
    void OnMeasurement(uint64_t imageSeq, V2 ptr) {
        const int64_t im = static_cast<int64_t>(imageSeq);
        if (started_ && im + static_cast<int64_t>(kHist) <= lastSeq_) return;  // older than the history: ignore
        const int64_t k = im - p_.latencyFrames;
        const Cnt vis = CumAt(k);
        const V2 gv{vis.a, vis.b};  // what our commands did, per the model (angle increments accumulated per frame)
        if (!haveX0_) {
            x0_ = ptr - gv;
            bias_ = {};
            haveX0_ = true;
            lastPtr_ = ptr;
            return;
        }
        const V2 nu = Diff(ptr, x0_ + bias_ + gv);
        lastInnov_ = nu;
        ++nMeas_;
        const double dgv = Norm(gv - lastGv_);  // how much the model itself moved since the previous measurement
        lastGv_ = gv;
        if (Norm(nu) > p_.innovClip && dgv < kStuckEps) {  // large, constant offset while the model is NOT moving = the pointer jumped (not a wall, not a false detection)
            if (jumpN_ > 0 && Norm(nu - lastBig_) < p_.jumpSameRad) ++jumpN_;
            else jumpN_ = 1;
            lastBig_ = nu;
            if (jumpN_ >= p_.jumpStreak) {
                x0_ = ptr - gv;
                bias_ = {};
                jumpN_ = 0;
                streak_[0] = streak_[1] = 0;
                lastPtr_ = ptr;
                ++nJumps_;
                return;
            }
        } else {
            jumpN_ = 0;
        }
        const double nus[2] = {nu.a, nu.b}, pt[2] = {ptr.a, ptr.b}, lp[2] = {lastPtr_.a, lastPtr_.b}, gvs[2] = {gv.a, gv.b};
        double* x0s[2] = {&x0_.a, &x0_.b};
        double* bs[2] = {&bias_.a, &bias_.b};
        for (int ax = 0; ax < 2; ++ax) {
            const double n = nus[ax];
            const int sgn = n > p_.satInnov ? 1 : (n < -p_.satInnov ? -1 : 0);
            const bool stuck = std::fabs(ax == 0 ? Diff(ptr, lastPtr_).a : pt[ax] - lp[ax]) < kStuckEps;  // a wall = the model says "moving", the pointer does not move
            if (sgn != 0 && stuck) streak_[ax] = (sgn == lastSign_[ax]) ? streak_[ax] + 1 : 1;
            else streak_[ax] = 0;
            lastSign_[ax] = (sgn != 0 && stuck) ? sgn : 0;
            if (streak_[ax] >= p_.satStreak) {  // re-anchor the model on the measurement and stop pushing into the wall for a while
                *x0s[ax] = pt[ax] - gvs[ax];
                *bs[ax] = 0;
                blockSign_[ax] = -sgn;  // innovation < 0 (pointer behind the model) = we were pushing in the + direction
                blockUntil_[ax] = lastSeq_ + p_.satHoldFrames;
                blockTarget_[ax] = ax == 0 ? tPrev_.a : tPrev_.b;
                streak_[ax] = 0;
                satFlag_ = true;
                continue;
            }
            *bs[ax] += p_.kBias * std::max(-p_.innovClip, std::min(p_.innovClip, n));
        }
        lastPtr_ = ptr;
    }

    // One call per xrEndFrame (seq increases by 1 per call; gaps are filled with "no command"). target = fingertip direction (rad, game axes).
    // extraDx / extraDy : counts the caller adds on top (keep-alive jiggle) ; they are part of the model like any other count.
    Out Step(uint64_t seq, bool targetValid, V2 target, int extraDx = 0, int extraDy = 0, const Jac* jac = nullptr) {
        Out o;
        const int64_t sq = static_cast<int64_t>(seq);
        if (!started_) {
            started_ = true;
            firstSeq_ = sq;
            lastSeq_ = sq - 1;  // cum(seq-1) = 0
            cumA_[Slot(sq - 1)] = cumB_[Slot(sq - 1)] = 0;
        }
        if (sq <= lastSeq_) {  // duplicate call: repeat nothing
            o.haveModel = haveX0_;
            return o;
        }
        for (int64_t s = lastSeq_ + 1; s < sq; ++s) {  // skipped calls: no command
            cumA_[Slot(s)] = cumA_[Slot(s - 1)];
            cumB_[Slot(s)] = cumB_[Slot(s - 1)];
        }
        lastSeq_ = sq;
        const Cnt prev = {cumA_[Slot(sq - 1)], cumB_[Slot(sq - 1)]};
        Jac J;
        if (jac && jac->valid && std::fabs(jac->a * jac->d - jac->b * jac->c) > 1e-12) {
            J = *jac;
        } else {  // legacy: axis-aligned, constant gain (the angle chart IS the control chart)
            J.a = p_.radPerCountX;
            J.d = p_.radPerCountY;
            J.b = J.c = 0;
            J.gx = std::fabs(p_.radPerCountX);
            J.gy = std::fabs(p_.radPerCountY);
            J.pxPerRad = 1.0;
            J.valid = true;
        }
        const double det = J.a * J.d - J.b * J.c;
        const auto toCounts = [&](V2 v) { return V2{(J.d * v.a - J.b * v.b) / det, (-J.c * v.a + J.a * v.b) / det}; };  // angle vector -> counts
        int dx = 0, dy = 0;
        o.haveModel = haveX0_;
        o.saturated = satFlag_;
        satFlag_ = false;
        if (haveX0_) {
            const V2 xh = x0_ + bias_ + V2{prev.a, prev.b};
            o.predicted = xh;
            if (targetValid) {
                if (!haveT_) {
                    tPrev_ = target;
                    vT_ = {};
                    haveT_ = true;
                }
                vT_ = vT_ * (1.0 - p_.velSmooth) + Diff(target, tPrev_) * p_.velSmooth;
                tPrev_ = target;
                const bool pxPath = J.pxValid && J.gx > 1e-9 && J.gy > 1e-9;
                if (pxPath) {
                    if (!haveVpx_) {
                        vPxX_ = vPxY_ = 0;
                        haveVpx_ = true;
                    } else {
                        vPxX_ = vPxX_ * (1.0 - p_.velSmooth) + J.vx * p_.velSmooth;
                        vPxY_ = vPxY_ * (1.0 - p_.velSmooth) + J.vy * p_.velSmooth;
                    }
                } else {
                    haveVpx_ = false;
                }
                // everything below happens in COUNTS space (and in pixels through gx, gy): the same pixel error must give the same correction wherever the pointer is
                const V2 vc = pxPath ? V2{vPxX_ / J.gx, vPxY_ / J.gy} : toCounts(vT_);  // fingertip velocity in counts per frame
                const double spd = std::hypot(vc.a * J.gx, vc.b * J.gy) / J.pxPerRad;
                // feed-forward only once the fingertip is really moving : at rest the velocity estimate is mostly hand-tracking noise
                const double ffGate = std::max(0.0, std::min(1.0, (spd - p_.ffSpeedLo) / (p_.ffSpeedHi - p_.ffSpeedLo)));
                V2 fc = vc * (p_.lead * p_.latencyFrames * ffGate);
                {
                    const double kx = fc.a * J.gx / J.pxPerRad, ky = fc.b * J.gy / J.pxPerRad;
                    const double sc = std::max(std::fabs(kx), std::fabs(ky)) > p_.leadClip ? p_.leadClip / std::max(std::fabs(kx), std::fabs(ky)) : 1.0;
                    fc = fc * sc;
                }
                V2 ec = pxPath ? V2{J.ex / J.gx, J.ey / J.gy} + fc : toCounts(Diff(target, xh)) + fc;  // error in counts (feed-forward included)
                o.error = V2{J.a * ec.a + J.b * ec.b, J.c * ec.a + J.d * ec.b};
                o.errUnits = std::hypot(ec.a * J.gx, ec.b * J.gy);
                {
                    double* e[2] = {&ec.a, &ec.b};
                    const double gq[2] = {J.gx, J.gy};
                    for (int ax = 0; ax < 2; ++ax) {
                        const double m = std::fabs(*e[ax]) * gq[ax] / J.pxPerRad;  // |error| in the control chart (rad-equivalent)
                        if (active_[ax]) active_[ax] = m >= p_.deadband;
                        else active_[ax] = m > p_.deadStart;
                        if (!active_[ax]) {
                            *e[ax] = 0;
                            rem_[ax] = 0;
                        }
                    }
                }
                const double tg[2] = {target.a, target.b};
                for (int ax = 0; ax < 2; ++ax)
                    if (blockUntil_[ax] > sq && std::fabs(ax == 0 ? Diff({tg[0], 0}, {blockTarget_[0], 0}).a : tg[ax] - blockTarget_[ax]) > p_.satReleaseRad) blockUntil_[ax] = 0;
                const double want[2] = {p_.kControl * ec.a, p_.kControl * ec.b};
                int out[2] = {0, 0};
                for (int ax = 0; ax < 2; ++ax) {
                    const double c = want[ax] + rem_[ax];
                    int ci = static_cast<int>(std::lround(c));
                    const bool clamped = std::abs(ci) > p_.maxCounts;
                    ci = std::max(-p_.maxCounts, std::min(p_.maxCounts, ci));
                    rem_[ax] = clamped ? 0.0 : c - ci;
                    if (blockUntil_[ax] > sq && ((ci > 0 && blockSign_[ax] > 0) || (ci < 0 && blockSign_[ax] < 0))) {  // do not push into a detected wall
                        ci = 0;
                        rem_[ax] = 0;
                    }
                    out[ax] = ci;
                }
                dx = out[0];
                dy = out[1];
            } else {
                haveT_ = false;  // hand lost: no velocity memory, no command
                haveVpx_ = false;
            }
        }
        dx += extraDx;
        dy += extraDy;
        if (J.pxValid && J.inc && (dx != 0 || dy != 0)) {  // exact increment (near the pole the linear J is off by a lot for a 100-count step)
            const V2 d = J.inc(dx, dy);
            cumA_[Slot(sq)] = prev.a + d.a;
            cumB_[Slot(sq)] = prev.b + d.b;
        } else {
            cumA_[Slot(sq)] = prev.a + J.a * dx + J.b * dy;  // angle increments caused by this frame's counts (incl. the caller's extra counts)
            cumB_[Slot(sq)] = prev.b + J.c * dx + J.d * dy;
        }
        o.dx = dx;
        o.dy = dy;
        return o;
    }

    // Trust a measurement completely (a strong detection after the pointer was lost for a while): x_hat becomes exactly this measurement, no clipping, no filtering.
    void Reanchor(uint64_t imageSeq, V2 ptr) {
        const int64_t k = static_cast<int64_t>(imageSeq) - p_.latencyFrames;
        const Cnt vis = CumAt(k);
        x0_ = ptr - V2{vis.a, vis.b};
        bias_ = {};
        haveX0_ = true;
        lastPtr_ = ptr;
        jumpN_ = 0;
        streak_[0] = streak_[1] = 0;
        ++nJumps_;
    }

    // Where the model says the pointer is in the image of call `imageSeq` (what has landed there: commands of calls <= imageSeq - L). Used to centre the
    // readback region of interest. Meaningful once HaveModel().
    V2 PredictedInImage(uint64_t imageSeq) const {
        const Cnt vis = CumAt(static_cast<int64_t>(imageSeq) - p_.latencyFrames);
        return x0_ + bias_ + V2{vis.a, vis.b};
    }

    // Where the model says the pointer will be once every command already sent has landed (x_hat of the NEXT Step), for the caller's Jacobian.
    V2 PredictedNow() const {
        if (!started_) return x0_ + bias_;
        return x0_ + bias_ + V2{cumA_[Slot(lastSeq_)], cumB_[Slot(lastSeq_)]};
    }

    // Diagnostics
    int Jumps() const { return nJumps_; }
    V2 LastInnovation() const { return lastInnov_; }
    V2 Bias() const { return bias_; }
    int Measurements() const { return nMeas_; }
    bool HaveModel() const { return haveX0_; }

private:
    static constexpr uint64_t kHist = 64;
    static constexpr double kStuckEps = 0.004;  // rad: pointer displacement between two measurements below which it counts as stationary
    struct Cnt {
        double a = 0, b = 0;  // accumulated angle increments (rad)
    };
    static size_t Slot(int64_t s) { return static_cast<size_t>(((s % static_cast<int64_t>(kHist)) + static_cast<int64_t>(kHist)) % static_cast<int64_t>(kHist)); }
    Cnt CumAt(int64_t k) const {  // sum of the counts issued at calls <= k
        if (!started_ || k < firstSeq_) return {};
        if (k > lastSeq_) k = lastSeq_;
        if (k + static_cast<int64_t>(kHist) <= lastSeq_) return {};  // too old
        return {cumA_[Slot(k)], cumB_[Slot(k)]};
    }

    ServoParams p_;
    bool started_ = false, haveX0_ = false, haveT_ = false, satFlag_ = false, haveVpx_ = false;
    double vPxX_ = 0, vPxY_ = 0;
    int64_t firstSeq_ = 0, lastSeq_ = -1;
    double cumA_[kHist] = {}, cumB_[kHist] = {};
    V2 x0_, bias_, lastInnov_, tPrev_, vT_, lastPtr_;
    double rem_[2] = {0, 0};
    bool active_[2] = {false, false};  // axis currently correcting (deadband hysteresis)
    int streak_[2] = {0, 0}, lastSign_[2] = {0, 0}, blockSign_[2] = {0, 0}, nMeas_ = 0, jumpN_ = 0, nJumps_ = 0;
    V2 lastBig_, lastGv_;
    int64_t blockUntil_[2] = {0, 0};
    double blockTarget_[2] = {0, 0};
};

}  // namespace hands::servo
