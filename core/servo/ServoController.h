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

namespace hands::servo {

struct V2 {
    double a = 0, b = 0;  // (yaw, pitch)
};
inline V2 operator+(V2 x, V2 y) { return {x.a + y.a, x.b + y.b}; }
inline V2 operator-(V2 x, V2 y) { return {x.a - y.a, x.b - y.b}; }
inline V2 operator*(V2 x, double s) { return {x.a * s, x.b * s}; }
inline double Norm(V2 x) { return std::sqrt(x.a * x.a + x.b * x.b); }

struct ServoParams {
    // Plant gain prior: radians per mouse count. mouse.ini fit: px = 1280.7 + 1193.7*yaw (1 OS pixel = 1/1193.7 rad) - valid if MSFS sees 1 count = 1 px.
    // dy > 0 moves the pointer DOWN, i.e. decreases pitch. Calibrate / refine with the calibration pulse.
    double radPerCountX = 1.0 / 1193.7;
    double radPerCountY = -1.0 / 1193.7;
    int latencyFrames = 2;      // L, calibrated at start (pulse) and logged
    double kControl = 0.8;      // share of the predicted error corrected per frame (0.5..1)
    double kBias = 0.25;        // innovation gain per measurement (0.1..0.3); small = robust to bad L, slow to absorb gain errors
    double lead = 0.7;          // share of the extrapolated fingertip motion fed forward (0 = none)
    double velSmooth = 0.4;     // fingertip velocity filter (per frame)
    double leadClip = 0.08;     // max feed-forward (rad)
    double innovClip = 0.035;   // max |innovation| absorbed per measurement (rad ~ 2 deg): outlier / false detection rejection
    int maxCounts = 60;         // per axis and frame (also what SendInput gets): 60 counts at 90 fps already sweep ~4 rad/s, far above a hand
    double deadband = 0.0010;   // rad: |error| below this sends nothing (~1.2 px: stops count chatter at rest)
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
        bool haveModel = false; // false until the first measurement: nothing is sent (caller may run its acquisition / wake-up logic)
        bool saturated = false; // a wall was detected on this frame
    };

    explicit ServoController(const ServoParams& p = ServoParams()) : p_(p) {}
    void SetParams(const ServoParams& p) { p_ = p; }
    const ServoParams& Params() const { return p_; }
    void Reset() { *this = ServoController(p_); }

    // Call BEFORE Step() of the call that received the readback. imageSeq = call index of the image the pointer was detected in.
    void OnMeasurement(uint64_t imageSeq, V2 ptr) {
        const int64_t im = static_cast<int64_t>(imageSeq);
        if (started_ && im + static_cast<int64_t>(kHist) <= lastSeq_) return;  // older than the history: ignore
        const int64_t k = im - p_.latencyFrames;
        const Cnt vis = CumAt(k);
        const V2 gv{p_.radPerCountX * static_cast<double>(vis.a), p_.radPerCountY * static_cast<double>(vis.b)};  // what our commands did, per the model
        if (!haveX0_) {
            x0_ = ptr - gv;
            bias_ = {};
            haveX0_ = true;
            lastPtr_ = ptr;
            return;
        }
        const V2 nu = ptr - (x0_ + bias_ + gv);
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
            const bool stuck = std::fabs(pt[ax] - lp[ax]) < kStuckEps;  // a wall = the model says "moving", the pointer does not move
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
    Out Step(uint64_t seq, bool targetValid, V2 target, int extraDx = 0, int extraDy = 0) {
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
        int dx = 0, dy = 0;
        o.haveModel = haveX0_;
        o.saturated = satFlag_;
        satFlag_ = false;
        if (haveX0_) {
            const V2 xh = x0_ + bias_ + V2{p_.radPerCountX * static_cast<double>(prev.a), p_.radPerCountY * static_cast<double>(prev.b)};
            o.predicted = xh;
            if (targetValid) {
                if (!haveT_) {
                    tPrev_ = target;
                    vT_ = {};
                    haveT_ = true;
                }
                vT_ = vT_ * (1.0 - p_.velSmooth) + (target - tPrev_) * p_.velSmooth;
                tPrev_ = target;
                V2 ff = vT_ * (p_.lead * p_.latencyFrames);
                ff.a = std::max(-p_.leadClip, std::min(p_.leadClip, ff.a));
                ff.b = std::max(-p_.leadClip, std::min(p_.leadClip, ff.b));
                V2 err = (target + ff) - xh;
                o.error = err;
                if (std::fabs(err.a) < p_.deadband) err.a = 0;
                if (std::fabs(err.b) < p_.deadband) err.b = 0;
                const double tg[2] = {target.a, target.b};
                for (int ax = 0; ax < 2; ++ax)
                    if (blockUntil_[ax] > sq && std::fabs(tg[ax] - blockTarget_[ax]) > p_.satReleaseRad) blockUntil_[ax] = 0;
                const double want[2] = {p_.kControl * err.a / p_.radPerCountX, p_.kControl * err.b / p_.radPerCountY};
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
            }
        }
        dx += extraDx;
        dy += extraDy;
        cumA_[Slot(sq)] = prev.a + dx;
        cumB_[Slot(sq)] = prev.b + dy;
        o.dx = dx;
        o.dy = dy;
        return o;
    }

    // Where the model says the pointer is in the image of call `imageSeq` (what has landed there: commands of calls <= imageSeq - L). Used to centre the
    // readback region of interest. Meaningful once HaveModel().
    V2 PredictedInImage(uint64_t imageSeq) const {
        const Cnt vis = CumAt(static_cast<int64_t>(imageSeq) - p_.latencyFrames);
        return x0_ + bias_ + V2{p_.radPerCountX * static_cast<double>(vis.a), p_.radPerCountY * static_cast<double>(vis.b)};
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
        int64_t a = 0, b = 0;
    };
    static size_t Slot(int64_t s) { return static_cast<size_t>(((s % static_cast<int64_t>(kHist)) + static_cast<int64_t>(kHist)) % static_cast<int64_t>(kHist)); }
    Cnt CumAt(int64_t k) const {  // sum of the counts issued at calls <= k
        if (!started_ || k < firstSeq_) return {};
        if (k > lastSeq_) k = lastSeq_;
        if (k + static_cast<int64_t>(kHist) <= lastSeq_) return {};  // too old
        return {cumA_[Slot(k)], cumB_[Slot(k)]};
    }

    ServoParams p_;
    bool started_ = false, haveX0_ = false, haveT_ = false, satFlag_ = false;
    int64_t firstSeq_ = 0, lastSeq_ = -1;
    int64_t cumA_[kHist] = {}, cumB_[kHist] = {};
    V2 x0_, bias_, lastInnov_, tPrev_, vT_, lastPtr_;
    double rem_[2] = {0, 0};
    int streak_[2] = {0, 0}, lastSign_[2] = {0, 0}, blockSign_[2] = {0, 0}, nMeas_ = 0, jumpN_ = 0, nJumps_ = 0;
    V2 lastBig_, lastGv_;
    int64_t blockUntil_[2] = {0, 0};
    double blockTarget_[2] = {0, 0};
};

}  // namespace hands::servo
