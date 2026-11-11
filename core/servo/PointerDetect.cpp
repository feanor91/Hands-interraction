// PointerDetect.cpp — voir PointerDetect.h.
#include "servo/PointerDetect.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace hands::servo {

namespace {

struct Region {
    std::vector<int> px;  // indices y * w + x
    int level = 0;
};

struct Workspace {
    std::vector<uint8_t> white;      // min(R,G,B)
    std::vector<uint8_t> value;      // max(R,G,B)
    std::vector<uint32_t> visit;     // etiquette de visite (generation)
    std::vector<uint8_t> accepted;
    std::vector<int> stack;
    uint32_t gen = 0;
};

double Ramp(double v, double lo, double hi) { return v <= lo ? 0.0 : (v >= hi ? 1.0 : (v - lo) / (hi - lo)); }

double Trap(double v, double a, double b, double c, double d) {
    if (v <= a || v >= d) return 0.0;
    if (v < b) return (v - a) / (b - a);
    if (v <= c) return 1.0;
    return (d - v) / (d - c);
}

struct P2 {
    long long x, y;
};

long long Cross(const P2& o, const P2& a, const P2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// Aire de l'enveloppe convexe de l'union des carres unite des pixels (chaine monotone d'Andrew sur les coins des pixels).
double HullArea(std::vector<P2>& pts) {
    std::sort(pts.begin(), pts.end(), [](const P2& a, const P2& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    pts.erase(std::unique(pts.begin(), pts.end(), [](const P2& a, const P2& b) { return a.x == b.x && a.y == b.y; }), pts.end());
    const size_t n = pts.size();
    if (n < 3) return static_cast<double>(n);
    std::vector<P2> hull(2 * n);
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        while (k >= 2 && Cross(hull[k - 2], hull[k - 1], pts[i]) <= 0) --k;
        hull[k++] = pts[i];
    }
    for (size_t i = n - 1, t = k + 1; i > 0; --i) {
        while (k >= t && Cross(hull[k - 2], hull[k - 1], pts[i - 1]) <= 0) --k;
        hull[k++] = pts[i - 1];
    }
    hull.resize(k - 1);
    double a = 0;
    for (size_t i = 0; i < hull.size(); ++i) {
        const P2& p = hull[i];
        const P2& q = hull[(i + 1) % hull.size()];
        a += static_cast<double>(p.x * q.y - q.x * p.y);
    }
    return std::fabs(a) / 2.0;
}

// Flood fill 4-connexe de W >= level depuis (sx, sy). Retourne faux si la region depasse aMax (le remplissage continue sans stocker pour marquer
// tous les pixels de la region trop grande comme visites).
bool Flood4(Workspace& ws, int w, int h, int sx, int sy, int level, int aMax, std::vector<int>* out, std::vector<int>* all = nullptr) {
    out->clear();
    if (all) all->clear();
    ws.stack.clear();
    const int s0 = sy * w + sx;
    ws.visit[static_cast<size_t>(s0)] = ws.gen;
    ws.stack.push_back(s0);
    bool tooBig = false;
    int count = 0;
    while (!ws.stack.empty()) {
        const int i = ws.stack.back();
        ws.stack.pop_back();
        ++count;
        if (all) all->push_back(i);
        if (!tooBig) {
            out->push_back(i);
            if (count > aMax) {
                tooBig = true;
                out->clear();
            }
        }
        const int x = i % w, y = i / w;
        const int nb[4] = {x > 0 ? i - 1 : -1, x + 1 < w ? i + 1 : -1, y > 0 ? i - w : -1, y + 1 < h ? i + w : -1};
        for (int k = 0; k < 4; ++k) {
            const int j = nb[k];
            if (j < 0 || ws.visit[static_cast<size_t>(j)] == ws.gen || ws.white[static_cast<size_t>(j)] < level) continue;
            ws.visit[static_cast<size_t>(j)] = ws.gen;
            ws.stack.push_back(j);
        }
    }
    return !tooBig;
}

void Dilate8(std::vector<uint8_t>& m, int w, int h, std::vector<uint8_t>& tmp) {
    tmp = m;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!m[static_cast<size_t>(y * w + x)]) continue;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && yy >= 0 && xx < w && yy < h) tmp[static_cast<size_t>(yy * w + xx)] = 1;
                }
        }
    m.swap(tmp);
}

}  // namespace

int DetectPointer(const uint8_t* rgba, int pitchBytes, int w, int h, const DetectParams& P, const DetectPrior* prior, PointerCandidate* out, int maxOut) {
    if (!rgba || w <= 0 || h <= 0 || maxOut <= 0) return 0;
    thread_local Workspace ws;
    const size_t npx = static_cast<size_t>(w) * static_cast<size_t>(h);
    ws.white.resize(npx);
    ws.value.resize(npx);
    bool anySeed = false;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = rgba + static_cast<size_t>(y) * static_cast<size_t>(pitchBytes);
        uint8_t* wr = &ws.white[static_cast<size_t>(y) * static_cast<size_t>(w)];
        uint8_t* vr = &ws.value[static_cast<size_t>(y) * static_cast<size_t>(w)];
        for (int x = 0; x < w; ++x) {
            const uint8_t r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2];
            const uint8_t mn = std::min(r, std::min(g, b)), mx = std::max(r, std::max(g, b));
            wr[x] = mn;
            vr[x] = mx;
            if (mn >= P.tSeed) anySeed = true;
        }
    }
    if (!anySeed) return 0;  // cas le plus frequent : rien de blanc pur dans la ROI

    ws.visit.assign(npx, 0);
    ws.accepted.assign(npx, 0);
    ws.gen = 0;

    // Niveaux : tFringe, puis 200 / 232 / tSeed (ceux qui sont dans ]tFringe, tSeed]).
    int levels[4];
    int nLevels = 0;
    levels[nLevels++] = P.tFringe;
    for (int t : {200, 232, P.tSeed})
        if (t > P.tFringe && t <= P.tSeed && (nLevels == 0 || t > levels[nLevels - 1])) levels[nLevels++] = t;

    std::vector<Region> regions;
    std::vector<int> tmpPix, allPix;
    for (int y = 0; y < h && static_cast<int>(regions.size()) < P.maxRegions; ++y)
        for (int x = 0; x < w && static_cast<int>(regions.size()) < P.maxRegions; ++x) {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
            if (ws.white[i] < P.tSeed || ws.accepted[i]) continue;
            for (int li = 0; li < nLevels; ++li) {
                ++ws.gen;
                const bool last = li == nLevels - 1;
                if (Flood4(ws, w, h, x, y, levels[li], P.aMax, &tmpPix, last ? &allPix : nullptr)) {
                    Region r;
                    r.px = tmpPix;
                    r.level = levels[li];
                    for (int idx : r.px) ws.accepted[static_cast<size_t>(idx)] = 1;
                    regions.push_back(std::move(r));
                    break;
                }
                // trop grande meme au niveau le plus haut : panneau blanc ; on marque toute la zone pour ne pas la reparcourir a chaque graine
                if (last)
                    for (int idx : allPix) ws.accepted[static_cast<size_t>(idx)] = 2;
            }
        }

    // centroides de toutes les regions (pour l'isolement)
    std::vector<std::pair<double, double>> cents;
    for (const Region& r : regions) {
        if (r.px.size() < 3) continue;
        double sx = 0, sy = 0;
        for (int idx : r.px) {
            sx += idx % w;
            sy += idx / w;
        }
        cents.push_back({sx / static_cast<double>(r.px.size()), sy / static_cast<double>(r.px.size())});
    }

    std::vector<PointerCandidate> cands;
    std::vector<uint8_t> mask, ring, dk, cur, tmp;
    for (const Region& r : regions) {
        const int n = static_cast<int>(r.px.size());
        if (n < P.aMin) continue;
        int bx0 = w, bx1 = -1, by0 = h, by1 = -1;
        double sx = 0, sy = 0;
        int peak = 0, nSat = 0;
        for (int idx : r.px) {
            const int x = idx % w, y = idx / w;
            bx0 = std::min(bx0, x);
            bx1 = std::max(bx1, x);
            by0 = std::min(by0, y);
            by1 = std::max(by1, y);
            sx += x;
            sy += y;
            const int wv = ws.white[static_cast<size_t>(idx)];
            peak = std::max(peak, wv);
            if (wv >= P.tSat) ++nSat;
        }
        const int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
        if (std::max(bw, bh) < P.sideMin || std::max(bw, bh) > P.sideMax) continue;
        const bool clipped = bx0 == 0 || by0 == 0 || bx1 == w - 1 || by1 == h - 1;
        if (clipped && !P.allowClipped) continue;
        const double satFrac = static_cast<double>(nSat) / n;

        // masque local (marge pad), halo
        const int pad = P.haloRadius + 1;
        const int pw = bw + 2 * pad, ph = bh + 2 * pad;
        mask.assign(static_cast<size_t>(pw) * static_cast<size_t>(ph), 0);
        for (int idx : r.px) mask[static_cast<size_t>((idx / w - by0 + pad) * pw + (idx % w - bx0 + pad))] = 1;
        // bord : pixel de la region dont un voisin 4 n'est pas dans la region
        std::vector<P2> corners;
        int nBoundary = 0;
        std::vector<uint8_t> boundary(mask.size(), 0);
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x) {
                if (!mask[static_cast<size_t>(y * pw + x)]) continue;
                const bool inner = x > 0 && y > 0 && x + 1 < pw && y + 1 < ph && mask[static_cast<size_t>(y * pw + x - 1)] && mask[static_cast<size_t>(y * pw + x + 1)] &&
                                   mask[static_cast<size_t>((y - 1) * pw + x)] && mask[static_cast<size_t>((y + 1) * pw + x)];
                if (!inner) {
                    boundary[static_cast<size_t>(y * pw + x)] = 1;
                    ++nBoundary;
                    const long long gx = x - pad + bx0, gy = y - pad + by0;
                    corners.push_back({gx, gy});
                    corners.push_back({gx + 1, gy});
                    corners.push_back({gx, gy + 1});
                    corners.push_back({gx + 1, gy + 1});
                }
            }
        // pixels sombres hors region (valides = dans la ROI)
        dk.assign(mask.size(), 0);
        std::vector<uint8_t> valid(mask.size(), 0);
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x) {
                const int gx = x - pad + bx0, gy = y - pad + by0;
                if (gx < 0 || gy < 0 || gx >= w || gy >= h || mask[static_cast<size_t>(y * pw + x)]) continue;
                valid[static_cast<size_t>(y * pw + x)] = 1;
                if (ws.value[static_cast<size_t>(gy) * static_cast<size_t>(w) + static_cast<size_t>(gx)] <= P.haloDark) dk[static_cast<size_t>(y * pw + x)] = 1;
            }
        cur = dk;
        ring = mask;
        for (int k = 0; k < P.haloRadius; ++k) {
            Dilate8(cur, pw, ph, tmp);
            Dilate8(ring, pw, ph, tmp);
        }
        int nRing = 0, nRingDark = 0;
        for (size_t i = 0; i < mask.size(); ++i) {
            const bool inRing = ring[i] && !mask[i] && valid[i];
            if (!inRing) continue;
            ++nRing;
            if (dk[i]) ++nRingDark;
        }
        const double ringFrac = nRing ? static_cast<double>(nRingDark) / nRing : 1.0;
        int nBoundHalo = 0;
        for (size_t i = 0; i < mask.size(); ++i)
            if (boundary[i] && cur[i]) ++nBoundHalo;
        const double boundFrac = nBoundary ? static_cast<double>(nBoundHalo) / nBoundary : 1.0;
        if (boundFrac < P.haloMinBound) continue;

        // contour noir net (voir DetectParams::outlineDark) : part des pixels du bord ayant un pixel <= outlineDark a 2 pixels au plus, parmi ceux qui ont un voisin
        // dans la ROI (les pixels du bord au ras de la ROI ne comptent pas)
        std::vector<uint8_t> dkO(mask.size(), 0), vNear = valid;
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x) {
                const size_t i = static_cast<size_t>(y * pw + x);
                if (!valid[i]) continue;
                const int gx = x - pad + bx0, gy = y - pad + by0;
                if (ws.value[static_cast<size_t>(gy) * static_cast<size_t>(w) + static_cast<size_t>(gx)] <= P.outlineDark) dkO[i] = 1;
            }
        for (int k = 0; k < P.haloRadius; ++k) {
            Dilate8(dkO, pw, ph, tmp);
            Dilate8(vNear, pw, ph, tmp);
        }
        int nOutB = 0, nOutDark = 0;
        for (size_t i = 0; i < mask.size(); ++i) {
            if (!boundary[i] || !vNear[i]) continue;
            ++nOutB;
            if (dkO[i]) ++nOutDark;
        }
        const double outlineFrac = nOutB ? static_cast<double>(nOutDark) / nOutB : 1.0;
        if (outlineFrac < P.outlineMin) continue;

        // moments / forme
        const double cx = sx / n, cy = sy / n;
        double mxx = 0, myy = 0, mxy = 0;
        for (int idx : r.px) {
            const double dx = idx % w - cx, dy = idx / w - cy;
            mxx += dx * dx;
            myy += dy * dy;
            mxy += dx * dy;
        }
        mxx = mxx / n + 1.0 / 12.0;
        myy = myy / n + 1.0 / 12.0;
        mxy /= n;
        const double tr = mxx + myy, det = mxx * myy - mxy * mxy;
        const double disc = std::sqrt(std::max(0.0, tr * tr / 4 - det));
        const double elong = std::sqrt((tr / 2 + disc) / std::max(tr / 2 - disc, 1e-6));
        const double hull = HullArea(corners);
        const double solidity = hull > 0 ? n / hull : 1.0;
        const double fill = static_cast<double>(n) / (static_cast<double>(bw) * bh);
        if (n >= P.tinyArea && solidity > P.solidityGate) continue;
        int nNear = 0;
        for (const auto& c : cents) {
            const double d = std::hypot(c.first - cx, c.second - cy);
            if (d > 0 && d <= P.isoRadius) ++nNear;
        }

        // point chaud : min(x + y), a egalite le plus petit y
        int tipX = 0, tipY = 0, best = 1 << 30;
        for (int idx : r.px) {
            const int x = idx % w, y = idx / w;
            if (x + y < best || (x + y == best && y < tipY)) {
                best = x + y;
                tipX = x;
                tipY = y;
            }
        }

        const double fPeak = P.peakFloor + (1 - P.peakFloor) * Ramp(peak, P.peakLo, P.peakHi);
        const double fSat = 0.7 + 0.3 * Ramp(satFrac, 0.08, 0.35);
        const double fHalo = P.haloFloor + (1 - P.haloFloor) * Ramp(ringFrac, P.haloRingLo, P.haloRingHi);
        const double fIso = 0.3 + 0.7 / (1.0 + 0.5 * std::max(0, nNear - 1));
        const double fSize = 0.4 + 0.6 * Trap(std::log(static_cast<double>(std::max(n, 1))), std::log(10.0), std::log(30.0), std::log(800.0), std::log(3000.0));
        double fShape;
        if (n < P.tinyArea) {
            fShape = 0.55 + 0.45 * Ramp(elong, 1.2, 2.5);
        } else {
            const double sSol = Trap(solidity, 0.50, 0.60, 0.82, 0.92);
            const double sFill = Trap(fill, 0.22, 0.32, 0.70, 0.85);
            fShape = 0.2 + 0.8 * sSol * (0.5 + 0.5 * sFill);
        }
        const double fOutline = 0.2 + 0.8 * Ramp(outlineFrac, 0.35, 0.85);
        double score = fPeak * fSat * fHalo * fIso * fSize * fShape * fOutline;
        if (clipped) score *= P.clipPenalty;

        PointerCandidate c;
        c.x = tipX;
        c.y = tipY;
        c.score = score;
        c.x0 = bx0;
        c.y0 = by0;
        c.x1 = bx1;
        c.y1 = by1;
        c.area = n;
        c.hand = n >= 120 && elong < 1.9;
        c.clipped = clipped;
        c.peak = peak;
        c.solidity = solidity;
        c.fill = fill;
        c.ring = ringFrac;
        c.bound = boundFrac;
        c.elong = elong;
        c.outline = outlineFrac;
        cands.push_back(c);
    }

    if (prior && prior->valid) {
        const double sg = std::max(prior->sigma, 1.0);
        for (PointerCandidate& c : cands) {
            const double d2 = (c.x - prior->x) * (c.x - prior->x) + (c.y - prior->y) * (c.y - prior->y);
            c.score *= 0.55 + 0.45 * std::exp(-d2 / (2 * sg * sg));  // un vrai pointeur loin de la prediction garde 55 % de son score (modele faux apres une perte)
        }
    }
    std::sort(cands.begin(), cands.end(), [](const PointerCandidate& a, const PointerCandidate& b) { return a.score > b.score; });
    const int nOut = std::min<int>(maxOut, static_cast<int>(cands.size()));
    for (int i = 0; i < nOut; ++i) out[i] = cands[static_cast<size_t>(i)];
    return nOut;
}

}  // namespace hands::servo
