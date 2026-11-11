// ServoBrain.cpp — voir ServoBrain.h.
#include "servo/ServoBrain.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace hands::servo {

namespace {
constexpr int kTileW = 768, kTileH = 576;      // balayage d'acquisition
constexpr int kTrackW = 448, kTrackH = 336;    // ROI de suivi
constexpr int kWideW = 768, kWideH = 576;      // ROI elargie quand le pointeur manque
constexpr double kMinScoreTrack = 0.25, kMinScoreScan = 0.40;
constexpr double kBlacklistRad = 0.03;         // rad (~35 px) : decors (eclats, reflets) pris pour le pointeur ; ils ne repondent pas aux comptes de souris
constexpr int kMaxProbeFails = 2;              // apres ce nombre de mesures sans reponse on continue avec les valeurs par defaut
constexpr uint64_t kHandSwitchMs = 350;        // l'autre main doit pointer seule pendant ce temps avant de reprendre la main (sauf pincement)
constexpr double kStableRad = 0.004;           // ~5 px : deux detections consecutives « au meme endroit »
constexpr uint64_t kLostToAcquireMs = 4000;    // pointeur introuvable depuis si longtemps : on recommence le balayage
constexpr uint64_t kKeepAliveMs = 700;         // MSFS masque le curseur apres quelques secondes sans mouvement et ignore les clics tant qu'il est masque
constexpr uint64_t kPinchLockLookbackMs = 130; // le verrou du pincement reprend la position du doigt d'il y a ~130 ms
constexpr uint64_t kClickDebounceMs = 90;      // pincement minimal avant d'enfoncer le bouton
}  // namespace

void ServoBrain::Log(int level, const char* fmt, ...) {
    if (!log_) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_(level, buf);
}

void ServoBrain::Reset() {
    mode_ = BrainMode::Acquire;
    ctl_ = ServoController(params_);
    probe_ = GainProbe();
    bad_.clear();
    probeFails_ = 0;
    switchSinceMs_ = 0;
    haveDet_ = false;
    stable_ = missRun_ = 0;
    hand_ = -1;
    locked_ = false;
    down_ = false;
    histN_[0] = histN_[1] = 0;
    prevPinch_[0] = prevPinch_[1] = false;
    tilePos_ = 0;
    badSince_ = 0;
    tracking_ = false;
    haveFinger_ = false;
    errPx_ = 0;
}

void ServoBrain::Recalibrate() {
    params_ = ServoParams();
    probeDone_ = false;
    Reset();
    Log(0, "servo : nouvelle mesure du gain demandee");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------------------------------------------------------------------------
int ServoBrain::ChooseHand(const BrainFrame& in, const bool edge[2]) {
    if (hand_ >= 0 && in.hand[hand_].usable && down_) return hand_;  // verrouillee pendant l'appui
    for (int k : {1, 0})
        if (edge[k] && in.hand[k].usable) return k;  // un pincement choisit la main
    bool ext[2];
    for (int k = 0; k < 2; ++k) ext[k] = in.hand[k].usable && in.hand[k].extended;
    if (hand_ >= 0 && in.hand[hand_].usable) {
        const int o = 1 - hand_;
        if (!(ext[o] && !ext[hand_])) {
            switchSinceMs_ = 0;
            return hand_;  // on ne change que si l'autre main pointe et pas celle-ci...
        }
        if (!switchSinceMs_) switchSinceMs_ = in.nowMs;
        if (in.nowMs - switchSinceMs_ < kHandSwitchMs) return hand_;  // ... depuis un moment : pas de va-et-vient quand les deux mains bougent
    }
    switchSinceMs_ = 0;
    if (ext[1]) return 1;
    if (ext[0]) return 0;
    if (in.hand[1].usable) return 1;
    if (in.hand[0].usable) return 0;
    return -1;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// ROI
// ---------------------------------------------------------------------------------------------------------------------------------------------
void ServoBrain::BuildTiles(const ImageRect& img) {
    tiles_.clear();
    if (img.w <= 0 || img.h <= 0) return;
    const int tw = std::min(kTileW, img.w), th = std::min(kTileH, img.h);
    const int cols = std::max(1, (img.w + tw - 1) / tw), rows = std::max(1, (img.h + th - 1) / th);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const int x = cols == 1 ? img.x : img.x + static_cast<int>(std::lround(static_cast<double>(c) * (img.w - tw) / (cols - 1)));
            const int y = rows == 1 ? img.y : img.y + static_cast<int>(std::lround(static_cast<double>(r) * (img.h - th) / (rows - 1)));
            tiles_.push_back({x, y});
        }
    tileImgW_ = img.w;
    tileImgH_ = img.h;
    tileW_ = tw;
    tileH_ = th;
    tilePos_ = 0;
}

bool ServoBrain::PickRoi(const EyeView& ev, const Px* finger, BrainRoi* r) {
    const ImageRect& img = ev.rect;
    Px center;
    bool haveCenter = false;
    int w = kTrackW, h = kTrackH;
    r->expX = r->expY = -1;
    auto project = [&](V2 ang, Px* out) {
        const emulation::Vec3 d = DirOf({ang.a, ang.b});
        return Project(ev, ev.pose.p + d, out);
    };
    if (mode_ == BrainMode::Track && ctl_.HaveModel()) {
        haveCenter = project(ctl_.PredictedInImage(seq_), &center);
        // pointeur perdu depuis un moment : trois requetes en rotation, sur la prediction du modele (pointeur fixe dans le monde), sur sa DERNIERE POSITION DANS L'IMAGE (pointeur
        // colle a la tete : il ne bouge pas dans l'image quand la tete tourne) et sur une dalle du balayage (le modele est faux : MSFS a re-ancre le pointeur)
        if (haveDet_ && nowMs_ - lastDetMs_ > 250) {
            roiPhase_ = (roiPhase_ + 1) % 3;
            if (roiPhase_ == 1) {
                center = lastDetPx_;
                haveCenter = true;
            } else if (roiPhase_ == 2) {
                haveCenter = false;
            }
        }
    } else if (mode_ == BrainMode::Probe && haveDet_) {
        haveCenter = project(lastDetAng_, &center);
        w = 320;
        h = 240;
    } else if (mode_ == BrainMode::Acquire && haveDet_ && nowMs_ - lastDetMs_ < 1500) {
        haveCenter = project(lastDetAng_, &center);  // on vient de le voir : on le confirme sur place
        w = 320;
        h = 240;
    }
    if (haveCenter) {
        r->expX = center.x;
        r->expY = center.y;
        if (mode_ == BrainMode::Track && haveDet_ && nowMs_ - lastDetMs_ > 250) {
            w = kWideW;
            h = kWideH;
        }
        if (mode_ == BrainMode::Track && haveDet_ && nowMs_ - lastDetMs_ > 1200) haveCenter = false;  // le prior ne vaut plus rien : balayage
    }
    if (haveCenter) {
        const Roi roi = MakeRoi(img, &center, 1, 0, w, h, w, h);
        if (roi.w <= 0) return false;
        r->x = roi.x;
        r->y = roi.y;
        r->w = roi.w;
        r->h = roi.h;
        return true;
    }
    // balayage : dalles triees par distance a la position de depart (le bout du doigt, sinon le centre de l'image)
    if (tiles_.empty() || tileImgW_ != img.w || tileImgH_ != img.h) BuildTiles(img);
    if (tiles_.empty()) return false;
    const double ox = finger ? finger->x : img.x + img.w * 0.5, oy = finger ? finger->y : img.y + img.h * 0.5;
    if (tilePos_ == 0) {
        std::stable_sort(tiles_.begin(), tiles_.end(), [&](const std::pair<int, int>& a, const std::pair<int, int>& b) {
            const double da = std::hypot(a.first + tileW_ * 0.5 - ox, a.second + tileH_ * 0.5 - oy), db = std::hypot(b.first + tileW_ * 0.5 - ox, b.second + tileH_ * 0.5 - oy);
            return da < db;
        });
    }
    const auto t = tiles_[static_cast<size_t>(tilePos_)];
    tilePos_ = (tilePos_ + 1) % static_cast<int>(tiles_.size());
    r->x = t.first;
    r->y = t.second;
    r->w = tileW_;
    r->h = tileH_;
    r->expX = r->expY = -1;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Pixels recus : detection, mesure
// ---------------------------------------------------------------------------------------------------------------------------------------------
void ServoBrain::OnPixels(const uint8_t* rgba, const BrainRoi& r) {
    DetectPrior prior;
    if (r.expX >= 0) {
        prior.valid = true;
        prior.x = r.expX - r.x;
        prior.y = r.expY - r.y;
        prior.sigma = (mode_ == BrainMode::Track && haveDet_) ? 70.0 : 120.0;
    }
    PointerCandidate c[4];
    const double minScore = prior.valid ? kMinScoreTrack : kMinScoreScan;
    int n = DetectPointer(rgba, r.w * 4, r.w, r.h, DetectParams(), prior.valid ? &prior : nullptr, c, 4);
    if ((n == 0 || c[0].score < minScore) && prior.valid) n = DetectPointer(rgba, r.w * 4, r.w, r.h, TrackingParams(), &prior, c, 4);
    const int fx = haveFinger_ ? static_cast<int>(std::lround(fingerPx_.x)) - r.x : -100000, fy = haveFinger_ ? static_cast<int>(std::lround(fingerPx_.y)) - r.y : -100000;
    if (n == 0 || c[0].score < minScore) {
        ++nMiss_;
        ++missRun_;
        stable_ = 0;
        if (mode_ == BrainMode::Track && missRun_ == 40 && diag_) diag_("manque", rgba, r, -1, -1, fx, fy);
        // recherche infructueuse depuis longtemps : une dalle annotee (la plus proche du doigt, la premiere du balayage) pour voir ce que la couche voit
        if (mode_ == BrainMode::Acquire && acquireSinceMs_ && nowMs_ - acquireSinceMs_ > 6000 && tilePos_ == 1 && diag_) {
            diag_("recherche", rgba, r, -1, -1, fx, fy);
            acquireSinceMs_ = nowMs_;
        }
        return;
    }
    const double px = r.x + c[0].x + 0.5, py = r.y + c[0].y + 0.5;
    const Ang a = AnglesOf(RayDir(r.view, {px, py}));
    const V2 ang{a.yaw, a.pitch};
    {
        const double rad = mode_ == BrainMode::Track ? 0.012 : kBlacklistRad;  // en suivi : seulement le voisinage immediat du decor
        for (const V2& b : bad_)
            if (Norm(ang - b) < rad) {  // un decor deja pris pour le pointeur
                ++nMiss_;
                stable_ = 0;
                return;
            }
    }
    const int missedBefore = missRun_;
    missRun_ = 0;
    sumScore_ += c[0].score;
    const bool consistent = haveDet_ && Norm(ang - lastDetAng_) < kStableRad;
    stable_ = consistent ? stable_ + 1 : 1;
    if (!haveDet_)
        Log(0, "servo : pointeur de MSFS repere en (%.0f, %.0f) dans l'image (score %.2f, %s, %dx%d)", px, py, c[0].score, c[0].hand ? "main" : "fleche", c[0].x1 - c[0].x0 + 1,
            c[0].y1 - c[0].y0 + 1);
    haveDet_ = true;
    lastDetAng_ = ang;
    lastDetPx_ = {px, py};
    lastDetMs_ = nowMs_;
    ++nDet_;
    if (mode_ == BrainMode::Probe) probe_.OnMeasurement(r.imageSeq, ang);
    if (mode_ == BrainMode::Track) {
        // apres une perte d'au moins ~6 mesures, le modele n'est plus fiable : une detection nette (score >= 0.6) le remet d'aplomb d'un coup
        if (missedBefore >= 6 && c[0].score >= 0.6) ctl_.Reanchor(r.imageSeq, ang);
        else ctl_.OnMeasurement(r.imageSeq, ang);
    }
    if (mode_ == BrainMode::Acquire && stable_ >= 4) {
        if (!probeDone_) {
            mode_ = BrainMode::Probe;
            probe_.Start(seq_);
            probe_.OnMeasurement(r.imageSeq, ang);
            Log(0, "servo : mesure du gain de la souris (quatre petites salves, gardez la main immobile)");
        } else {
            mode_ = BrainMode::Track;
            ctl_ = ServoController(params_);
            ctl_.OnMeasurement(r.imageSeq, ang);
            Log(0, "servo : suivi repris");
        }
        if (diag_) diag_("acquis", rgba, r, static_cast<int>(c[0].x), static_cast<int>(c[0].y), fx, fy);
    }
    // ecart pointeur / doigt trop grand pendant longtemps : capture pour analyse
    if (mode_ == BrainMode::Track && haveFinger_) {
        const double dx = px - fingerPx_.x, dy = py - fingerPx_.y;
        if (std::hypot(dx, dy) > 150.0) {
            if (!badSince_) badSince_ = nowMs_;
            if (nowMs_ - badSince_ > 2500) {
                if (diag_) diag_("ecart", rgba, r, static_cast<int>(c[0].x), static_cast<int>(c[0].y), fx, fy);
                badSince_ = nowMs_;
            }
        } else {
            badSince_ = 0;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Une image
// ---------------------------------------------------------------------------------------------------------------------------------------------
BrainOutput ServoBrain::Frame(const BrainFrame& in) {
    BrainOutput out;
    nowMs_ = in.nowMs;
    ++seq_;
    if (mode_ == BrainMode::Acquire) {
        if (!acquireSinceMs_) acquireSinceMs_ = in.nowMs;
    } else {
        acquireSinceMs_ = 0;
    }
    const double tl = std::tan(static_cast<double>(in.view.fov.left)), tr = std::tan(static_cast<double>(in.view.fov.right));
    pxPerRad_ = (in.view.rect.w > 0 && tr > tl) ? in.view.rect.w / (tr - tl) : 1215.0;

    // 1) main, bout du doigt, cible
    bool edge[2] = {false, false}, pinchNow[2] = {false, false};
    for (int k = 0; k < 2; ++k) {
        pinchNow[k] = in.hand[k].usable && in.hand[k].pinch;
        edge[k] = pinchNow[k] && !prevPinch_[k];
        if (in.hand[k].usable && !pinchNow[k]) {  // historique du bout du doigt (hors pincement : il glisse alors vers le pouce)
            Sample& sm = hist_[k][histHead_[k]];
            sm.t = in.nowMs;
            sm.p = in.hand[k].tip;
            histHead_[k] = (histHead_[k] + 1) % 24;
            histN_[k] = std::min(24, histN_[k] + 1);
        }
    }
    const int prevHand = hand_;
    hand_ = in.menuOpen ? -1 : ChooseHand(in, edge);
    const int hand = hand_;
    if (hand != prevHand && hand >= 0) Log(0, "servo : main %s", hand == 1 ? "droite" : "gauche");
    const bool pinch = hand >= 0 && pinchNow[hand];
    if (hand >= 0 && edge[hand]) {  // verrou : position du bout du doigt environ 130 ms avant le pincement
        emulation::Vec3 pos = in.hand[hand].tip;
        for (int i = 0; i < histN_[hand]; ++i) {
            const Sample& sm = hist_[hand][(histHead_[hand] - 1 - i + 48) % 24];
            pos = sm.p;
            if (in.nowMs - sm.t >= kPinchLockLookbackMs) break;
        }
        lockTip_ = pos;
        lockKnuckle_ = in.hand[hand].knuckle;
        locked_ = true;
    }
    if (!pinch) locked_ = false;
    haveFinger_ = false;
    V2 target;
    if (hand >= 0) {
        const BrainHand& hd = in.hand[hand];
        const emulation::Vec3 tipW = (pinch && locked_) ? lockTip_ + (hd.knuckle - lockKnuckle_) : hd.tip;
        const emulation::Vec3 d = tipW - in.view.pose.p;
        Px px;
        if (emulation::Length(d) > 0.05f && Project(in.view, tipW, &px)) {
            // Cible bornee a l'image (marge de 60 px) : le pointeur ne se voit pas hors de l'image, et le pousser vers un doigt tres decale ferait filer le curseur de
            // Windows hors de la fenetre de MSFS (autre ecran : les clics n'arrivent plus).
            const ImageRect& rc = in.view.rect;
            if (rc.w > 240 && rc.h > 240) {
                const double m = 60.0;
                const Px c{std::max(rc.x + m, std::min(rc.x + rc.w - m, px.x)), std::max(rc.y + m, std::min(rc.y + rc.h - m, px.y))};
                if (c.x != px.x || c.y != px.y) px = c;
                const Ang a = AnglesOf(RayDir(in.view, px));
                target = {a.yaw, a.pitch};
            } else {
                const Ang a = AnglesOf(emulation::Normalized(d));
                target = {a.yaw, a.pitch};
            }
            fingerPx_ = px;
            haveFinger_ = true;
        }
    }
    for (int k = 0; k < 2; ++k) prevPinch_[k] = pinchNow[k];

    // 2) commandes
    int sentDx = 0, sentDy = 0;
    if (mode_ == BrainMode::Acquire) {
        if (hand >= 0 && in.nowMs - lastWakeMs_ > 350) {  // reveille un curseur masque (MSFS le masque apres quelques secondes sans mouvement)
            lastWakeMs_ = in.nowMs;
            jiggle_ = -jiggle_;
            sentDx = 2 * jiggle_;
        }
    } else if (mode_ == BrainMode::Probe) {
        int dx = 0, dy = 0;
        const bool going = probe_.Step(seq_, &dx, &dy);
        sentDx = dx;
        sentDy = dy;
        if (!going && probe_.Done()) {
            const GainProbe::Result& r = probe_.Final();
            bool retry = false;
            if (r.ok) {
                params_.radPerCountX = r.radPerCountX;
                params_.radPerCountY = r.radPerCountY;
                params_.latencyFrames = r.latencyFrames;
                Log(0, "servo : gain mesure x %.2f px/compte, y %.2f px/compte (a priori %.2f), latence %d image(s)", r.radPerCountX * pxPerRad_, -r.radPerCountY * pxPerRad_,
                    ServoParams().radPerCountX * pxPerRad_, r.latencyFrames);
            } else {
                Log(1, "servo : mesure du gain impossible (%s ; gains par salve %.1f %.1f %.1f %.1f px/compte, latences %d %d %d %d)", r.why, r.est[0] * pxPerRad_, r.est[1] * pxPerRad_,
                    r.est[2] * pxPerRad_, r.est[3] * pxPerRad_, r.lat[0], r.lat[1], r.lat[2], r.lat[3]);
                if (!r.responded && probeFails_ < kMaxProbeFails && haveDet_) {  // la cible ne bouge pas avec la souris : un decor, pas le pointeur
                    ++probeFails_;
                    if (bad_.size() >= 8) bad_.erase(bad_.begin());
                    bad_.push_back(lastDetAng_);
                    Log(1, "servo : la cible reperee ne repond pas a la souris (reflet ou decor) : recherche du vrai pointeur (essai %d/%d)", probeFails_, kMaxProbeFails);
                    retry = true;
                }
            }
            if (retry) {
                mode_ = BrainMode::Acquire;
                probe_ = GainProbe();
                haveDet_ = false;
                stable_ = 0;
            } else {
                probeDone_ = true;
                mode_ = BrainMode::Track;
                ctl_ = ServoController(params_);
                haveDet_ = haveDet_ && in.nowMs - lastDetMs_ < 1000;
            }
        }
    } else {  // Track
        int jx = 0, jy = 0;
        if (hand >= 0 && in.nowMs - lastSendMs_ > kKeepAliveMs && !pinch) {  // garde le curseur visible
            jiggle_ = -jiggle_;
            jx = jiggle_;
        }
        const ServoController::Out o = ctl_.Step(seq_, hand >= 0 && haveFinger_, target, jx, jy);
        sentDx = o.dx;  // inclut deja les comptes de garde (jx, jy)
        sentDy = o.dy;
        if (o.haveModel && hand >= 0) errPx_ = Norm(o.error) * pxPerRad_;
        if (in.nowMs - lastDetMs_ > kLostToAcquireMs && haveDet_) {
            Log(1, "servo : pointeur introuvable depuis %.1f s : nouvelle recherche", static_cast<double>(in.nowMs - lastDetMs_) / 1000.0);
            mode_ = BrainMode::Acquire;
            haveDet_ = false;
            stable_ = 0;
            ctl_ = ServoController(params_);
        }
    }
    out.dx = sentDx;
    out.dy = sentDy;
    if (sentDx != 0 || sentDy != 0) {
        lastSendMs_ = in.nowMs;
        ++nSent_;
    }

    // 3) bouton : pas de clic tant que le pointeur n'est pas repere ; le bouton ne descend qu'apres kClickDebounceMs de pincement (le geste d'ouverture du menu pince
    //    les deux mains presque ensemble : pas de clic parasite dans le cockpit) ; deux mains pincees = jamais de clic ; un pincement plus court est un « tap » (clic bref).
    const bool canClick = mode_ == BrainMode::Track && ctl_.HaveModel() && (in.nowMs - lastDetMs_ < 800);
    const bool both = pinchNow[0] && pinchNow[1];
    if (hand >= 0 && edge[hand]) pinchStartMs_ = in.nowMs;
    if (tapUp_) {
        out.buttonEvent = 0;
        tapUp_ = false;
        down_ = false;
    } else {
        bool want = false;
        if (hand >= 0 && pinch && !both && (canClick || down_)) want = down_ || (in.nowMs - pinchStartMs_ >= kClickDebounceMs);
        if (want && !down_) {
            out.buttonEvent = 1;
            down_ = true;
            lastSendMs_ = in.nowMs;
        } else if (!want && down_) {
            out.buttonEvent = 0;
            down_ = false;
        } else if (!want && !down_ && prevSelPinch_ && !pinch && !prevBoth_ && heldMs_ < kClickDebounceMs && canClick && hand >= 0) {  // tap
            out.buttonEvent = 1;
            down_ = true;
            tapUp_ = true;
            lastSendMs_ = in.nowMs;
        }
    }
    heldMs_ = pinch ? in.nowMs - pinchStartMs_ : 0;
    prevSelPinch_ = pinch;
    prevBoth_ = both;

    // 4) requete suivante
    if (in.readerFree) {
        BrainRoi r;
        if (PickRoi(in.view, haveFinger_ ? &fingerPx_ : nullptr, &r)) {
            r.valid = true;
            r.imageSeq = seq_;
            r.view = in.view;
            out.request = r;
            ++nReq_;
        }
    }

    // 5) etat et journal
    const bool tracking = mode_ == BrainMode::Track && haveDet_ && in.nowMs - lastDetMs_ < 600;
    SetStatus(in, tracking);
    if (tracking != tracking_) {
        Log(0, tracking ? "servo : pointeur suivi" : "servo : pointeur perdu");
        tracking_ = tracking;
    }
    if (in.nowMs - logT_ >= 1000) {
        const double dt = logT_ ? static_cast<double>(in.nowMs - logT_) / 1000.0 : 1.0;
        const double meanScore = nDet_ ? sumScore_ / nDet_ : 0.0;
        sumScore_ = 0;
        Log(0, "servo : %s, main %s, %.0f detections/s (%d requetes, %d manques), score moyen %.2f, ecart estime %.0f px, pointeur (%.0f,%.0f), doigt (%.0f,%.0f), %d envois, L=%d, gain %.2f/%.2f px/compte%s%s",
            mode_ == BrainMode::Acquire ? "recherche" : (mode_ == BrainMode::Probe ? "mesure" : "suivi"), hand == 1 ? "droite" : (hand == 0 ? "gauche" : "aucune"), nDet_ / dt, nReq_,
            nMiss_, meanScore, errPx_, lastDetPx_.x, lastDetPx_.y, haveFinger_ ? fingerPx_.x : -1.0, haveFinger_ ? fingerPx_.y : -1.0, nSent_, params_.latencyFrames,
            params_.radPerCountX * pxPerRad_, -params_.radPerCountY * pxPerRad_, down_ ? ", bouton enfonce" : "", ctl_.Jumps() ? ", sauts du pointeur" : "");
        nDet_ = nReq_ = nMiss_ = nSent_ = 0;
        logT_ = in.nowMs;
    }
    return out;
}

void ServoBrain::SetStatus(const BrainFrame& in, bool tracking) {
    char st[200];
    if (mode_ == BrainMode::Probe) std::snprintf(st, sizeof st, "mesure du gain en cours…");
    else if (tracking) std::snprintf(st, sizeof st, "suivi OK, écart estimé %.0f px, main %s", errPx_, hand_ == 1 ? "droite" : (hand_ == 0 ? "gauche" : "—"));
    else std::snprintf(st, sizeof st, "recherche du pointeur MSFS… (bougez un peu la souris si rien ne se passe)");
    (void)in;
    status_ = st;
}

}  // namespace hands::servo
