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
constexpr double kWheelStartDeg = 14.0;        // roulis du poignet pendant un pincement a partir duquel le pincement devient une molette
constexpr uint64_t kClickDebounceMs = 110;     // pincement minimal avant d'enfoncer le bouton
constexpr uint64_t kTapMinMs = 50;             // un pincement plus court que ca est un parasite du suivi des mains, pas un clic
constexpr uint64_t kClickPendingMaxMs = 700;   // un pincement qui n'a pas pu cliquer (pointeur pas encore arrive) dans ce delai est abandonne : pas de clic tardif
constexpr uint64_t kReleaseDebounceMs = 90;    // un pincement qui « saute » moins que ca pendant un appui ne relache pas le bouton (un relachement = un clic pour MSFS)
constexpr uint64_t kClickFreshMs = 300;        // pour cliquer, le pointeur doit avoir ete VU depuis moins que ca (MSFS ignore les clics tant qu'il est masque)
constexpr double kClickMaxErrPx = 45.0;        // ... et etre a moins de ca du bout du doigt (sinon le clic tomberait a cote, ou sur une autre commande)
constexpr uint64_t kSwitchStealMs = 800;       // apres un changement de main, l'autre ne la reprend pas par un pincement avant ce delai
constexpr double kInsideMarginPx = 0.0;        // une main n'est candidate que si son bout de doigt est dans l'image (a cette marge pres)
constexpr double kClampMarginPx = 40.0;        // la cible du pointeur est bornee a l'image moins cette marge
constexpr uint64_t kBothWindowMs = 400;        // deux mains « pincees ensemble » (geste d'ouverture du menu) : les deux fronts a moins de ca l'un de l'autre
constexpr double kStuckShortfallPx = 450.0;    // le pointeur n'a pas suivi ~450 px de deplacement commande : mur de MSFS / menu 2D / curseur fige
constexpr double kStuckOkShortfallPx = 150.0;  // ... il suit (a peu pres) tant que le manque reste sous ca
constexpr double kStuckResetMovePx = 60.0;     // deplacement du pointeur a partir duquel on repart d'une nouvelle reference
constexpr uint64_t kBlockBaseMs = 2000, kBlockMaxMs = 8000;  // pause des comptes quand le pointeur ne suit plus (doublee a chaque nouvel echec)
constexpr double kFreeStartDeg = 22.0;         // molette SANS pincement (rotatifs : pas besoin de cliquer) : roulis rapide du poignet, doigt immobile sur la commande
constexpr double kFreeCalmPxPerS = 300.0;      // le bout du doigt ne doit pas bouger de plus de ca (px/s) pour que le roulis compte
constexpr double kFreeRefTauS = 1.0;           // la reference du roulis suit la main avec cette constante de temps : une derive lente ne declenche rien
constexpr uint64_t kFreeIdleMs = 1500;         // la molette s'arrete si le poignet ne bouge plus depuis ce temps
constexpr float kLockMaxBackM = 0.03f;       // le verrou ne remonte pas a plus de 3 cm du doigt (le geste de pincement en deplace de 1 a 2)
constexpr float kPrelatchOnM = 0.034f, kPrelatchOffM = 0.038f;  // distance pouce-doigt qui fige le pointeur (le geste de pincement deplace deja le doigt) / le libere
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

// Jacobian of the plant at the model's pointer position for the current view (see Jac in ServoController.h): angle change per mouse count
bool ServoBrain::ComputeJac(const EyeView& view, V2 ang, Jac* out) const {
    Px p0;
    if (!Project(view, view.pose.p + DirOf({ang.a, ang.b}), &p0)) return false;
    const auto angAt = [&](double x, double y) {
        const Ang a = AnglesOf(RayDir(view, {x, y}));
        return V2{a.yaw, a.pitch};
    };
    const V2 a0 = angAt(p0.x, p0.y), ax = angAt(p0.x + 1.0, p0.y), ay = angAt(p0.x, p0.y + 1.0);
    const auto wrap = [](double d) {
        while (d > 3.14159265358979) d -= 6.28318530717959;
        while (d < -3.14159265358979) d += 6.28318530717959;
        return d;
    };
    const double gx = std::fabs(params_.radPerCountX) * pxPerRad_, gy = std::fabs(params_.radPerCountY) * pxPerRad_;  // pixels per count
    out->a = wrap(ax.a - a0.a) * gx;
    out->c = (ax.b - a0.b) * gx;
    out->b = wrap(ay.a - a0.a) * gy;
    out->d = (ay.b - a0.b) * gy;  // dy > 0 moves the pointer DOWN: pitch decreases
    out->gx = gx;
    out->gy = gy;
    out->pxPerRad = pxPerRad_;
    out->valid = true;
    return true;
}

long long ServoBrain::CumAt(const long long* arr, uint64_t imageSeq) const {
    const int64_t k = static_cast<int64_t>(imageSeq) - params_.latencyFrames;
    if (k > static_cast<int64_t>(seq_)) return arr[seq_ % 64];
    if (k + 62 < static_cast<int64_t>(seq_)) return arr[(seq_ + 1) % 64];  // trop vieux : la plus ancienne valeur connue
    return arr[static_cast<uint64_t>(k) % 64];
}

void ServoBrain::ResetStuck() {
    stuckRef_ = lastDetPx_;
    stuckBaseX_ = CumAt(cumX_, lastDetSeq_);
    stuckBaseY_ = CumAt(cumY_, lastDetSeq_);
}

void ServoBrain::Reset() {
    mode_ = BrainMode::Acquire;
    ctl_ = ServoController(params_);
    probe_ = GainProbe();
    bad_.clear();
    probeFails_ = 0;
    probeDone_ = !autoProbe_;
    probeViewSet_ = false;
    switchSinceMs_ = 0;
    haveDet_ = false;
    stable_ = missRun_ = 0;
    hand_ = -1;
    locked_ = false;
    down_ = false;
    histN_[0] = histN_[1] = 0;
    prevPinch_[0] = prevPinch_[1] = false;
    prevRaw_[0] = prevRaw_[1] = false;
    armed_[0] = armed_[1] = false;
    prevCanClick_ = pinchClicked_ = prevWheelUsed_ = false;
    prelatched_ = preArmed_ = false;
    deferTap_ = false;
    freeWheel_ = freeRefValid_ = haveFingerPrev_ = false;
    fpSpeed_ = 0.0;
    blockUntilMs_ = 0;
    blockN_ = 0;
    lastJumps_ = 0;
    std::fill(std::begin(cumX_), std::end(cumX_), 0LL);
    std::fill(std::begin(cumY_), std::end(cumY_), 0LL);
    stuckBaseX_ = stuckBaseY_ = 0;
    prevDist_[0] = prevDist_[1] = 1.0f;
    errNowPx_ = 1e9;
    tilePos_ = 0;
    badSince_ = 0;
    tracking_ = false;
    haveFinger_ = false;
    errPx_ = 0;
}

void ServoBrain::Recalibrate() {
    params_ = ServoParams();
    Reset();
    probeDone_ = false;  // apres Reset() (qui le remet selon autoProbe_)
    Log(0, "servo : nouvelle mesure du gain demandee");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------------------------------------------------------------------------
int ServoBrain::ChooseHand(const BrainFrame& in, const bool edge[2]) {
    if (hand_ >= 0 && in.hand[hand_].usable && down_) return hand_;  // verrouillee pendant l'appui
    for (int k : {1, 0}) {
        if (!(edge[k] && in.hand[k].usable)) continue;  // un pincement choisit la main
        if (k != hand_ && hand_ >= 0 && in.hand[hand_].usable && in.nowMs - lastSwitchMs_ < kSwitchStealMs) continue;  // pas de va-et-vient
        return k;
    }
    bool ext[2];
    for (int k = 0; k < 2; ++k) ext[k] = in.hand[k].usable && in.hand[k].extended;
    if (hand_ >= 0 && in.hand[hand_].usable) {
        const int o = 1 - hand_;
        if (in.nowMs - lastSwitchMs_ < kSwitchStealMs) return hand_;  // une main vient de prendre la main : on ne repart pas dans l'autre sens tout de suite
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
    lastDetSeq_ = r.imageSeq;
    ++nDet_;
    // la mesure du gain se fait dans le plan de l'image (pseudo-angles = pixels / pxPerRad) : MSFS deplace son pointeur d'un nombre constant de PIXELS par compte, pas de radians
    // La tete peut bouger pendant la mesure (pointeur fixe dans le monde) : les positions sont projetees dans la vue de reference du debut de la mesure.
    const auto chartIn = [&](const EyeView& v, bool* ok) {
        Px q;
        *ok = Project(v, v.pose.p + DirOf({ang.a, ang.b}), &q);
        return V2{(q.x - (v.rect.x + v.rect.w * 0.5)) / pxPerRad_, -(q.y - (v.rect.y + v.rect.h * 0.5)) / pxPerRad_};
    };
    if (mode_ == BrainMode::Probe && probeViewSet_) {
        bool ok = false;
        const V2 chart = chartIn(probeView_, &ok);
        if (ok) probe_.OnMeasurement(r.imageSeq, chart);
    }
    if (mode_ == BrainMode::Track) {
        // apres une perte d'au moins ~6 mesures, le modele n'est plus fiable : une detection nette (score >= 0.6) le remet d'aplomb d'un coup
        if (missedBefore >= 6 && c[0].score >= 0.6) ctl_.Reanchor(r.imageSeq, ang);
        else ctl_.OnMeasurement(r.imageSeq, ang);
    }
    if (mode_ == BrainMode::Acquire && stable_ >= 4) {
        if (!probeDone_) {
            mode_ = BrainMode::Probe;
            probe_.Start(seq_);
            probeView_ = r.view;
            probeViewSet_ = true;
            bool ok = false;
            const V2 chart = chartIn(probeView_, &ok);
            if (ok) probe_.OnMeasurement(r.imageSeq, chart);
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
BrainOutput ServoBrain::Frame(const BrainFrame& inRaw) {
    BrainFrame in = inRaw;
    BrainOutput out;
    nowMs_ = in.nowMs;
    ++seq_;
    cumX_[seq_ % 64] = cumX_[(seq_ + 63) % 64];
    cumY_[seq_ % 64] = cumY_[(seq_ + 63) % 64];
    if (mode_ == BrainMode::Acquire) {
        if (!acquireSinceMs_) acquireSinceMs_ = in.nowMs;
    } else {
        acquireSinceMs_ = 0;
    }
    const double tl = std::tan(static_cast<double>(in.view.fov.left)), tr = std::tan(static_cast<double>(in.view.fov.right));
    pxPerRad_ = (in.view.rect.w > 0 && tr > tl) ? in.view.rect.w / (tr - tl) : 1215.0;

    // 0) bout du doigt lisse (One Euro) : le suivi des mains tremble de 1 a 3 mm, soit 3 a 8 px a 40 cm, et le pointeur « dansait » ; lisse a l'arret, reactif en mouvement
    {
        const float dt = lastFrameMs_ ? static_cast<float>(in.nowMs - lastFrameMs_) / 1000.0f : 0.0f;
        lastFrameMs_ = in.nowMs;
        if (dt > 0.001f && dt < 0.5f) lastFrameDtS_ = dt;
        if (dt > 0.001f && dt < 0.5f) frameMsEma_ = frameMsEma_ * 0.9 + dt * 1000.0 * 0.1;
        // plafond de comptes par image : ~5400 comptes/s (60 a 90 i/s) ; a 30 i/s : 150 par image, sinon le pointeur ne rattrape pas une main rapide (2000 px/s au plus)
        ctl_.SetMaxCounts(std::max(60, std::min(150, static_cast<int>(5400.0 * frameMsEma_ / 1000.0))));
        for (int k = 0; k < 2; ++k) {
            if (!in.hand[k].usable) {
                tipFilter_[k].Reset();
                continue;
            }
            in.hand[k].tip = tipFilter_[k].Filter(in.hand[k].tip, dt, tipMinHz_, tipBeta_);
        }
    }

    // 1) main, bout du doigt, cible
    // Une main n'est candidate que si son bout de doigt se projette DANS l'image (marge de 40 px) : le pointeur ne peut pas l'y suivre (murs de MSFS) et une main
    // qui repose en bas ou sur le cote ne doit ni prendre la main ni cliquer (clics a l'autre bout de l'image dans l'essai du 12 nov.).
    const ImageRect& rcImg = in.view.rect;
    const bool bigImg = rcImg.w > 240 && rcImg.h > 240;
    bool inside[2] = {false, false};
    for (int k = 0; k < 2; ++k) {
        if (!in.hand[k].usable) continue;
        Px px;
        const emulation::Vec3 d = in.hand[k].tip - in.view.pose.p;
        if (!(emulation::Length(d) > 0.05f && Project(in.view, in.hand[k].tip, &px))) continue;
        inside[k] = !bigImg || (px.x >= rcImg.x - kInsideMarginPx && px.x <= rcImg.x + rcImg.w + kInsideMarginPx && px.y >= rcImg.y - kInsideMarginPx &&
                                px.y <= rcImg.y + rcImg.h + kInsideMarginPx);
    }
    BrainFrame sel = in;  // ce que le choix de la main et le pincement voient
    for (int k = 0; k < 2; ++k) sel.hand[k].usable = in.hand[k].usable && inside[k];

    bool edge[2] = {false, false}, pinchNow[2] = {false, false}, pinchRaw[2] = {false, false};
    for (int k = 0; k < 2; ++k) {
        pinchRaw[k] = in.hand[k].usable && in.hand[k].pinch;
        if (pinchRaw[k]) lastPinchSeenMs_[k] = in.nowMs;
        pinchNow[k] = sel.hand[k].usable && in.hand[k].pinch;
        // un pincement qui « saute » pendant un appui : on garde l'appui (relacher puis renfoncer = deux clics pour MSFS)
        if (down_ && k == hand_ && !pinchNow[k] && sel.hand[k].usable && in.nowMs - lastPinchSeenMs_[k] < kReleaseDebounceMs) pinchNow[k] = true;
        if (pinchRaw[k] && !prevRaw_[k]) pinchEdgeMs_[k] = in.nowMs;
        edge[k] = pinchRaw[k] && !prevRaw_[k] && sel.hand[k].usable && !(down_ && k == hand_);  // front du pincement brut, main exploitable
        if (!pinchNow[k]) armed_[k] = false;
        if (in.hand[k].usable && !pinchNow[k]) {  // historique du bout du doigt (hors pincement : il glisse alors vers le pouce)
            Sample& sm = hist_[k][histHead_[k]];
            sm.t = in.nowMs;
            sm.p = in.hand[k].tip;
            histHead_[k] = (histHead_[k] + 1) % 24;
            histN_[k] = std::min(24, histN_[k] + 1);
        }
    }
    const int prevHand = hand_;
    hand_ = in.menuOpen ? -1 : ChooseHand(sel, edge);
    const int hand = hand_;
    if (hand != prevHand) {
        prelatched_ = false;
        preArmed_ = false;
        locked_ = false;
        if (hand >= 0) {
            Log(0, "servo : main %s", hand == 1 ? "droite" : "gauche");
            lastSwitchMs_ = in.nowMs;
            ResetStuck();
        }
    }
    if (hand >= 0 && edge[hand]) armed_[hand] = true;  // seul un pincement commence PENDANT que cette main est la main choisie peut cliquer
    const bool pinch = hand >= 0 && pinchNow[hand] && armed_[hand];
    // verrou : position du bout du doigt environ 130 ms avant le debut du geste. Le geste de pincement (le pouce vient a la rencontre du doigt, qui bouge aussi) dure
    // 150 a 300 ms : des que le pouce s'approche (pre-verrou, comme le mode rayon) on fige la position ; sinon, au front du pincement.
    auto lockFromHistory = [&](int h) {
        emulation::Vec3 pos = in.hand[h].tip;
        for (int i = 0; i < histN_[h]; ++i) {
            const Sample& sm = hist_[h][(histHead_[h] - 1 - i + 48) % 24];
            if (emulation::Length(sm.p - in.hand[h].tip) > kLockMaxBackM) break;  // le doigt se deplacait vraiment (main qui arrive) : pas de retour en arriere au-dela de ca
            pos = sm.p;
            if (in.nowMs - sm.t >= kPinchLockLookbackMs) break;
        }
        lockTip_ = pos;
        lockKnuckle_ = in.hand[h].knuckle;
        locked_ = true;
    };
    if (hand >= 0) {
        const float dist = in.hand[hand].pinchDist;
        if (dist > kPrelatchOffM || dist < 0.0f) preArmed_ = true;
        if (!pinch && !prelatched_ && preArmed_ && dist >= 0.0f && dist < kPrelatchOnM && dist < prevDist_[hand] - 0.0005f) {
            prelatched_ = true;
            prelatchMs_ = in.nowMs;
            lockFromHistory(hand);
        } else if (prelatched_ && !pinch && (dist > kPrelatchOffM || in.nowMs - prelatchMs_ > 1500)) {
            prelatched_ = false;
            preArmed_ = false;  // reste desarme tant que le pouce n'est pas reparti
            locked_ = false;
        }
        prevDist_[hand] = dist;
        if (edge[hand] && !prelatched_) lockFromHistory(hand);
        if (edge[hand] && prelatched_) prelatched_ = false;  // le pincement prend le relais (le verrou garde sa position)
    }
    if (!pinch && !prelatched_ && !freeWheel_) locked_ = false;
    if (deferTap_ && (in.nowMs > deferUntilMs_ || hand != deferHand_ || pinch || in.menuOpen)) deferTap_ = false;
    haveFinger_ = false;
    if (hand < 0) haveFingerJump_ = false;
    V2 target;
    if (hand >= 0) {
        const BrainHand& hd = in.hand[hand];
        // clic differe : le pointeur n'etait pas encore arrive quand le pincement s'est termine ; la cible reste a l'endroit ou le doigt etait pendant le pincement
        const emulation::Vec3 tipW = ((pinch || prelatched_ || freeWheel_) && locked_) ? lockTip_ + (hd.knuckle - lockKnuckle_) : (deferTap_ ? deferTipW_ : hd.tip);
        if ((pinch || prelatched_) && locked_) lastLockedTipW_ = tipW;
        const emulation::Vec3 d = tipW - in.view.pose.p;
        Px px;
        if (emulation::Length(d) > 0.05f && Project(in.view, tipW, &px)) {
            // Cible bornee a l'image (la meme marge) : pousser le pointeur vers un doigt tres decale ferait filer le curseur de Windows hors de la fenetre de MSFS.
            if (bigImg) {
                const Px c{std::max(rcImg.x + kClampMarginPx, std::min(rcImg.x + rcImg.w - kClampMarginPx, px.x)),
                           std::max(rcImg.y + kClampMarginPx, std::min(rcImg.y + rcImg.h - kClampMarginPx, px.y))};
                if (c.x != px.x || c.y != px.y) px = c;
                const Ang a = AnglesOf(RayDir(in.view, px));
                target = {a.yaw, a.pitch};
            } else {
                const Ang a = AnglesOf(emulation::Normalized(d));
                target = {a.yaw, a.pitch};
            }
            // saut de la cible (changement de main, verrou qui se pose ou se leve) : pas de pic d'anticipation (la vitesse de la cible serait enorme)
            const bool lockedNow = ((pinch || prelatched_ || freeWheel_) && locked_) || deferTap_;
            if (hand != prevTargetHand_ || lockedNow != prevTargetLocked_ || (haveFingerJump_ && std::hypot(px.x - jumpPrevPx_.x, px.y - jumpPrevPx_.y) > 150.0)) ctl_.ResetTarget();
            prevTargetHand_ = hand;
            prevTargetLocked_ = lockedNow;
            jumpPrevPx_ = px;
            haveFingerJump_ = true;
            fingerPx_ = px;
            haveFinger_ = true;
        }
    }
    for (int k = 0; k < 2; ++k) {
        prevPinch_[k] = pinchNow[k];
        prevRaw_[k] = pinchRaw[k];
    }
    // geste d'ouverture du menu = les deux mains pincent a peu pres ENSEMBLE ; une main qui pince depuis longtemps (posee sur la manette des gaz, etc.) ne doit pas
    // empecher l'autre de cliquer
    const bool bothRaw = pinchRaw[0] && pinchRaw[1] && (pinchEdgeMs_[0] > pinchEdgeMs_[1] ? pinchEdgeMs_[0] - pinchEdgeMs_[1] : pinchEdgeMs_[1] - pinchEdgeMs_[0]) < kBothWindowMs;

    // 2) commandes
    int sentDx = 0, sentDy = 0;
    errNowPx_ = 1e9;
    bool havePred = false;
    V2 predAng;
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
                if (r.sent && !r.responded && probeFails_ < kMaxProbeFails && haveDet_) {  // la cible ne bouge pas avec la souris : un decor, pas le pointeur
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
        const bool blocked = blockUntilMs_ > in.nowMs;
        if (!blocked && blockUntilMs_) {  // fin de la pause : on repart du pointeur tel qu'on le voit
            blockUntilMs_ = 0;
            ResetStuck();
            if (haveDet_ && in.nowMs - lastDetMs_ < 400) ctl_.Reanchor(lastDetSeq_, lastDetAng_);
        }
        Jac jac;
        const bool haveJ = ctl_.HaveModel() && ComputeJac(in.view, ctl_.PredictedNow(), &jac);
        const ServoController::Out o = ctl_.Step(seq_, hand >= 0 && haveFinger_ && !blocked, target, jx, jy, haveJ ? &jac : nullptr);
        // cumul des comptes envoyes (hors garde) par numero d'image : sert a savoir ce qui a DEJA pu atterrir dans l'image d'ou vient la derniere detection
        cumX_[seq_ % 64] = cumX_[(seq_ + 63) % 64] + (blocked ? 0 : o.dx - jx);
        cumY_[seq_ % 64] = cumY_[(seq_ + 63) % 64] + (blocked ? 0 : o.dy - jy);
        if (!blocked) {
            // le pointeur suit-il les comptes ? deplacement commande ATTERRI (comptes envoyes jusqu'a l'image de la detection - latence) contre deplacement vu depuis la reference
            if (ctl_.Jumps() != lastJumps_) {  // le pointeur a saute (re-ancrage par MSFS) : nouvelle reference
                lastJumps_ = ctl_.Jumps();
                ResetStuck();
            }
            if (haveDet_ && in.nowMs - lastDetMs_ < 120) {  // detection FRAICHE seulement : un pointeur non detecte (hors de la vue, ROI a cote) n'est pas un pointeur fige
                const long long sx = CumAt(cumX_, lastDetSeq_) - stuckBaseX_, sy = CumAt(cumY_, lastDetSeq_) - stuckBaseY_;
                const double ex = sx * params_.radPerCountX * pxPerRad_, ey = -sy * params_.radPerCountY * pxPerRad_;
                const double mx = lastDetPx_.x - stuckRef_.x, my = lastDetPx_.y - stuckRef_.y;
                const double en = std::hypot(ex, ey);
                const double along = en > 1e-6 ? (mx * ex + my * ey) / en : 0.0;
                const double shortfall = en - along;
                if (std::hypot(mx, my) > kStuckResetMovePx && shortfall < kStuckOkShortfallPx) {
                    ResetStuck();
                    blockN_ = 0;
                } else if (en > kStuckShortfallPx && shortfall > kStuckShortfallPx) {
                    const uint64_t ms = std::min(kBlockMaxMs, kBlockBaseMs << std::min(blockN_, 2));
                    ++blockN_;
                    blockUntilMs_ = in.nowMs + ms;
                    Log(1, "servo : le pointeur ne suit plus les comptes (%.0f px commandes, %.0f vus : mur de MSFS, menu 2D ou curseur fige) : pause de %.0f s", en, along, ms / 1000.0);
                }
            }
        }
        sentDx = o.dx;  // inclut deja les comptes de garde (jx, jy)
        sentDy = o.dy;
        const double errPxNow = haveJ ? o.errUnits : Norm(o.error) * pxPerRad_;  // pixels
        if (o.haveModel && hand >= 0) errPx_ = errPxNow;
        if (o.haveModel) {
            havePred = true;
            predAng = o.predicted;
            if (hand >= 0 && haveFinger_) errNowPx_ = errPxNow;
        }
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

    // 2b) molette : pendant un pincement, un roulis du poignet (comme pour les rotatifs des zones) fait tourner la molette de la souris : un cran tous les
    //     wheelStepDeg_ degres. Le pincement devient alors une molette : le bouton est relache et ne redescend pas.
    if (hand >= 0 && pinch && in.hand[hand].wristAxis.x * in.hand[hand].wristAxis.x + in.hand[hand].wristAxis.y * in.hand[hand].wristAxis.y + in.hand[hand].wristAxis.z * in.hand[hand].wristAxis.z > 0.5f) {
        const BrainHand& hd = in.hand[hand];
        const emulation::Vec3 n1 = emulation::Normalized(hd.palmUp - hd.wristAxis * emulation::Dot(hd.palmUp, hd.wristAxis));
        if (edge[hand]) {
            rollAxis_ = hd.wristAxis;
            rollN0_ = n1;
            rollAcc_ = 0;
            rollLast_ = 0;
            wheelMode_ = false;
        } else {
            const double ang = std::atan2(static_cast<double>(emulation::Dot(rollAxis_, emulation::Cross(rollN0_, n1))), static_cast<double>(emulation::Dot(rollN0_, n1))) * 57.29577951;
            double d = ang - rollLast_;
            while (d > 180.0) d -= 360.0;
            while (d < -180.0) d += 360.0;
            rollLast_ = ang;
            rollAcc_ += d;
            if (!wheelMode_ && std::fabs(rollAcc_) >= kWheelStartDeg) {
                wheelMode_ = true;
                rollAcc_ -= rollAcc_ > 0 ? kWheelStartDeg : -kWheelStartDeg;
                Log(0, "servo : molette (roulis du poignet)");
            }
            if (wheelMode_) {
                int steps = 0;
                while (rollAcc_ >= wheelStepDeg_) {
                    rollAcc_ -= wheelStepDeg_;
                    ++steps;
                }
                while (rollAcc_ <= -wheelStepDeg_) {
                    rollAcc_ += wheelStepDeg_;
                    --steps;
                }
                out.wheelSteps = wheelInvert_ ? -steps : steps;
            }
        }
    }
    if (!pinch) wheelMode_ = false;

    // 2c) molette SANS pincement : pour les rotatifs il n'y a pas besoin de cliquer. Le doigt pointe la commande (pointeur arrive, doigt quasi immobile) et le poignet fait un
    //     roulis rapide d'au moins 22 deg par rapport a sa position « lissee » : un cran tous les wheelStepDeg_ degres, le pointeur reste verrouille sur la commande.
    {
        const double dtS = lastFrameDtS_;
        if (haveFinger_) {
            if (haveFingerPrev_ && dtS > 0.001) fpSpeed_ = fpSpeed_ * 0.7 + 0.3 * std::hypot(fingerPx_.x - fingerPrevPx_.x, fingerPx_.y - fingerPrevPx_.y) / dtS;
            fingerPrevPx_ = fingerPx_;
        }
        haveFingerPrev_ = haveFinger_;
        const bool axisOk = hand >= 0 && in.hand[hand].wristAxis.x * in.hand[hand].wristAxis.x + in.hand[hand].wristAxis.y * in.hand[hand].wristAxis.y +
                                                 in.hand[hand].wristAxis.z * in.hand[hand].wristAxis.z > 0.5f;
        if (freeWheel_ && (hand != freeHand_ || !axisOk || pinch || in.menuOpen || mode_ != BrainMode::Track)) {
            freeWheel_ = false;
            if (!pinch && !prelatched_) locked_ = false;
        }
        if (axisOk && !pinch && !in.menuOpen && mode_ == BrainMode::Track) {
            const BrainHand& hd = in.hand[hand];
            const emulation::Vec3 n1 = emulation::Normalized(hd.palmUp - hd.wristAxis * emulation::Dot(hd.palmUp, hd.wristAxis));
            if (!freeRefValid_ || freeRefHand_ != hand || hand != prevHand) {
                freeRef_ = n1;
                freeRefValid_ = true;
                freeRefHand_ = hand;
            }
            const auto rollOf = [&](const emulation::Vec3& axis, const emulation::Vec3& a, const emulation::Vec3& b) {
                return std::atan2(static_cast<double>(emulation::Dot(axis, emulation::Cross(a, b))), static_cast<double>(emulation::Dot(a, b))) * 57.29577951;
            };
            if (!freeWheel_) {
                const double ang = rollOf(hd.wristAxis, freeRef_, n1);
                const bool calm = fpSpeed_ < kFreeCalmPxPerS;
                const bool ready = hd.extended && calm && haveFinger_ && errNowPx_ <= 60.0 && in.nowMs - lastDetMs_ < kClickFreshMs && blockUntilMs_ <= in.nowMs && !down_ &&
                                   in.nowMs - lastSwitchMs_ > 600;
                if (ready && std::fabs(ang) >= kFreeStartDeg) {
                    freeWheel_ = true;
                    freeHand_ = hand;
                    rollAxis_ = hd.wristAxis;
                    rollN0_ = freeRef_;
                    rollAcc_ = ang - (ang > 0 ? kFreeStartDeg : -kFreeStartDeg);
                    rollLast_ = ang;
                    freeLastMoveMs_ = in.nowMs;
                    lockFromHistory(hand);
                    Log(0, "servo : molette (roulis du poignet sans pincement, %.0f deg)", ang);
                } else if (ready) {
                    // la reference suit la main lentement (une derive lente ne declenche rien)
                    const double a = std::min(1.0, dtS / kFreeRefTauS);
                    const emulation::Vec3 m = freeRef_ * static_cast<float>(1.0 - a) + n1 * static_cast<float>(a);
                    if (emulation::Length(m) > 1e-4f) freeRef_ = emulation::Normalized(m);
                } else {
                    freeRef_ = n1;  // hors conditions (doigt qui bouge, pointeur pas arrive) : on repart de la position courante
                }
            } else {
                const double ang = rollOf(rollAxis_, rollN0_, n1);
                double d = ang - rollLast_;
                while (d > 180.0) d -= 360.0;
                while (d < -180.0) d += 360.0;
                rollLast_ = ang;
                rollAcc_ += d;
                if (std::fabs(d) > 0.4) freeLastMoveMs_ = in.nowMs;
                int steps = 0;
                while (rollAcc_ >= wheelStepDeg_) {
                    rollAcc_ -= wheelStepDeg_;
                    ++steps;
                }
                while (rollAcc_ <= -wheelStepDeg_) {
                    rollAcc_ += wheelStepDeg_;
                    --steps;
                }
                out.wheelSteps += wheelInvert_ ? -steps : steps;
                if (in.nowMs - freeLastMoveMs_ > kFreeIdleMs || fpSpeed_ > 3.0 * kFreeCalmPxPerS) {
                    freeWheel_ = false;
                    freeRef_ = n1;
                    if (!prelatched_) locked_ = false;
                }
            }
        } else if (axisOk) {  // pincement / menu : la reference suit la main, la molette libre repart de la position courante a la fin
            const BrainHand& hd = in.hand[hand];
            freeRef_ = emulation::Normalized(hd.palmUp - hd.wristAxis * emulation::Dot(hd.palmUp, hd.wristAxis));
            freeRefValid_ = true;
            freeRefHand_ = hand;
        } else {
            freeRefValid_ = false;
        }
    }

    // 3) bouton. Un clic n'est envoye que si : le pointeur a ete VU recemment (MSFS ignore les clics quand il est masque), il est a moins de 45 px du bout du doigt (sinon
    //    le clic tombe a cote), le pincement est arme (commence dans l'image, main choisie), dure au moins 110 ms (pas un parasite du suivi des mains) et n'est pas le geste
    //    d'ouverture du menu (deux mains). Un pincement qui n'a pas pu cliquer dans les 700 ms est abandonne (pas de clic tardif). Un pincement plus court que 110 ms mais
    //    d'au moins 50 ms est un « tap » (clic bref). Pendant un appui, rien de tout cela ne retient plus le bouton (glisser).
    const bool canClick = mode_ == BrainMode::Track && ctl_.HaveModel() && haveFinger_ && (in.nowMs - lastDetMs_ < kClickFreshMs) && errNowPx_ <= kClickMaxErrPx && blockUntilMs_ <= in.nowMs;
    const bool both = bothRaw;
    if (hand >= 0 && edge[hand]) {
        pinchStartMs_ = in.nowMs;
        pinchClicked_ = false;
        skipWhy_.clear();
    }
    bool wheelUsed = false;
    if (tapUp_) {
        out.buttonEvent = 0;
        tapUp_ = false;
        down_ = false;
    } else {
        bool want = false;
        const uint64_t held = pinch ? in.nowMs - pinchStartMs_ : 0;
        if (hand >= 0 && pinch && !both && !wheelMode_) {
            if (down_) {
                want = true;
            } else if (held >= kClickDebounceMs && held <= kClickPendingMaxMs) {
                want = canClick;
                if (!canClick) {
                    char b[96];
                    if (mode_ != BrainMode::Track || !ctl_.HaveModel() || in.nowMs - lastDetMs_ >= kClickFreshMs) std::snprintf(b, sizeof b, "pointeur MSFS non detecte");
                    else if (!haveFinger_) std::snprintf(b, sizeof b, "doigt hors image");
                    else std::snprintf(b, sizeof b, "pointeur a %.0f px du doigt", errNowPx_);
                    skipWhy_ = b;
                }
            }
        }
        if (hand >= 0 && pinch && both) skipWhy_ = "deux mains pincees";
        wheelUsed = hand >= 0 && pinch && wheelMode_;
        if (want && !down_) {
            out.buttonEvent = 1;
            down_ = true;
            pinchClicked_ = true;
            lastSendMs_ = in.nowMs;
        } else if (!want && down_) {
            out.buttonEvent = 0;
            down_ = false;
        } else if (!want && !down_ && prevSelPinch_ && !pinch && !prevBoth_ && heldMs_ >= kTapMinMs && heldMs_ < kClickDebounceMs && prevCanClick_ && hand >= 0) {  // tap
            out.buttonEvent = 1;
            down_ = true;
            tapUp_ = true;
            pinchClicked_ = true;
            lastSendMs_ = in.nowMs;
        }
    }
    if (out.buttonEvent >= 0) Log(0, "servo : bouton %s (pointeur (%.0f,%.0f), doigt (%.0f,%.0f))", out.buttonEvent == 1 ? "ENFONCE" : "relache", lastDetPx_.x, lastDetPx_.y, haveFinger_ ? fingerPx_.x : -1.0, haveFinger_ ? fingerPx_.y : -1.0);
    // clic differe : des que le pointeur arrive sous le doigt (verrouille), le clic que le pincement voulait faire part
    if (deferTap_ && out.buttonEvent < 0 && !down_ && !pinch && hand == deferHand_ && canClick) {
        out.buttonEvent = 1;
        down_ = true;
        tapUp_ = true;
        deferTap_ = false;
        lastSendMs_ = in.nowMs;
        Log(0, "servo : bouton ENFONCE (clic differe : le pointeur est arrive apres la fin du pincement) (pointeur (%.0f,%.0f), doigt (%.0f,%.0f))", lastDetPx_.x, lastDetPx_.y, fingerPx_.x, fingerPx_.y);
    }
    // fin d'un pincement sans clic : on dit pourquoi (journal) ; si c'est parce que le pointeur n'etait pas encore arrive, le clic est differe (600 ms)
    if (prevSelPinch_ && !pinch && !pinchClicked_ && !prevWheelUsed_ && heldMs_ >= kTapMinMs) {
        const bool far = (skipWhy_.empty() ? heldMs_ < kClickDebounceMs : skipWhy_ != "deux mains pincees") && heldMs_ <= kClickPendingMaxMs;
        Log(0, "servo : pincement de %.0f ms sans clic (%s)%s", static_cast<double>(heldMs_),
            !skipWhy_.empty() ? skipWhy_.c_str() : (heldMs_ < kClickDebounceMs ? "pointeur trop loin du doigt au relachement ou pas vu" : "?"), far && !prevBoth_ && hand >= 0 ? " : clic differe" : "");
        if (far && !prevBoth_ && hand >= 0 && mode_ == BrainMode::Track) {
            deferTap_ = true;
            deferHand_ = hand;
            deferUntilMs_ = in.nowMs + 600;
            deferTipW_ = lastLockedTipW_;
        }
        skipWhy_.clear();
    }
    heldMs_ = pinch ? in.nowMs - pinchStartMs_ : 0;
    prevSelPinch_ = pinch;
    prevBoth_ = both;
    prevCanClick_ = canClick;
    prevWheelUsed_ = wheelUsed;

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

    if (trace_) {
        BrainTrace trc;
        trc.t = in.nowMs;
        trc.mode = static_cast<int>(mode_);
        trc.hand = hand;
        trc.pinchRaw = hand >= 0 && pinchRaw[hand];
        trc.pinch = pinch;
        trc.down = down_;
        trc.canClick = canClick;
        if (haveFinger_) {
            trc.fingerX = fingerPx_.x;
            trc.fingerY = fingerPx_.y;
        }
        if (haveDet_) {
            trc.detX = lastDetPx_.x;
            trc.detY = lastDetPx_.y;
            trc.detAgeMs = static_cast<double>(in.nowMs - lastDetMs_);
        }
        if (havePred) {
            Px pp;
            if (Project(in.view, in.view.pose.p + DirOf({predAng.a, predAng.b}), &pp)) {
                trc.predX = pp.x;
                trc.predY = pp.y;
            }
        }
        trc.errPx = errNowPx_ < 1e8 ? errNowPx_ : -1.0;
        trc.insideBits = (inside[0] ? 1 : 0) | (inside[1] ? 2 : 0) | (in.hand[0].usable ? 4 : 0) | (in.hand[1].usable ? 8 : 0) | (prelatched_ ? 16 : 0) | (blockUntilMs_ > in.nowMs ? 32 : 0) | (freeWheel_ ? 64 : 0) | (deferTap_ ? 128 : 0);
        trc.pinchDist = hand >= 0 ? in.hand[hand].pinchDist * 1000.0f : -1.0f;
        {
            const emulation::Vec3 fwd = emulation::Rotate(in.view.pose.q, emulation::Vec3{0.0f, 0.0f, -1.0f});
            const Ang ha = AnglesOf(fwd);
            trc.headYawDeg = ha.yaw * 57.29577951;
            trc.headPitchDeg = ha.pitch * 57.29577951;
        }
        if (haveDet_) {
            trc.detYawDeg = lastDetAng_.a * 57.29577951;
            trc.detPitchDeg = lastDetAng_.b * 57.29577951;
        }
        trc.dx = out.dx;
        trc.dy = out.dy;
        trc.wheel = out.wheelSteps;
        trace_(trc);
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
