// ServoBrain.h — la logique de l'asservissement visuel du pointeur de MSFS, sans Windows / OpenXR / D3D12 (testable en simulation de bout en bout).
//
// Le « cerveau » recoit a chaque image : la vue de l'oeil de reference (pose, champ de vision, zone rendue), l'etat des deux mains, l'heure ; et, quand une
// lecture de l'image du jeu est terminee, les pixels de la ROI. Il rend : des comptes de souris, un evenement de bouton, et la ROI a lire pour l'image courante.
//   Recherche  : balayage de l'image par dalles jusqu'a voir la fleche de MSFS pendant 4 images de suite ;
//   Mesure     : quatre salves de comptes connus (GainProbe) -> gain (rad / compte) et latence ;
//   Suivi      : ServoController (observateur + correction lente) vers la direction du bout de l'index vu du meme oeil.
// L'adaptateur de la couche (layer/servo.cpp) ne fait que : lire les pixels (D3D12), traduire les mains OpenXR, injecter les comptes.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "emulation/PoseMath.h"
#include "servo/GainProbe.h"
#include "servo/PointerDetect.h"
#include "servo/ServoController.h"
#include "servo/ServoMath.h"

namespace hands::servo {

struct BrainHand {
    bool usable = false;                 // articulations valides
    bool pinch = false;                  // pincement (deja avec hysteresis)
    bool extended = false;               // index tendu (aide au choix de la main)
    emulation::Vec3 tip;                 // bout reel du doigt (articulation du bout + rayon)
    emulation::Vec3 knuckle;             // base de l'index : bouge peu pendant un pincement
    emulation::Vec3 wristAxis;           // poignet -> base du majeur (axe du roulis) ; nul si inconnu
    emulation::Vec3 palmUp;              // « haut » de la paume : sert a mesurer le roulis du poignet (molette)
    float pinchDist = -1.0f;             // distance pouce - bout du doigt (m) ; < 0 : inconnue (pre-verrou du pointeur quand le pouce s'approche)
};

struct BrainRoi {
    bool valid = false;
    int x = 0, y = 0, w = 0, h = 0;      // pixels de la ressource
    double expX = -1, expY = -1;         // position attendue du pointeur (image complete), -1 : inconnue
    uint64_t imageSeq = 0;               // numero d'appel de l'image lue
    EyeView view;                        // vue au moment de l'image
};

struct BrainFrame {
    uint64_t nowMs = 0;
    EyeView view;
    BrainHand hand[2];                   // 0 gauche, 1 droite
    bool menuOpen = false;
    bool readerFree = true;              // une nouvelle lecture peut etre demandee
};

struct BrainOutput {
    int dx = 0, dy = 0;                  // comptes de souris a injecter
    int buttonEvent = -1;                // -1 rien, 0 relacher, 1 enfoncer
    int wheelSteps = 0;                  // crans de molette (+ : vers le haut / avant) a injecter
    BrainRoi request;                    // valid : lire cette ROI de l'image courante
};

enum class BrainMode { Acquire, Probe, Track };

// Une ligne par image pour l'analyse hors ligne (layer/servo.cpp l'ecrit dans logs/servo_trace.csv).
struct BrainTrace {
    uint64_t t = 0;
    int mode = 0, hand = -1;
    bool pinchRaw = false, pinch = false, down = false, canClick = false;
    double fingerX = -1, fingerY = -1;   // bout du doigt (pixels de l'image), -1 : pas de doigt exploitable
    double detX = -1, detY = -1, detAgeMs = -1;  // derniere detection du pointeur
    double predX = -1, predY = -1;       // position du pointeur selon le modele
    double errPx = -1;                   // ecart cible / modele
    int insideBits = 0;                  // 1/2 : doigt gauche/droit dans l'image, 4/8 : main gauche/droite exploitable, 16 : pre-verrou, 32 : pause des comptes,
                                         // 64 : molette libre, 128 : clic differe, 256 : glisser (bouton enfonce, doigt parti), 512 : re-ancrage attendu apres un glisser
    float pinchDist = -1.0f;             // mm
    double headYawDeg = 0, headPitchDeg = 0, detYawDeg = 0, detPitchDeg = 0;  // direction de la tete et du pointeur vu (cap / site, degres, repere du jeu)
    double headRollDeg = 0;              // roulis de la tete (degres)
    double plant[4] = {0, 0, 0, 0};      // reponse estimee de MSFS (pixels par compte, [m11 m12 m21 m22])
    int dx = 0, dy = 0, wheel = 0;
};

class ServoBrain {
public:
    // Journal (niveau : 0 info, 1 avertissement) et diagnostic (tag, pixels RGBA de la ROI, detection (-1 : aucune), doigt (-100000 : hors image)).
    using LogFn = std::function<void(int, const std::string&)>;
    using DiagFn = std::function<void(const char*, const uint8_t*, const BrainRoi&, int, int, int, int)>;
    void SetLogger(LogFn f) { log_ = std::move(f); }
    void SetDiag(DiagFn f) { diag_ = std::move(f); }
    using TraceFn = std::function<void(const BrainTrace&)>;
    void SetTrace(TraceFn f) { trace_ = std::move(f); }

    void SetTipFilter(float minCutoffHz, float beta) { tipMinHz_ = minCutoffHz; tipBeta_ = beta; }  // reglage du lissage du bout du doigt (tests : 1000 = sans lissage)
    void SetWheel(double stepDeg, bool invert) { wheelStepDeg_ = stepDeg; wheelInvert_ = invert; }
    void Reset();                        // oublie tout (garde le gain mesure)
    void Recalibrate();                  // oublie aussi le gain mesure et refait la mesure (menu « Remesurer »)
    // Mesure du gain au demarrage (4 salves, main et tete immobiles). Faux (defaut) : on part des valeurs a priori (gain 1.02 px/compte, latence 2) : le regulateur tient
    // des gains de 0.6 a 1.6 et une latence a +-1 (tests), alors qu'une mesure qui ne trouve jamais un pointeur « stable » (tete qui bouge) retarde le suivi de plusieurs secondes.
    void SetAutoProbe(bool on) { autoProbe_ = on; }
    void SetControlGain(double k) { params_.kControl = k; ctl_ = ServoController(params_); }  // part de l'erreur predite corrigee a chaque image (tests / reglage)
    void OnPixels(const uint8_t* rgba, const BrainRoi& req);
    BrainOutput Frame(const BrainFrame& inRaw);

    BrainMode Mode() const { return mode_; }
    const std::string& StatusText() const { return status_; }
    double ErrorPx() const { return errPx_; }
    int Hand() const { return hand_; }
    bool ButtonDown() const { return down_; }
    const ServoParams& Params() const { return params_; }
    // reponse estimee de MSFS dans le plan de l'image (pixels par compte) ; rotation en degres
    void Plant(double m[4]) const { for (int i = 0; i < 4; ++i) m[i] = M_[i]; }
    double PlantRotDeg() const;
    bool Tracking() const { return tracking_; }

private:
    struct Sample {
        uint64_t t = 0;
        emulation::Vec3 p;
    };
    int ChooseHand(const BrainFrame& in, const bool edge[2]);
    bool PickRoi(const EyeView& ev, const Px* finger, BrainRoi* r);
    void BuildTiles(const ImageRect& img);
    void Log(int level, const char* fmt, ...);
    void SetStatus(const BrainFrame& in, bool tracking);
    bool ComputeJac(const EyeView& view, V2 ang, Jac* out) const;
    long long CumAt(const long long* arr, uint64_t imageSeq, int lat = -1) const;  // lat < 0 : latence du modele
    void ResetStuck();
    void PlantInit();                    // M = diag(gain a priori ou mesure)
    void PlantFit(double cx, double cy, double ox, double oy);  // une paire (comptes atterris, deplacement vu en pixels)

    LogFn log_;
    DiagFn diag_;
    TraceFn trace_;
    uint64_t seq_ = 0, nowMs_ = 0;
    BrainMode mode_ = BrainMode::Acquire;
    ServoParams params_;
    ServoController ctl_;
    GainProbe probe_;
    bool probeDone_ = true;   // vrai = pas de mesure du gain (voir SetAutoProbe) ; Recalibrate le remet a faux
    bool autoProbe_ = false;
    EyeView probeView_;       // vue de reference de la mesure du gain : les positions du pointeur y sont projetees (la tete peut bouger pendant la mesure)
    bool probeViewSet_ = false;
    V2 tgtPrev_;
    uint64_t wheelPinchMs_ = 0, lastClickUpMs_ = 0;
    int wheelHand_ = -1, wheelLastDir_ = 1;
    bool reclickBlocked_ = false;
    Px lastClickFingerPx_{0, 0};
    bool tgtPrevValid_ = false;
    int probeFails_ = 0;
    std::vector<V2> bad_;  // directions (cap, site) de decors pris pour le pointeur : ignorees pendant la recherche
    uint64_t switchSinceMs_ = 0;
    int roiPhase_ = 0;
    double sumScore_ = 0;
    // detection
    bool haveDet_ = false;
    uint64_t lastDetMs_ = 0;
    V2 lastDetAng_;
    Px lastDetPx_;
    int stable_ = 0, missRun_ = 0;
    // balayage
    std::vector<std::pair<int, int>> tiles_;
    int tilePos_ = 0, tileImgW_ = 0, tileImgH_ = 0, tileW_ = 0, tileH_ = 0;
    // main, cible, bouton
    int hand_ = -1;
    bool prevPinch_[2] = {false, false};
    Sample hist_[2][24];
    int histN_[2] = {0, 0}, histHead_[2] = {0, 0};
    bool locked_ = false;
    emulation::Vec3 lockTip_, lockKnuckle_;
    emulation::OneEuro tipFilter_[2], knFilter_[2];
    uint64_t lastFrameMs_ = 0;
    double frameMsEma_ = 11.1;
    int prevTargetHand_ = -2;
    bool prevTargetLocked_ = false, haveFingerJump_ = false;
    Px jumpPrevPx_;
    float tipMinHz_ = 1.0f, tipBeta_ = 12.0f;
    bool wheelMode_ = false;             // pincement transforme en molette (roulis du poignet)
    emulation::Vec3 rollAxis_, rollN0_;
    double rollAcc_ = 0, rollLast_ = 0, wheelStepDeg_ = 16.0;
    bool wheelInvert_ = false;
    bool down_ = false, tapUp_ = false, prevSelPinch_ = false, prevBoth_ = false;
    bool prevRaw_[2] = {false, false};   // pincement brut de l'image precedente (un front se detecte sur le brut)
    bool armed_[2] = {false, false};     // pincement commence DANS l'image, main choisie : seul un pincement arme peut cliquer
    uint64_t lastPinchSeenMs_[2] = {0, 0}, lastSwitchMs_ = 0, pinchEdgeMs_[2] = {0, 0};
    bool prelatched_ = false, preArmed_ = false, deferTap_ = false;
    bool freeWheel_ = false, freeRefValid_ = false, haveFingerPrev_ = false;   // molette sans pincement
    uint64_t freeDiagMs_ = 0;                                                  // dernier diagnostic « molette libre refusee »
    int freeHand_ = -1, freeRefHand_ = -1;
    emulation::Vec3 freeRef_;
    uint64_t freeLastMoveMs_ = 0;
    double fpSpeed_ = 0.0, lastFrameDtS_ = 0.033;
    Px fingerPrevPx_;
    int deferHand_ = -1;
    uint64_t deferUntilMs_ = 0;
    emulation::Vec3 deferTipW_, lastLockedTipW_;
    uint64_t prelatchMs_ = 0, lastDetSeq_ = 0, blockUntilMs_ = 0;
    float prevDist_[2] = {1.0f, 1.0f};
    long long cumX_[64] = {}, cumY_[64] = {}, stuckBaseX_ = 0, stuckBaseY_ = 0;
    long long absCum_[64] = {};          // cumul des |comptes| envoyes (la boucle est-elle posee ?)
    int blockN_ = 0, lastJumps_ = 0;
    Px stuckRef_;
    bool prevCanClick_ = false, pinchClicked_ = false, prevWheelUsed_ = false;
    double errNowPx_ = 1e9;              // ecart cible / modele de cette image (1e9 : inconnu)
    std::string skipWhy_;                // pourquoi le pincement en cours ne clique pas (journal)
    uint64_t lastSendMs_ = 0, lastWakeMs_ = 0, pinchStartMs_ = 0, heldMs_ = 0;
    int jiggle_ = 1;
    // reponse de MSFS estimee en ligne (moindres carres recursifs) : pixels de l'image par compte. Voir Jac::m11.
    double M_[4] = {1, 0, 0, 1}, P_[4] = {0, 0, 0, 0};
    bool haveM_ = false;
    int plantFits_ = 0, plantRejects_ = 0;
    bool fitPrevValid_ = false;
    uint64_t fitPrevSeq_ = 0, noFitBeforeSeq_ = 0;
    V2 fitPrevAng_;
    // appui : le pointeur reste fige (MSFS prend tout deplacement bouton enfonce pour un glisser) ; au-dela d'un vrai deplacement du doigt, glisser en boucle ouverte
    bool pressTgtValid_ = false, dragActive_ = false, dragSent_ = false;
    V2 pressTgt_, dragPrevTgt_;
    double dragRemX_ = 0, dragRemY_ = 0;
    bool reanchorPending_ = false;       // apres un glisser : le prochain pointeur vu re-ancre le modele (un levier saisi par MSFS fige le pointeur)
    uint64_t reanchorAfterSeq_ = 0, reanchorUntilMs_ = 0;
    // etat de la derniere image (pour l'annotation des diagnostics et le journal)
    Px fingerPx_;
    bool haveFinger_ = false;
    bool tracking_ = false;
    double errPx_ = 0, pxPerRad_ = 1215.0;
    // journal
    uint64_t logT_ = 0, badSince_ = 0, acquireSinceMs_ = 0;
    int nDet_ = 0, nReq_ = 0, nMiss_ = 0, nSent_ = 0;
    std::string status_ = "arrêté";
};

}  // namespace hands::servo
