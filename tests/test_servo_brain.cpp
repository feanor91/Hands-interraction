// Simulation de bout en bout de l'asservissement visuel : un « MSFS » simule (pointeur = integrateur de comptes de souris, latence L, gain inconnu, curseur
// masque apres 3 s sans mouvement) dessine une VRAIE fleche (decoupee d'une capture native) dans une image synthetique ; le cerveau la cherche (balayage),
// mesure le gain, puis suit un bout d'index qui bouge. Verifie les conventions de coordonnees, la machine a etats et la tenue aux erreurs de gain / latence.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "BmpLoader.h"
#include "TestFramework.h"
#include "servo/ServoBrain.h"

using namespace hands::servo;
namespace em = hands::emulation;

namespace {

const double kPxPerRad = 1215.0;
const double kBaseGain = 1.0 / 1193.7;

struct Sprite {
    testbmp::Img img;
    int tipX = 4, tipY = 4;  // position du bout de la fleche dans le sprite
};

struct Assets {
    testbmp::Img bgDark, bgBright;
    Sprite arrow;
    bool ok = false;
};

const Assets& GetAssets() {
    static Assets a = [] {
        Assets r;
        const std::string dir = HANDS_TESTDATA_DIR;
        testbmp::Img crop;
        r.ok = testbmp::LoadBmp(dir + "/neg_scene.bmp", &r.bgDark) && testbmp::LoadBmp(dir + "/neg_efb_labels.bmp", &r.bgBright) && testbmp::LoadBmp(dir + "/ptr_arrow_medium.bmp", &crop);
        if (r.ok) {  // sprite : zone autour de la fleche (bout en (58, 20) dans le recadrage), avec 4 px de marge
            const int x0 = 54, y0 = 16, w = 25, h = 27;
            r.arrow.img.w = w;
            r.arrow.img.h = h;
            r.arrow.img.rgba.resize(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 4; ++c) r.arrow.img.rgba[(static_cast<size_t>(y) * w + x) * 4 + c] = crop.rgba[(static_cast<size_t>(y0 + y) * crop.w + (x0 + x)) * 4 + c];
            r.arrow.tipX = 58 - x0;
            r.arrow.tipY = 20 - y0;
        }
        return r;
    }();
    return a;
}

struct Sim {
    int L = 2, readLat = 1;             // latence souris -> image du jeu, latence de lecture (images)
    double readJitter = 0.0;            // part des lectures qui prennent une image de plus (cadence irreguliere : 31 / 47 ms dans le journal reel)
    double gainRatio = 1.0;             // gain reel / gain a priori du cerveau
    double fps = 90.0;
    bool hideIdle = true;               // MSFS masque le curseur apres 3 s sans mouvement
    bool startHidden = false;
    bool brightBg = false;
    V2 ptr{0.30, -0.25};                // direction (cap, site) du pointeur au depart
    double wallYaw = 1e9;
    double headSwayDeg = 0.0;           // balancement de la tete (cap) : amplitude en degres
    double headSwayHz = 0.4;
    double headTriAmpDeg = 0.0;         // la tete tourne a vitesse CONSTANTE (onde triangulaire) : jamais immobile (essai reel : on regarde partout pendant la mesure)
    double headTriDegS = 30.0;
    double lJitter = 0.0;               // part des paquets de comptes qui arrivent une image plus tard (la latence reelle fluctue : images de 31 / 47 ms)
    std::vector<int> delayOf;
    bool headAnchored = false;          // le pointeur est colle a la tete (sinon : fixe dans le monde)
    bool pxPlant = true;                // MSFS deplace son pointeur d'un nombre CONSTANT de PIXELS de l'image par compte (mesure : 1.05 px/compte de -20 a -80 deg de site), pas de radians
    bool decoy = false;                 // une deuxieme fleche IMMOBILE (un « reflet » avec un contour noir : le pire cas pour le detecteur)
    V2 decoyAng{0.17, -0.21};
    int teleportAt = -1;                // appel ou MSFS « re-ancre » le pointeur (saut de 0.4 rad sans commande)
    V2 teleportBy{0.4, 0.1};
    // etat
    EyeView view;
    std::vector<std::pair<int, int>> cnt;
    uint64_t lastMoveMs = 0;
    uint64_t t0 = 5000;
    ServoBrain brain;
    int downs = 0, ups = 0, wheel = 0;
    double tipNoiseM = 0.0;             // bruit du suivi de la main (ecart type, metres)
    uint64_t nrng = 0x9E3779B97F4A7C15ull;
    double Gauss() { double a = 0; for (int i = 0; i < 12; ++i) { nrng ^= nrng >> 12; nrng ^= nrng << 25; nrng ^= nrng >> 27; a += static_cast<double>((nrng * 2685821657736338717ull) >> 11) / 9007199254740992.0; } return a - 6.0; }
    bool buttonDown = false;
    std::vector<std::string> logs;
    double plantRotDeg = 0.0;           // la reponse de MSFS aux comptes est tournee de cet angle dans le plan de l'image (essai reel : -50 deg avec la tete penchee de 73 deg)
    double plantGainY = 1.0;            // gain en y relatif au gain en x (essai reel : x1.1 en x pour x1.3 en y)
    bool grabOnPress = false;           // MSFS « saisit » la commande sous le pointeur a l'appui : bouton enfonce, les comptes deplacent le levier et le pointeur reste fige
    long long leverX = 0, leverY = 0;   // comptes cumules recus par le levier saisi
    long long countsWhileDown = 0;      // somme des |dx| + |dy| envoyes aux appels ou le bouton est enfonce (apres l'evenement de l'appel)
    double maxPtrMoveWhileDown = 0.0;   // distance maximale (px) du pointeur a sa position a l'appui, bouton enfonce
    Px pressPx;                         // position du pointeur (px) a l'appui
    std::vector<double> pressErr;       // ecart reel (px) pointeur - bout du doigt a chaque appui

    Sim() {
        view.fov = {-0.94f, 0.70f, 0.77f, -0.96f};
        view.rect = {0, 0, 2688, 2880};
        view.pose = {{0, 0, 0, 1}, {0, 0, 0}};
        brain.SetLogger([this](int, const std::string& m) { logs.push_back(m); });
        brain.SetAutoProbe(true);  // les tests valident la mesure ; en production elle n'est faite que sur demande (menu « Remesurer ») : voir les tests « sans mesure »
        brain.Recalibrate();
    }

    double TrueGain() const { return kBaseGain * gainRatio; }

    em::Vec3 WorldPoint(V2 ang, double dist) const { return view.pose.p + DirOf({ang.a, ang.b}) * static_cast<float>(dist); }
    // position MONDE du pointeur (colle a la tete : on tourne sa direction avec la tete)
    em::Vec3 PointerWorld() const {
        em::Vec3 d = DirOf({ptr.a, ptr.b});
        if (headAnchored) d = em::Rotate(view.pose.q, d);
        return view.pose.p + d;
    }
    Px PixelOf(const em::Vec3& p) const {
        Px out;
        Project(view, p, &out);
        return out;
    }

    bool Visible(uint64_t now) const { return !hideIdle || (lastMoveMs != 0 && now - lastMoveMs < 3000); }

    void Render(const BrainRoi& roi, uint64_t now, std::vector<uint8_t>* px) const {
        const BrainRoi& r = roi;
        const Assets& a = GetAssets();
        const testbmp::Img& bg = brightBg ? a.bgBright : a.bgDark;
        px->resize(static_cast<size_t>(r.w) * r.h * 4);
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x) {
                const int sx = (x + r.x) % bg.w, sy = (y + r.y) % bg.h;
                for (int c = 0; c < 4; ++c) (*px)[(static_cast<size_t>(y) * r.w + x) * 4 + c] = bg.rgba[(static_cast<size_t>(sy) * bg.w + sx) * 4 + c];
            }
        auto paste = [&](const Px& p) {
            const int tipX = static_cast<int>(std::floor(p.x)), tipY = static_cast<int>(std::floor(p.y));
            for (int y = 0; y < a.arrow.img.h; ++y)
                for (int x = 0; x < a.arrow.img.w; ++x) {
                    const int gx = tipX - a.arrow.tipX + x - r.x, gy = tipY - a.arrow.tipY + y - r.y;
                    if (gx < 0 || gy < 0 || gx >= r.w || gy >= r.h) continue;
                    for (int c = 0; c < 4; ++c) (*px)[(static_cast<size_t>(gy) * r.w + gx) * 4 + c] = a.arrow.img.rgba[(static_cast<size_t>(y) * a.arrow.img.w + x) * 4 + c];
                }
        };
        if (decoy) paste(PixelOf(WorldPoint(decoyAng, 1.0)));
        if (Visible(now)) paste(PixelOf(PointerWorld()));
    }
};

struct Frame {
    V2 finger{0.05, -0.12};  // direction du bout de l'index vue de l'oeil
    double dist = 0.45;
    bool pinch = false;
    bool present = true;
    bool menuOpen = false;
    double rollDeg = 0.0;               // roulis du poignet de la main droite
    bool leftPresent = false, leftPinch = false;  // la main gauche (geste d'ouverture du menu)
    double pinchDist = -1.0;            // distance pouce - doigt de la main droite (m), -1 : inconnue
    bool fixKnuckle = false;            // la base de l'index ne bouge pas (seul le bout glisse vers le pouce)
};

struct Metrics {
    std::vector<double> err;
};

// Joue `n` appels ; `script(k)` donne l'etat du doigt a l'appel k.
Metrics Run(Sim& s, int n, const std::function<Frame(int)>& script) {
    Metrics m;
    s.cnt.assign(static_cast<size_t>(n) + 8, {0, 0});
    s.delayOf.assign(static_cast<size_t>(n) + 8, 0);
    struct Inflight {
        bool active = false;
        int at = 0;
        BrainRoi req;
        std::vector<uint8_t> px;
    } fl;
    const double dtMs = 1000.0 / s.fps;
    if (s.startHidden) s.lastMoveMs = 0;
    else s.lastMoveMs = s.t0;
    for (int k = 0; k < n; ++k) {
        const uint64_t now = s.t0 + static_cast<uint64_t>(std::llround(k * dtMs));
        if (s.headSwayDeg != 0.0) {  // la tete se balance autour de l'axe vertical (la vue de l'oeil tourne)
            const double th = s.headSwayDeg * 3.14159265358979 / 180.0 * std::sin(2.0 * 3.14159265358979 * s.headSwayHz * k * dtMs / 1000.0);
            s.view.pose.q = {0.0f, static_cast<float>(std::sin(th / 2)), 0.0f, static_cast<float>(std::cos(th / 2))};
        }
        if (s.headTriAmpDeg != 0.0) {
            const double A = s.headTriAmpDeg, per = 4.0 * A / s.headTriDegS, ph = std::fmod(k * dtMs / 1000.0, per) / per;  // 0..1
            const double th = (ph < 0.25 ? 4.0 * ph : (ph < 0.75 ? 2.0 - 4.0 * ph : 4.0 * ph - 4.0)) * A * 3.14159265358979 / 180.0;
            s.view.pose.q = {0.0f, static_cast<float>(std::sin(th / 2)), 0.0f, static_cast<float>(std::cos(th / 2))};
        }
        // le « jeu » applique les comptes envoyes il y a L appels
        for (int dj = 0; dj < 2; ++dj) {
            const int j = k - s.L - dj;
            if (j < 0 || s.delayOf[static_cast<size_t>(j)] != dj) continue;
            const auto c = s.cnt[static_cast<size_t>(j)];
            if (c.first != 0 || c.second != 0) {
                if (s.grabOnPress && s.buttonDown) {
                    s.leverX += c.first;  // le levier saisi suit la souris, le pointeur reste fige
                    s.leverY += c.second;
                } else if (s.pxPlant) {
                    Px q = s.PixelOf(s.PointerWorld());
                    const double g = s.TrueGain() * kPxPerRad;
                    const double u = g * c.first, v = g * s.plantGainY * c.second;
                    const double rr = s.plantRotDeg * 3.14159265358979 / 180.0, cr = std::cos(rr), sr = std::sin(rr);
                    q.x += u * cr - v * sr;
                    q.y += u * sr + v * cr;
                    const Ang an = AnglesOf(RayDir(s.view, q));
                    s.ptr = {an.yaw, an.pitch};
                } else {
                    s.ptr.a += s.TrueGain() * c.first;
                    s.ptr.b += -s.TrueGain() * c.second;
                }
                s.ptr.a = std::min(s.ptr.a, s.wallYaw);
                s.lastMoveMs = now;
            }
        }
        if (k == s.teleportAt) {
            s.ptr.a += s.teleportBy.a;
            s.ptr.b += s.teleportBy.b;
        }
        if (fl.active && k >= fl.at) {
            s.brain.OnPixels(fl.px.data(), fl.req);
            fl.active = false;
        }
        const Frame f = script(k);
        BrainFrame in;
        in.nowMs = now;
        in.view = s.view;
        in.menuOpen = f.menuOpen;
        in.readerFree = !fl.active;
        em::Vec3 tip = s.WorldPoint(f.finger, f.dist);
        const em::Vec3 trueTip = tip;
        if (s.tipNoiseM > 0) tip = tip + em::Vec3{static_cast<float>(s.tipNoiseM * s.Gauss()), static_cast<float>(s.tipNoiseM * s.Gauss()), static_cast<float>(s.tipNoiseM * s.Gauss())};
        BrainHand& h = in.hand[1];
        h.usable = f.present;
        h.pinch = f.pinch;
        h.pinchDist = static_cast<float>(f.pinchDist);
        h.extended = true;
        h.tip = tip;
        h.knuckle = (f.fixKnuckle ? s.WorldPoint({0.05, -0.12}, 0.45) : tip) + em::Vec3{0.0f, -0.03f, 0.06f};
        h.wristAxis = em::Vec3{0.0f, 0.0f, -1.0f};
        {
            const double r = f.rollDeg * 3.14159265358979 / 180.0;
            h.palmUp = em::Vec3{static_cast<float>(-std::sin(r)), static_cast<float>(std::cos(r)), 0.0f};
        }
        BrainHand& hl = in.hand[0];
        hl.usable = f.leftPresent;
        hl.pinch = f.leftPinch;
        hl.extended = false;
        hl.tip = tip + em::Vec3{-0.3f, 0.0f, 0.0f};
        hl.knuckle = hl.tip + em::Vec3{0.0f, -0.03f, 0.06f};
        const BrainOutput out = s.brain.Frame(in);
        s.cnt[static_cast<size_t>(k)] = {out.dx, out.dy};
        s.delayOf[static_cast<size_t>(k)] = (s.lJitter > 0.0 && (s.Gauss() + 6.0) / 12.0 < s.lJitter) ? 1 : 0;
        s.wheel += out.wheelSteps;
        if (out.buttonEvent == 1) {
            ++s.downs;
            s.buttonDown = true;
        }
        if (out.buttonEvent == 0) {
            ++s.ups;
            s.buttonDown = false;
        }
        if (out.request.valid) {
            fl.active = true;
            fl.at = k + s.readLat + ((s.readJitter > 0.0 && (s.Gauss() + 6.0) / 12.0 < s.readJitter) ? 1 : 0);
            fl.req = out.request;
            s.Render(out.request, now, &fl.px);
        }
        const Px pp = s.PixelOf(s.PointerWorld()), fp = s.PixelOf(trueTip);
        m.err.push_back(std::hypot(pp.x - fp.x, pp.y - fp.y));
        if (out.buttonEvent == 1) {
            s.pressPx = pp;
            s.pressErr.push_back(m.err.back());
        }
        if (s.buttonDown) {
            s.countsWhileDown += std::abs(out.dx) + std::abs(out.dy);
            s.maxPtrMoveWhileDown = std::max(s.maxPtrMoveWhileDown, std::hypot(pp.x - s.pressPx.x, pp.y - s.pressPx.y));
        }
    }
    return m;
}

double MaxErr(const Metrics& m, int from, int to) {
    double v = 0;
    for (int i = from; i < to && i < static_cast<int>(m.err.size()); ++i) v = std::max(v, m.err[static_cast<size_t>(i)]);
    return v;
}

double RmsErr(const Metrics& m, int from, int to) {
    double s = 0;
    int n = 0;
    for (int i = from; i < to && i < static_cast<int>(m.err.size()); ++i, ++n) s += m.err[static_cast<size_t>(i)] * m.err[static_cast<size_t>(i)];
    return n ? std::sqrt(s / n) : 0.0;
}

}  // namespace

TEST(ServoBrain_demarrage_a_froid_recherche_mesure_suivi) {
    CHECK(GetAssets().ok);
    Sim s;
    const Metrics m = Run(s, 700, [](int) { return Frame(); });
    std::printf("      demarrage : mode %d, ecart au repos (calls 600-700) max %.1f px, gain mesure x%.2f (reel x%.2f), L estime %d (reel %d), clics %d/%d\n", static_cast<int>(s.brain.Mode()),
                MaxErr(m, 600, 700), s.brain.Params().radPerCountX / kBaseGain, s.gainRatio, s.brain.Params().latencyFrames, s.L, s.downs, s.ups);
    CHECK(s.brain.Mode() == BrainMode::Track);
    CHECK(MaxErr(m, 600, 700) < 6.0);
    CHECK(std::fabs(s.brain.Params().radPerCountX / (kBaseGain * s.gainRatio) - 1.0) < 0.1);
    CHECK(std::abs(s.brain.Params().latencyFrames - s.L) <= 1);
    CHECK_EQ(s.downs, 0);  // aucun clic sans pincement
}

TEST(ServoBrain_suit_un_doigt_qui_bouge_puis_se_pose) {
    Sim s;
    // le doigt part a (0.05,-0.12) ; a partir de l'appel 500 il glisse de 0.15 rad en 0.5 s vers la gauche et en bas, puis se repose
    auto script = [](int k) {
        Frame f;
        const double t = k < 500 ? 0.0 : std::min(1.0, (k - 500) / 45.0);
        const double e = 10 * t * t * t - 15 * t * t * t * t + 6 * t * t * t * t * t;
        f.finger = {0.05 - 0.15 * e, -0.12 - 0.08 * e};
        return f;
    };
    const Metrics m = Run(s, 800, script);
    std::printf("      poursuite : ecart RMS pendant le geste %.1f px (max %.1f), au repos 60 images apres %.1f px\n", RmsErr(m, 500, 550), MaxErr(m, 500, 550), MaxErr(m, 740, 800));
    CHECK(s.brain.Mode() == BrainMode::Track);
    CHECK(RmsErr(m, 500, 550) < 60.0);
    CHECK(MaxErr(m, 740, 800) < 6.0);
}

TEST(ServoBrain_curseur_masque_au_depart_le_reveil_le_fait_apparaitre) {
    Sim s;
    s.startHidden = true;
    const Metrics m = Run(s, 800, [](int) { return Frame(); });
    std::printf("      curseur masque : mode %d, ecart au repos %.1f px\n", static_cast<int>(s.brain.Mode()), MaxErr(m, 700, 800));
    CHECK(s.brain.Mode() == BrainMode::Track);
    CHECK(MaxErr(m, 700, 800) < 6.0);
}

TEST(ServoBrain_tient_aux_gains_et_latences_differents) {
    for (int L : {1, 2, 3}) {
        for (int r : {1, 2}) {
            for (double ratio : {0.6, 1.0, 1.6}) {
                Sim s;
                s.L = L;
                s.readLat = r;
                s.gainRatio = ratio;
                const Metrics m = Run(s, 700, [](int) { return Frame(); });
                std::printf("      L=%d r=%d gain x%.1f : mode %d, ecart au repos %.1f px, gain mesure x%.2f, L estime %d\n", L, r, ratio, static_cast<int>(s.brain.Mode()),
                            MaxErr(m, 620, 700), s.brain.Params().radPerCountX / (kBaseGain * ratio), s.brain.Params().latencyFrames);
                CHECK(s.brain.Mode() == BrainMode::Track);
                CHECK(MaxErr(m, 620, 700) < 8.0);
            }
        }
    }
}

TEST(ServoBrain_pincement_clique_une_fois_et_fait_glisser) {
    Sim s;
    auto script = [](int k) {
        Frame f;
        const bool drag = k >= 500 && k < 620;
        f.pinch = drag;
        const double t = std::min(1.0, std::max(0.0, (k - 520) / 60.0));
        f.finger = {0.05 - 0.10 * t, -0.12};
        return f;
    };
    const Metrics m = Run(s, 760, script);
    std::printf("      glisser : %d enfoncement(s), %d relachement(s), ecart apres glisser %.1f px\n", s.downs, s.ups, MaxErr(m, 700, 760));
    CHECK_EQ(s.downs, 1);
    CHECK_EQ(s.ups, 1);
    CHECK(MaxErr(m, 700, 760) < 8.0);
}

TEST(ServoBrain_pas_de_clic_si_le_pointeur_n_est_pas_repere) {
    Sim s;
    s.startHidden = true;
    s.hideIdle = true;
    // pincement des le debut : tant que le pointeur n'est pas suivi, aucun bouton
    const Metrics m = Run(s, 40, [](int) {
        Frame f;
        f.pinch = true;
        return f;
    });
    (void)m;
    CHECK_EQ(s.downs, 0);
}

TEST(ServoBrain_menu_ouvert_pas_de_commande_ni_de_bouton) {
    Sim s;
    int sent = 0;
    const Metrics m = Run(s, 600, [](int k) {
        Frame f;
        f.menuOpen = k >= 450;
        f.pinch = k >= 470 && k < 500;
        return f;
    });
    (void)m;
    for (int k = 460; k < 600; ++k) sent += std::abs(s.cnt[static_cast<size_t>(k)].first) + std::abs(s.cnt[static_cast<size_t>(k)].second);
    std::printf("      menu : comptes envoyes pendant l'ouverture %d, enfoncements %d\n", sent, s.downs);
    CHECK_EQ(s.downs, 0);
}

TEST(ServoBrain_fond_clair_avec_etiquettes_blanches_converge_quand_meme) {
    Sim s;
    s.brightBg = true;
    s.ptr = {0.25, -0.30};
    const Metrics m = Run(s, 900, [](int) { return Frame(); });
    std::printf("      fond clair : mode %d, ecart au repos %.1f px\n", static_cast<int>(s.brain.Mode()), MaxErr(m, 800, 900));
    CHECK(s.brain.Mode() == BrainMode::Track);
    CHECK(MaxErr(m, 800, 900) < 10.0);
}

TEST(ServoBrain_doigt_hors_de_l_image_le_pointeur_reste_dans_l_image) {
    Sim s;
    // le doigt part dans l'image puis a 70 degres a droite et 60 degres en bas : hors du champ de l'oeil (+40 / -55). La main n'est plus candidate : plus de commande, le
    // pointeur reste ou il est (dans l'image), le suivi reprend quand le doigt revient.
    const Metrics m = Run(s, 1000, [](int k) {
        Frame f;
        if (k >= 500 && k < 800) f.finger = {1.22, -1.05};
        return f;
    });
    const Px p = s.PixelOf(s.PointerWorld());
    long long sent = 0;
    for (int k = 600; k < 800; ++k) sent += std::abs(s.cnt[static_cast<size_t>(k)].first) + std::abs(s.cnt[static_cast<size_t>(k)].second);
    std::printf("      doigt hors image : pointeur final en (%.0f, %.0f), %lld comptes envoyes pendant l'absence, ecart apres le retour %.1f px\n", p.x, p.y, sent, MaxErr(m, 950, 1000));
    CHECK(p.x > 0 && p.x < 2688 && p.y > 0 && p.y < 2880);
    CHECK(s.brain.Mode() == BrainMode::Track);
    CHECK(sent < 200);  // gardes de visibilite seulement
    CHECK(MaxErr(m, 950, 1000) < 8.0);
}

TEST(ServoBrain_deux_mains_pincees_aucun_clic_dans_le_cockpit) {
    Sim s;
    // geste d'ouverture du menu : la main droite pince, la gauche 44 ms plus tard (la droite n'a pas encore tenu 90 ms)
    const Metrics m = Run(s, 700, [](int k) {
        Frame f;
        f.pinch = k >= 500 && k < 600;
        f.leftPresent = true;
        f.leftPinch = k >= 504 && k < 600;
        return f;
    });
    (void)m;
    std::printf("      deux mains : %d enfoncement(s), %d relachement(s)\n", s.downs, s.ups);
    CHECK_EQ(s.downs, 0);
    CHECK_EQ(s.ups, 0);
}

TEST(ServoBrain_tap_bref_donne_un_clic_complet) {
    Sim s;
    const Metrics m = Run(s, 640, [](int k) {
        Frame f;
        f.pinch = k >= 520 && k < 527;  // 7 images a 90 Hz = 78 ms : entre 50 et 110 ms
        return f;
    });
    (void)m;
    std::printf("      tap : %d enfoncement(s), %d relachement(s)\n", s.downs, s.ups);
    CHECK_EQ(s.downs, 1);
    CHECK_EQ(s.ups, 1);
}

TEST(ServoBrain_parasite_de_pincement_tres_court_ne_clique_pas) {
    Sim s;
    const Metrics m = Run(s, 700, [](int k) {
        Frame f;
        f.pinch = (k >= 520 && k < 524) || (k >= 600 && k < 603);  // 44 ms et 33 ms : le suivi des mains qui « saute »
        return f;
    });
    (void)m;
    std::printf("      parasites de pincement : %d enfoncement(s)\n", s.downs);
    CHECK_EQ(s.downs, 0);
}

TEST(ServoBrain_pincement_qui_saute_une_image_pendant_l_appui_ne_relache_pas) {
    Sim s;
    const Metrics m = Run(s, 760, [](int k) {
        Frame f;
        f.pinch = k >= 500 && k < 640 && !(k >= 560 && k < 564);  // 4 images (44 ms) de trou au milieu de l'appui
        return f;
    });
    (void)m;
    std::printf("      pincement troue : %d enfoncement(s), %d relachement(s)\n", s.downs, s.ups);
    CHECK_EQ(s.downs, 1);
    CHECK_EQ(s.ups, 1);
}

TEST(ServoBrain_pointeur_qui_ne_peut_pas_atteindre_le_doigt_aucun_clic) {
    Sim s;
    s.wallYaw = 0.0;  // mur de MSFS : le pointeur ne va pas plus a droite que le cap 0
    const Metrics m = Run(s, 900, [](int k) {
        Frame f;
        f.finger = {0.45, -0.12};  // dans l'image, mais derriere le mur
        f.pinch = k >= 700 && k < 800;
        return f;
    });
    (void)m;
    const Px p = s.PixelOf(s.PointerWorld()), q = s.PixelOf(s.WorldPoint({0.45, -0.12}, 0.45));
    std::printf("      mur : pointeur (%.0f, %.0f), doigt (%.0f, %.0f), %d enfoncement(s)\n", p.x, p.y, q.x, q.y, s.downs);
    CHECK_EQ(s.downs, 0);
}

TEST(ServoBrain_doigt_hors_image_un_pincement_ne_clique_pas_et_ne_prend_pas_la_main) {
    Sim s;
    const Metrics m = Run(s, 800, [](int k) {
        Frame f;
        f.finger = k < 600 ? V2{0.05, -0.12} : V2{1.22, -1.05};  // a 70 deg a droite, 60 deg en bas : hors du champ de l'oeil
        f.pinch = k >= 650 && k < 750;
        return f;
    });
    (void)m;
    std::printf("      doigt hors image + pincement : %d enfoncement(s), main choisie %d\n", s.downs, s.brain.Hand());
    CHECK_EQ(s.downs, 0);
    CHECK_EQ(s.brain.Hand(), -1);
}

TEST(ServoBrain_reflet_immobile_avec_contour_noir_est_ecarte_apres_la_mesure) {
    Sim s;
    s.decoy = true;  // une fleche immobile pres du doigt : le balayage la trouve en premier
    const Metrics m = Run(s, 1400, [](int) { return Frame(); });
    std::printf("      reflet : mode %d, ecart au repos %.1f px\n", static_cast<int>(s.brain.Mode()), MaxErr(m, 1300, 1400));
    bool sawRetry = false;
    for (const std::string& l : s.logs) sawRetry = sawRetry || l.find("ne repond pas") != std::string::npos;
    CHECK(sawRetry);
    CHECK(s.brain.Mode() == BrainMode::Track);
    CHECK(MaxErr(m, 1300, 1400) < 8.0);
}

TEST(ServoBrain_tete_qui_se_balance_pointeur_fixe_dans_le_monde) {
    Sim s;
    s.headSwayDeg = 8.0;  // +-8 degres a 0.4 Hz (pic ~20 deg/s)
    const Metrics m = Run(s, 1100, [](int) { return Frame(); });
    std::printf("      balancement, pointeur monde : mode %d, ecart RMS apres 8 s %.1f px (max %.1f)\n", static_cast<int>(s.brain.Mode()), RmsErr(m, 700, 1100), MaxErr(m, 700, 1100));
    CHECK(s.brain.Mode() == BrainMode::Track);
    CHECK(RmsErr(m, 700, 1100) < 15.0);
}

TEST(ServoBrain_mesure_du_gain_tete_en_mouvement_continu) {
    // essai reel du 12 nov. 17:57 : la tete bouge en permanence, le pointeur est fixe dans le monde. La mesure ne trouvait jamais un pointeur « stable » en pixels (0 salve
    // envoyee, « mesure du gain impossible », puis le vrai pointeur mis sur la liste des decors).
    Sim s;
    s.headTriAmpDeg = 25.0;  // +-25 deg a 30 deg/s, sans arret
    const Metrics m = Run(s, 1800, [](int) { return Frame(); });
    bool measured = false, blacklisted = false, impossible = false;
    for (const std::string& l : s.logs) {
        measured = measured || l.find("gain mesure") != std::string::npos;
        blacklisted = blacklisted || l.find("ne repond pas") != std::string::npos;
        impossible = impossible || l.find("impossible") != std::string::npos;
    }
    std::printf("      mesure tete mobile : mesure %d, decor %d, impossible %d, mode %d, ecart RMS final %.1f px\n", measured, blacklisted, impossible, static_cast<int>(s.brain.Mode()), RmsErr(m, 1500, 1800));
    CHECK(measured);
    CHECK(!blacklisted);
    CHECK(!impossible);
    CHECK(s.brain.Mode() == BrainMode::Track);
    CHECK(RmsErr(m, 1500, 1800) < 15.0);
}

TEST(ServoBrain_sans_mesure_du_gain_le_suivi_demarre_tout_de_suite) {
    // production : pas de mesure au demarrage (valeurs a priori) ; le regulateur absorbe un gain et une latence differents
    const double ratios[] = {0.7, 1.0, 1.4};
    for (const double ratio : ratios) {
        Sim s;
        s.brain.SetAutoProbe(false);
        s.brain.Recalibrate();  // (le constructeur du Sim a active la mesure) : puis on la desactive proprement
        s.brain.SetAutoProbe(false);
        s.brain.Reset();
        s.gainRatio = ratio;
        s.L = 3;
        s.headSwayDeg = 10.0;
        const Metrics m = Run(s, 900, [](int) { return Frame(); });
        bool probed = false;
        for (const std::string& l : s.logs) probed = probed || l.find("quatre petites salves") != std::string::npos;
        std::printf("      sans mesure, gain x%.1f, L=3, tete mobile : mode %d, ecart RMS %.1f px (max %.1f), mesure lancee %d\n", ratio, static_cast<int>(s.brain.Mode()), RmsErr(m, 500, 900), MaxErr(m, 500, 900), probed);
        CHECK(!probed);
        CHECK(s.brain.Mode() == BrainMode::Track);
        CHECK(RmsErr(m, 500, 900) < 15.0);
    }
}

TEST(ServoBrain_tete_qui_se_balance_pointeur_colle_a_la_tete) {
    Sim s;
    s.headSwayDeg = 8.0;
    s.headAnchored = true;
    s.pxPlant = false;
    const Metrics m = Run(s, 1100, [](int) { return Frame(); });
    std::printf("      balancement, pointeur tete : mode %d, ecart RMS apres 8 s %.1f px (max %.1f)\n", static_cast<int>(s.brain.Mode()), RmsErr(m, 700, 1100), MaxErr(m, 700, 1100));
    CHECK(s.brain.Mode() == BrainMode::Track);
    CHECK(RmsErr(m, 700, 1100) < 40.0);
}

TEST(ServoBrain_pointeur_re_ancre_par_MSFS_loin_du_modele_est_retrouve) {
    Sim s;
    s.teleportAt = 700;  // le pointeur saute de ~480 px apres la convergence
    const Metrics m = Run(s, 1300, [](int) { return Frame(); });
    int back = -1;
    for (int k = 700; k < 1300; ++k) {
        bool ok = true;
        for (int j = k; j < std::min(1300, k + 30) && ok; ++j) ok = m.err[static_cast<size_t>(j)] < 8.0;
        if (ok) {
            back = k - 700;
            break;
        }
    }
    int pauses = 0;
    for (const std::string& l : s.logs) pauses += l.find("ne suit plus") != std::string::npos ? 1 : 0;
    std::printf("      saut de 0.4 rad : retour a < 8 px apres %d images (%.1f s a 90 Hz), %d pause(s) des comptes\n", back, back / 90.0, pauses);
    for (const std::string& l : s.logs)
        if (l.find("ne suit plus") != std::string::npos) std::printf("        %s\n", l.c_str());
    CHECK(back >= 0 && back < 270);  // moins de 3 s
    CHECK(s.brain.Mode() == BrainMode::Track);
}

TEST(ServoBrain_doigt_qui_tremble_le_pointeur_ne_danse_pas) {
    Sim s;
    s.tipNoiseM = 0.002;  // 2 mm de bruit sur le suivi de la main (~5 px a 45 cm)
    const Metrics m = Run(s, 900, [](int) { return Frame(); });
    double jit = 0;
    int n = 0;
    long long counts = 0;
    for (int k = 600; k < 900; ++k) {
        counts += std::abs(s.cnt[static_cast<size_t>(k)].first) + std::abs(s.cnt[static_cast<size_t>(k)].second);
        if (k > 600) {
            jit += std::fabs(m.err[static_cast<size_t>(k)] - m.err[static_cast<size_t>(k - 1)]);
            ++n;
        }
    }
    std::printf("      tremblement : ecart RMS %.1f px, variation image a image %.2f px, %lld comptes en 300 images\n", RmsErr(m, 600, 900), jit / n, counts);
    CHECK(RmsErr(m, 600, 900) < 4.0);
    CHECK(counts < 600);  // au repos, presque aucun compte : pas de « danse »
}

TEST(ServoBrain_roulis_du_poignet_pendant_un_pincement_fait_tourner_la_molette) {
    Sim s;
    s.brain.SetWheel(20.0, false);
    const Metrics m = Run(s, 800, [](int k) {
        Frame f;
        f.pinch = k >= 500 && k < 700;
        f.rollDeg = k < 520 ? 0.0 : -std::min(94.0, (k - 520) * 1.0);  // sens horaire (vu de l'utilisateur), 1 deg par image : 94 deg
        return f;
    });
    (void)m;
    std::printf("      molette : %d cran(s) pour 94 deg, %d enfoncement(s) %d relachement(s)\n", s.wheel, s.downs, s.ups);
    CHECK(s.wheel >= 3 && s.wheel <= 5);
    CHECK(s.downs == s.ups);
    CHECK(!s.buttonDown);
}

TEST(ServoBrain_molette_le_pincement_perdu_en_plein_roulis_ne_coupe_pas_la_molette) {
    // essai reel du 12 nov. : pendant le roulis la distance pouce - index passe de 10 a 23 mm (le suivi des mains lache le pincement) : la molette s'arretait apres 14 deg et
    // le pincement suivant repartait de zero
    Sim s;
    s.brain.SetWheel(20.0, false);
    const Metrics m = Run(s, 800, [](int k) {
        Frame f;
        f.pinch = k >= 500 && k < 700 && !(k >= 545 && k < 565);  // 20 images a 90 i/s = 220 ms sans pincement, en plein roulis
        f.rollDeg = k < 520 ? 0.0 : -std::min(94.0, (k - 520) * 1.0);
        return f;
    });
    (void)m;
    std::printf("      molette avec trou de pincement : %d cran(s) pour 94 deg, %d enfoncement(s) %d relachement(s)\n", s.wheel, s.downs, s.ups);
    CHECK(s.wheel >= 5 && s.wheel <= 6);
    CHECK(s.downs <= 2);
    CHECK(s.downs == s.ups);
    CHECK(!s.buttonDown);
}

TEST(ServoBrain_molette_relachement_voulu_puis_poignet_qui_revient_pas_de_cran_inverse) {
    for (const double openDist : {0.06, -1.0}) {  // pouce clairement ecarte, ou distance inconnue (seule la regle « meme sens » protege alors)
        Sim s;
        s.brain.SetWheel(20.0, false);
        const Metrics m = Run(s, 800, [&](int k) {
            Frame f;
            f.pinch = k >= 500 && k < 585;
            f.pinchDist = k >= 585 ? openDist : 0.012;
            f.rollDeg = k < 520 ? 0.0 : (k < 580 ? -(k - 520) * 1.0 : std::min(0.0, -60.0 + (k - 585) * 1.5));  // -60 deg puis retour apres le relachement
            return f;
        });
        (void)m;
        std::printf("      relachement puis retour du poignet (distance %.3f) : %d cran(s)\n", openDist, s.wheel);
        CHECK(s.wheel >= 3);
    }
}

TEST(ServoBrain_molette_premier_cran_tout_de_suite) {
    Sim s;
    s.brain.SetWheel(20.0, false);
    const Metrics m = Run(s, 800, [](int k) {
        Frame f;
        f.pinch = k >= 500 && k < 700;
        f.rollDeg = k < 520 ? 0.0 : -std::min(12.0, (k - 520) * 1.0);  // 12 deg seulement
        return f;
    });
    (void)m;
    std::printf("      molette 12 deg : %d cran(s)\n", s.wheel);
    CHECK_EQ(s.wheel, 1);
}

TEST(ServoBrain_deuxieme_clic_trop_vite_au_meme_endroit_est_ignore) {
    // pincement, relachement, nouveau pincement 110 ms apres (le pouce flotte pres du seuil) : un seul clic. Un pincement 800 ms apres : un deuxieme clic.
    Sim s;
    const Metrics m = Run(s, 900, [](int k) {
        Frame f;
        f.pinch = (k >= 500 && k < 530) || (k >= 540 && k < 570) || (k >= 640 && k < 670);
        return f;
    });
    (void)m;
    std::printf("      double clic : %d enfoncement(s), %d relachement(s)\n", s.downs, s.ups);
    CHECK_EQ(s.downs, 2);
    CHECK_EQ(s.ups, 2);
}

TEST(ServoBrain_molette_sens_inverse) {
    Sim s;
    s.brain.SetWheel(20.0, true);
    const Metrics m = Run(s, 800, [](int k) {
        Frame f;
        f.pinch = k >= 500 && k < 700;
        f.rollDeg = k < 520 ? 0.0 : -std::min(94.0, (k - 520) * 1.0);
        return f;
    });
    (void)m;
    CHECK(s.wheel <= -3);
}

TEST(ServoBrain_mur_de_MSFS_les_comptes_font_une_pause_au_lieu_de_marteler) {
    Sim s;
    s.wallYaw = 0.0;
    const Metrics m = Run(s, 1000, [](int) {
        Frame f;
        f.finger = {0.45, -0.12};  // derriere le mur : le pointeur ne bougera pas
        return f;
    });
    (void)m;
    long long sent = 0;
    for (int k = 600; k < 1000; ++k) sent += std::abs(s.cnt[static_cast<size_t>(k)].first) + std::abs(s.cnt[static_cast<size_t>(k)].second);
    int pauses = 0;
    for (const std::string& l : s.logs) pauses += l.find("ne suit plus") != std::string::npos ? 1 : 0;
    std::printf("      mur : %lld comptes en 400 images, %d pause(s)\n", sent, pauses);
    CHECK(pauses >= 1);
    CHECK(sent < 5000);  // sans la pause : 60 comptes par image = 24000
}

TEST(ServoBrain_l_autre_main_pincee_depuis_longtemps_n_empeche_pas_de_cliquer) {
    Sim s;
    // la main gauche reste pincee (posee sur une manette) ; la droite clique 120 ms 200 images plus tard : ce n'est pas le geste d'ouverture du menu
    const Metrics m = Run(s, 700, [](int k) {
        Frame f;
        f.leftPresent = true;
        f.leftPinch = k >= 300 && k < 700;
        f.pinch = k >= 520 && k < 560;
        return f;
    });
    (void)m;
    std::printf("      main gauche pincee depuis longtemps : %d enfoncement(s)\n", s.downs);
    CHECK_EQ(s.downs, 1);
}

TEST(ServoBrain_le_pre_verrou_garde_le_pointeur_la_ou_le_doigt_etait_avant_le_geste) {
    Sim s;
    // le doigt est immobile en (0.05, -0.12) puis, pendant que le pouce vient (60 -> 20 mm en 28 images), il glisse de 0.04 rad (~48 px) ; pincement a l'image 528
    const Metrics m = Run(s, 700, [](int k) {
        Frame f;
        const double t = std::min(1.0, std::max(0.0, (k - 500) / 28.0));
        f.finger = {0.05 + 0.04 * t, -0.12};
        f.pinchDist = 0.060 - 0.040 * t;
        f.fixKnuckle = true;
        f.pinch = k >= 528 && k < 640;
        return f;
    });
    // pendant le pincement (image 620) le pointeur doit etre reste pres de l'endroit d'avant le geste : l'ecart au doigt (qui a glisse de ~49 px) reste proche de 49 px
    const double errHeld = m.err[620];
    std::printf("      pre-verrou : pendant le pincement le pointeur est a %.0f px du doigt (qui a glisse de 49 px ; sans pre-verrou ~21 px)\n", errHeld);
    CHECK(errHeld > 33.0 && errHeld < 65.0);
    CHECK_EQ(s.downs, 1);
}

TEST(ServoBrain_a_30_images_par_seconde_le_retard_est_compense_en_mouvement_regulier) {
    Sim s;
    s.fps = 30.0;
    s.L = 2;
    s.readLat = 1;
    // doigt qui glisse a vitesse reguliere (0.003 rad par image = ~3.6 px par image = 110 px/s) puis a 0.011 rad par image (~13 px/image)
    auto script = [](int k) {
        Frame f;
        double x = 0.05;
        if (k > 300) x += 0.003 * std::min(100, k - 300);
        if (k > 400) x += 0.008 * std::min(30, k - 400);
        f.finger = {x, -0.12};
        return f;
    };
    const Metrics m = Run(s, 560, script);
    double slow = 0, fast = 0;
    for (int k = 350; k < 400; ++k) slow += m.err[static_cast<size_t>(k)];
    for (int k = 415; k < 430; ++k) fast += m.err[static_cast<size_t>(k)];
    slow /= 50.0;
    fast /= 15.0;
    std::printf("      mouvement regulier a 30 i/s : ecart moyen %.1f px a 3.6 px/image, %.1f px a 13 px/image\n", slow, fast);
    CHECK(slow < 8.0);
    CHECK(fast < 22.0);
}

TEST(ServoBrain_pincement_avant_l_arrivee_du_pointeur_le_clic_est_differe) {
    Sim s;
    // le doigt saute loin (la main change) et pince presque tout de suite : un pincement de 100 ms, fini bien avant que le pointeur n'arrive
    const Metrics m = Run(s, 800, [](int k) {
        Frame f;
        if (k >= 500) f.finger = {0.60, -0.55};
        f.pinch = k >= 503 && k < 510;
        return f;
    });
    (void)m;
    for (const std::string& l : s.logs)
        if (l.find("bouton") != std::string::npos || l.find("pincement") != std::string::npos) std::printf("        %s\n", l.c_str());
    bool deferred = false;
    for (const std::string& l : s.logs) deferred = deferred || l.find("clic differe") != std::string::npos;
    std::printf("      clic differe : %d enfoncement(s), %d relachement(s), journal %s\n", s.downs, s.ups, deferred ? "ok" : "absent");
    CHECK_EQ(s.downs, 1);
    CHECK_EQ(s.ups, 1);
    CHECK(deferred);
}

TEST(ServoBrain_roulis_du_poignet_sans_pincement_fait_tourner_la_molette_sans_cliquer) {
    Sim s;
    s.brain.SetWheel(20.0, false);
    const Metrics m = Run(s, 900, [](int k) {
        Frame f;
        f.rollDeg = k < 520 ? 0.0 : -std::min(94.0, (k - 520) * 1.0);  // 1 deg par image a 90 i/s : 90 deg/s
        return f;
    });
    (void)m;
    std::printf("      molette sans pincement : %d cran(s) pour 94 deg, %d enfoncement(s)\n", s.wheel, s.downs);
    CHECK(s.wheel >= 3 && s.wheel <= 5);
    CHECK_EQ(s.downs, 0);
}

TEST(ServoBrain_derive_lente_du_poignet_ou_main_qui_bouge_pas_de_molette) {
    {
        Sim s;
        // 60 deg en 10 s (6 deg/s) : un geste lent, pas un roulis
        const Metrics m = Run(s, 1400, [](int k) {
            Frame f;
            f.rollDeg = k < 500 ? 0.0 : -std::min(60.0, (k - 500) * 0.066);
            return f;
        });
        (void)m;
        std::printf("      derive lente : %d cran(s)\n", s.wheel);
        CHECK_EQ(s.wheel, 0);
    }
    {
        Sim s;
        // roulis rapide MAIS le doigt est en plein deplacement (on va vers une commande) : pas de molette
        const Metrics m = Run(s, 800, [](int k) {
            Frame f;
            const double t = std::min(1.0, std::max(0.0, (k - 520) / 40.0));
            f.finger = {0.05 + 0.25 * t, -0.12};
            f.rollDeg = k < 520 ? 0.0 : -std::min(94.0, (k - 520) * 2.0);
            return f;
        });
        (void)m;
        std::printf("      roulis pendant un deplacement : %d cran(s)\n", s.wheel);
        CHECK_EQ(s.wheel, 0);
    }
}

TEST(ServoBrain_grand_deplacement_a_30_images_par_seconde_pas_de_fausse_pause) {
    for (int L : {2, 3}) {
        Sim s;
        s.fps = 30.0;
        s.L = L;
        s.readLat = 2;
        const Metrics m = Run(s, 700, [](int k) {
            Frame f;
            if (k >= 400 && k < 500) f.finger = {0.55, -0.45};
            return f;
        });
        int pauses = 0;
        for (const std::string& l : s.logs) pauses += l.find("ne suit plus") != std::string::npos ? 1 : 0;
        std::printf("      grand deplacement 30 i/s L=%d : %d pause(s), ecart 40 images apres l'arrivee %.1f px\n", L, pauses, MaxErr(m, 460, 500));
        CHECK_EQ(pauses, 0);
        CHECK(MaxErr(m, 460, 500) < 12.0);
    }
}

TEST(ServoBrain_cadence_irreguliere_le_pointeur_ne_oscille_pas_autour_du_doigt) {
    for (double jit : {0.0, 0.3, 0.5}) {
        Sim s;
        s.fps = 30.0;
        s.L = 2;
        s.readLat = 1;
        s.readJitter = jit;
        s.gainRatio = 1.1;
        const Metrics m = Run(s, 900, [](int) { return Frame(); });
        double sum = 0, mx = 0;
        long long counts = 0;
        for (int k = 600; k < 900; ++k) {
            sum += m.err[static_cast<size_t>(k)];
            mx = std::max(mx, m.err[static_cast<size_t>(k)]);
            counts += std::abs(s.cnt[static_cast<size_t>(k)].first) + std::abs(s.cnt[static_cast<size_t>(k)].second);
        }
        std::printf("      cadence irreguliere %.0f %% : ecart moyen au repos %.1f px (max %.1f), %lld comptes en 300 images\n", jit * 100, sum / 300.0, mx, counts);
        CHECK(sum / 300.0 < 8.0);
    }
}

TEST(ServoBrain_rejoue_un_vrai_trajet_de_doigt_quasi_immobile_sans_osciller) {
    // 169 images reelles (essai du 12 nov., 11:23) : le doigt reste dans un carre de 36 x 40 px ; dans le casque le pointeur tournait autour (ecart median 33 px)
    std::vector<Px> path;
    {
        std::FILE* f = std::fopen((std::string(HANDS_TESTDATA_DIR) + "/finger_path.txt").c_str(), "r");
        CHECK(f != nullptr);
        if (!f) return;
        double x, y;
        while (std::fscanf(f, "%lf %lf", &x, &y) == 2) path.push_back({x, y});
        std::fclose(f);
    }
    CHECK(path.size() > 100);
    for (double gainRatio : {0.8, 1.0}) {
        Sim s;
        s.fps = 30.0;
        s.L = 2;
        s.readLat = 1;
        s.gainRatio = gainRatio;
        s.brain.SetTipFilter(1000.0f, 0.0f);  // le trajet est deja lisse (c'est la cible que la couche a vue)
        const int warm = 600;
        auto script = [&](int k) {
            Frame f;
            const Px p = path[std::min(path.size() - 1, static_cast<size_t>(std::max(0, k - warm)))];
            const Ang a = AnglesOf(RayDir(s.view, p));
            f.finger = {a.yaw, a.pitch};
            return f;
        };
        const int n = warm + static_cast<int>(path.size());
        const Metrics m = Run(s, n, script);
        double sum = 0, mx = 0;
        long long counts = 0;
        for (int k = warm; k < n; ++k) {
            sum += m.err[static_cast<size_t>(k)];
            mx = std::max(mx, m.err[static_cast<size_t>(k)]);
            counts += std::abs(s.cnt[static_cast<size_t>(k)].first) + std::abs(s.cnt[static_cast<size_t>(k)].second);
        }
        std::printf("      trajet reel, gain x%.1f : ecart moyen %.1f px (max %.1f), %lld comptes en %zu images\n", gainRatio, sum / path.size(), mx, counts, path.size());
        CHECK(sum / path.size() < 12.0);
    }
}

// EXPERIENCE (ignoree sans la variable HANDS_REPLAY_CSV) : rejoue le controleur sur les vraies donnees d'une trace
TEST(ServoReplay_trace_reelle) {
    const char* path = std::getenv("HANDS_REPLAY_CSV");
    if (!path) return;
    const int from = std::getenv("HANDS_REPLAY_FROM") ? std::atoi(std::getenv("HANDS_REPLAY_FROM")) : 11078, count = 120;
    std::FILE* f = std::fopen(path, "r");
    CHECK(f != nullptr);
    if (!f) return;
    char line[512];
    struct Row { double t, fx, fy, dxp, dyp, age, rdx, rdy, rpx, rpy; };
    std::vector<Row> rows;
    int idx = -1;
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == 't') continue;
        ++idx;
        if (idx < from || idx >= from + count) continue;
        double v[24];
        int n = 0;
        char* c = line;
        while (n < 24) {
            v[n++] = std::strtod(c, &c);
            if (*c != ',') break;
            ++c;
        }
        // t_ms,mode,hand,pinch_raw,pinch,down,can_click,finger_x,finger_y,det_x,det_y,det_age_ms,pred_x,pred_y,err_px,dx,dy,wheel
        rows.push_back({v[0], v[7], v[8], v[9], v[10], v[11], v[15], v[16], v[12], v[13]});
    }
    std::fclose(f);
    EyeView view;
    view.fov = {-0.94f, 0.70f, 0.77f, -0.96f};
    view.rect = {0, 0, 2688, 2880};
    view.pose = {{0, 0, 0, 1}, {0, 0, 0}};
    ServoParams p;
    p.radPerCountX = 0.85 / 1215.0;
    p.radPerCountY = -1.00 / 1215.0;
    p.latencyFrames = 2;
    ServoController ctl(p);
    auto ang = [&](double x, double y) {
        const Ang a = AnglesOf(RayDir(view, {x, y}));
        return V2{a.yaw, a.pitch};
    };
    std::printf("      row   real dx,dy   replay dx,dy   real pred   replay pred   err(px)\n");
    uint64_t seq = 1000;
    for (size_t i = 0; i < rows.size(); ++i, ++seq) {
        const Row& r = rows[i];
        if (r.age < 100) {
            const int lat = r.age < 40 ? 1 : (r.age < 55 ? 2 : 3);
            ctl.OnMeasurement(seq - lat, ang(r.dxp, r.dyp));
        }
        const ServoController::Out o = ctl.Step(seq, true, ang(r.fx, r.fy));
        const Px pp = [&] { Px q; Project(view, DirOf({o.predicted.a, o.predicted.b}), &q); return q; }();
        if (i < 45) std::printf("      %3zu   %4.0f,%4.0f    %4d,%4d    %5.0f,%5.0f   %5.0f,%5.0f   %5.0f\n", i, r.rdx, r.rdy, o.dx, o.dy, r.rpx, r.rpy, pp.x, pp.y, Norm(o.error) * 1215.0);
    }
}

TEST(ServoBrain_tete_penchee_vers_le_bas_le_gain_en_cap_change_et_le_pointeur_ne_danse_pas) {
    for (double pitchDeg : {0.0, -45.0, -65.0}) {
        Sim s;
        s.pxPlant = true;
        s.tipNoiseM = 0.002;
        const double th = pitchDeg * 3.14159265358979 / 180.0;
        s.view.pose.q = {static_cast<float>(std::sin(th / 2)), 0.0f, 0.0f, static_cast<float>(std::cos(th / 2))};  // la tete regarde vers le bas
        const double axisPitch = th;                                                                                // site de l'axe de vue
        s.ptr = {0.30, axisPitch + 0.1};
        const V2 finger{0.05, axisPitch - 0.05};
        const Metrics m = Run(s, 1100, [&](int) {
            Frame f;
            f.finger = finger;
            return f;
        });
        long long counts = 0;
        for (int k = 800; k < 1100; ++k) counts += std::abs(s.cnt[static_cast<size_t>(k)].first) + std::abs(s.cnt[static_cast<size_t>(k)].second);
        std::printf("      tete a %.0f deg : mode %d, gain mesure x%.2f (px/compte), ecart RMS au repos %.1f px (max %.1f), %lld comptes en 300 images\n", pitchDeg,
                    static_cast<int>(s.brain.Mode()), s.brain.Params().radPerCountX * kPxPerRad, RmsErr(m, 800, 1100), MaxErr(m, 800, 1100), counts);
        CHECK(s.brain.Mode() == BrainMode::Track);
        CHECK(RmsErr(m, 800, 1100) < 7.0);
        CHECK(counts < 700);
    }
}

// Experience (HANDS_EXPERIMENT=1) : amortissement de la regulation quand la latence reelle fluctue (essai reel du 12 nov. : le pointeur tourne autour du doigt vers -65 deg de site).
TEST(ServoExperiment_gain_de_regulation) {
    if (!std::getenv("HANDS_EXPERIMENT")) return;
    for (double lj : {0.0, 0.3, 0.6})
        for (double gr : {1.0, 1.3})
            for (double kc : {0.8, 0.6, 0.5, 0.4, 0.3}) {
                double worstOver = 0, sumSettle = 0, sumRms = 0;
                int nrun = 0;
                for (double pitchDeg : {-68.0}) {
                    Sim s;
                    s.fps = 30.0;
                    s.readJitter = 0.4;
                    s.lJitter = lj;
                    s.gainRatio = gr;
                    s.tipNoiseM = 0.001;
                    s.brain.SetAutoProbe(false);
                    s.brain.Reset();
                    s.brain.SetControlGain(kc);
                    const double th = pitchDeg * 3.14159265358979 / 180.0;
                    s.view.pose.q = {static_cast<float>(std::sin(th / 2)), 0.0f, 0.0f, static_cast<float>(std::cos(th / 2))};
                    s.ptr = {0.30, th + 0.1};
                    const Metrics m = Run(s, 900, [&](int k) {
                        Frame f;
                        const double t = k < 500 ? 0.0 : std::min(1.0, (k - 500) / 6.0);
                        f.finger = {0.05 - 0.25 * t, th - 0.10 + 0.10 * t};
                        return f;
                    });
                    double over = 0;
                    int settle = 0;
                    for (int k = 515; k < 900; ++k)
                        if (m.err[static_cast<size_t>(k)] > 10.0) settle = k - 515;
                    for (int k = 530; k < 900; ++k) over = std::max(over, m.err[static_cast<size_t>(k)]);
                    worstOver = std::max(worstOver, over);
                    sumSettle += settle;
                    sumRms += RmsErr(m, 700, 900);
                    ++nrun;
                }
                std::printf("      EXP lJitter %.1f gain x%.1f kControl %.1f : pire erreur apres 0.5 s %.0f px, derniere erreur >10 px a +%.0f images (moy.), RMS au repos %.1f px\n", lj, gr, kc, worstOver, sumSettle / nrun, sumRms / nrun);
            }
}

TEST(ServoBrain_doigt_et_pointeur_de_part_et_d_autre_du_pole_pas_d_emballement) {
    // essai reel du 12 nov. (tete a -28 deg, doigt a y = 2834 sur 2880, soit -77 deg de site) : l'erreur etait prise comme une difference d'angles puis ramenee en comptes par
    // une matrice presque singuliere (1 px = ~7 fois plus de cap pres du pole) : 6000 px d'« erreur », -150 / +150 comptes par image pendant 3.7 s, le pointeur tape contre le mur.
    for (double fps : {30.0, 90.0}) {
        Sim s;
        s.fps = fps;
        s.tipNoiseM = 0.001;
        const double th = -60.0 * 3.14159265358979 / 180.0;
        s.view.pose.q = {static_cast<float>(std::sin(th / 2)), 0.0f, 0.0f, static_cast<float>(std::cos(th / 2))};
        // le doigt (-87 deg de site) et le pointeur (-85 deg) sont de part et d'autre du POLE : leurs caps different de ~170 deg alors qu'ils sont a ~300 px l'un de l'autre
        s.ptr = {3.0, -85.0 * 3.14159265358979 / 180.0};
        const int n = static_cast<int>(fps * 12.0);
        const Metrics m = Run(s, n, [&](int) {
            Frame f;
            f.finger = {0.05, -87.0 * 3.14159265358979 / 180.0};
            return f;
        });
        int sat = 0;
        long long counts = 0;
        for (int k = 0; k < n; ++k) {
            const auto c = s.cnt[static_cast<size_t>(k)];
            if (std::max(std::abs(c.first), std::abs(c.second)) >= 100) ++sat;
            if (k >= n - static_cast<int>(fps * 3.0)) counts += std::abs(c.first) + std::abs(c.second);
        }
        const int from = n - static_cast<int>(fps * 3.0);
        std::printf("      doigt pres du pole, %.0f i/s : mode %d, %d images a >= 100 comptes, ecart RMS final %.1f px (max %.1f), %lld comptes sur 3 s\n", fps, static_cast<int>(s.brain.Mode()), sat,
                    RmsErr(m, from, n), MaxErr(m, from, n), counts);
        CHECK(s.brain.Mode() == BrainMode::Track);
        CHECK(sat < 20);
        CHECK(MaxErr(m, from, n) < 15.0);
    }
}

TEST(ServoBrain_cible_et_pointeur_de_part_et_d_autre_du_cap_180_pas_d_emballement) {
    // le cap est un angle : pres de +-180 deg (regard vers l'arriere, ou pres du pole), 0.06 rad d'ecart reel font ~6.2 rad de difference de caps
    for (double fps : {30.0, 90.0}) {
        Sim s;
        s.fps = fps;
        s.view.pose.q = {0.0f, 1.0f, 0.0f, 0.0f};  // la vue regarde vers +z (cap 180 deg)
        s.ptr = {-3.10, 0.0};
        const int n = static_cast<int>(fps * 8.0);
        const Metrics m = Run(s, n, [&](int) {
            Frame f;
            f.finger = {3.12, -0.05};
            return f;
        });
        int sat = 0;
        for (int k = 0; k < n; ++k) {
            const auto c = s.cnt[static_cast<size_t>(k)];
            if (std::max(std::abs(c.first), std::abs(c.second)) >= 100) ++sat;
        }
        const int from = n - static_cast<int>(fps * 2.0);
        std::printf("      cap 180, %.0f i/s : mode %d, %d images a >= 100 comptes, ecart final max %.1f px\n", fps, static_cast<int>(s.brain.Mode()), sat, MaxErr(m, from, n));
        CHECK(s.brain.Mode() == BrainMode::Track);
        CHECK(sat < 10);
        CHECK(MaxErr(m, from, n) < 15.0);
    }
}

TEST(ServoBrain_saut_de_la_cible_changement_de_main_pas_de_depassement) {
    for (double fps : {30.0, 90.0}) {
        Sim s;
        s.fps = fps;
        const int k0 = static_cast<int>(fps * 5.0);
        const Metrics m = Run(s, k0 + static_cast<int>(fps * 4), [&](int k) {
            Frame f;
            if (k >= k0) f.finger = {0.45, -0.40};
            return f;
        });
        // depassement : ecart maximum une fois arrive a moins de 15 px puis relache ; temps pour rester sous 10 px
        int arrived = -1, settled = -1;
        double over = 0;
        for (int k = k0; k < static_cast<int>(m.err.size()); ++k) {
            if (arrived < 0 && m.err[static_cast<size_t>(k)] < 15.0) arrived = k - k0;
            if (arrived >= 0) over = std::max(over, m.err[static_cast<size_t>(k)]);
        }
        for (int k = k0; k < static_cast<int>(m.err.size()) && settled < 0; ++k) {
            bool ok = true;
            for (int j = k; j < std::min(static_cast<int>(m.err.size()), k + static_cast<int>(fps * 0.5)) && ok; ++j) ok = m.err[static_cast<size_t>(j)] < 10.0;
            if (ok) settled = k - k0;
        }
        std::printf("      saut de cible a %.0f i/s : arrive apres %.2f s, depassement max %.0f px, stable (<10 px) apres %.2f s\n", fps, arrived / fps, over, settled / fps);
        CHECK(arrived >= 0 && settled >= 0);
        CHECK(over < 40.0);
        CHECK(settled / fps < 1.2);
    }
}

// ---- plant tourne / anisotrope, clic tenu, levier saisi, clic pres du pointeur detecte ----

namespace {

double Smooth(double t) {
    t = std::min(1.0, std::max(0.0, t));
    return 10 * t * t * t - 15 * t * t * t * t + 6 * t * t * t * t * t;
}

// Brain en suivi, regle comme en production (pas de mesure du gain).
void SetupProd(Sim& s) {
    s.brain.SetAutoProbe(false);
    s.brain.Recalibrate();  // (le constructeur du Sim a active la mesure) : puis on la desactive proprement
    s.brain.SetAutoProbe(false);
    s.brain.Reset();
}

// Doigt au repos, trois gestes doux de ~0.1 rad dans des directions differentes separes de 1.5 s de repos, puis repos.
void RunRotatedPlant(const char* label, double rotDeg, double ratio, double gainY, double pitchDeg) {
    Sim s;
    SetupProd(s);
    s.plantRotDeg = rotDeg;
    s.gainRatio = ratio;
    s.plantGainY = gainY;
    s.tipNoiseM = 0.001;
    const double th = pitchDeg * 3.14159265358979 / 180.0;
    s.view.pose.q = {static_cast<float>(std::sin(th / 2)), 0.0f, 0.0f, static_cast<float>(std::cos(th / 2))};
    s.ptr = {0.30, th + 0.1};
    const V2 base{0.05, th - 0.05};
    const int starts[3] = {450, 630, 810};
    const V2 deltas[3] = {{-0.10, 0.0}, {0.0, -0.08}, {0.08, 0.06}};
    const Metrics m = Run(s, 1100, [&](int k) {
        Frame f;
        V2 p = base;
        for (int i = 0; i < 3; ++i) {
            const double e = Smooth((k - starts[i]) / 45.0);
            p.a += deltas[i].a * e;
            p.b += deltas[i].b * e;
        }
        f.finger = p;
        return f;
    });
    const double r1 = MaxErr(m, 570, 630), r2 = MaxErr(m, 750, 810), r3 = MaxErr(m, 1040, 1100);
    long long counts = 0;
    for (int k = 1000; k < 1100; ++k) counts += std::abs(s.cnt[static_cast<size_t>(k)].first) + std::abs(s.cnt[static_cast<size_t>(k)].second);
    std::printf("      %s : repos max %.1f / %.1f / %.1f px, %lld comptes sur les 100 dernieres images, mode %d\n", label, r1, r2, r3, counts, static_cast<int>(s.brain.Mode()));
    CHECK(r1 < 10.0);
    CHECK(r2 < 10.0);
    CHECK(r3 < 10.0);
    CHECK(counts < 60);
}

// Pincement a l'appel 400, immobile jusqu'a 415, doigt de +0.08 rad en x en 40 images, tenue jusqu'a 475, relache ; arret a 560.
struct DragResult {
    double fingerDx = 0.0, errDragEnd = 0.0, errFinal = 0.0;
    bool lost = false;
};

DragResult RunDrag(Sim& s) {
    SetupProd(s);
    const V2 f0{0.05, -0.12}, f1{0.13, -0.12};
    const Px p0 = s.PixelOf(s.WorldPoint(f0, 0.45)), p1 = s.PixelOf(s.WorldPoint(f1, 0.45));
    const Metrics m = Run(s, 560, [&](int k) {
        Frame f;
        f.pinch = k >= 400 && k < 475;
        const double e = Smooth((k - 415) / 40.0);
        f.finger = {f0.a + (f1.a - f0.a) * e, f0.b};
        return f;
    });
    DragResult r;
    r.fingerDx = p1.x - p0.x;
    r.errDragEnd = m.err[470];
    r.errFinal = MaxErr(m, 550, 560);
    for (const std::string& l : s.logs) r.lost = r.lost || l.find("ne suit plus les comptes") != std::string::npos;
    return r;
}

}  // namespace

TEST(ServoBrain_plant_tourne_de_50_deg_le_pointeur_ne_tourne_pas_autour_du_doigt) {
    RunRotatedPlant("plant -50 deg, gain x1.1 / y x1.2", -50.0, 1.1, 1.2, 0.0);
}

TEST(ServoBrain_plant_tourne_de_60_deg_dans_l_autre_sens_le_pointeur_ne_tourne_pas_autour_du_doigt) {
    RunRotatedPlant("plant +60 deg, gain x0.9 / y x1.2", 60.0, 0.9, 1.2, 0.0);
}

TEST(ServoBrain_tete_penchee_de_70_deg_et_plant_tourne_de_50_deg) {
    RunRotatedPlant("tete -70 deg, plant -50 deg, gain x1.1 / y x1.2", -50.0, 1.1, 1.2, -70.0);
}

TEST(ServoBrain_clic_tenu_le_pointeur_ne_bouge_pas) {
    Sim s;
    SetupProd(s);
    s.plantRotDeg = -40.0;
    s.tipNoiseM = 0.0015;
    const Metrics m = Run(s, 640, [](int k) {
        Frame f;
        f.pinch = k >= 450 && k < 490;
        const double drift = 0.008 * Smooth((k - 450) / 40.0);  // ~10 px sur la tenue
        f.finger = {0.05 + drift, -0.12};
        return f;
    });
    std::printf("      clic tenu : %d/%d, %lld comptes bouton enfonce, pointeur deplace au plus de %.1f px, ecart final %.1f px\n", s.downs, s.ups, s.countsWhileDown, s.maxPtrMoveWhileDown, MaxErr(m, 600, 640));
    CHECK_EQ(s.downs, 1);
    CHECK_EQ(s.ups, 1);
    CHECK(s.countsWhileDown <= 2);
    CHECK(s.maxPtrMoveWhileDown < 4.0);
}

TEST(ServoBrain_pincement_qui_glisse_un_peu_ne_tire_pas_le_bouton) {
    Sim s;
    SetupProd(s);
    s.plantRotDeg = -40.0;
    s.tipNoiseM = 0.0015;
    const Metrics m = Run(s, 640, [](int k) {
        Frame f;
        f.pinch = k >= 450 && k < 490;
        const double drift = 0.012 * Smooth((k - 460) / 25.0);  // ~15 px pendant la tenue
        f.finger = {0.05 + drift, -0.12};
        return f;
    });
    std::printf("      pincement qui glisse : %d/%d, %lld comptes bouton enfonce, pointeur deplace au plus de %.1f px, ecart final %.1f px\n", s.downs, s.ups, s.countsWhileDown, s.maxPtrMoveWhileDown, MaxErr(m, 600, 640));
    CHECK_EQ(s.downs, 1);
    CHECK_EQ(s.ups, 1);
    CHECK(s.countsWhileDown <= 2);
}

TEST(ServoBrain_glisser_un_levier_saisi_le_levier_suit_le_doigt) {
    Sim s;
    s.grabOnPress = true;
    const DragResult r = RunDrag(s);
    const double expected = r.fingerDx / (s.TrueGain() * kPxPerRad);
    std::printf("      levier saisi : %d/%d, levier (%lld, %lld) comptes pour %.0f attendus (doigt %.0f px), pointeur fige a %.1f px max, ecart final %.1f px, \"ne suit plus\" %d\n", s.downs, s.ups, s.leverX, s.leverY, expected,
                r.fingerDx, s.maxPtrMoveWhileDown, r.errFinal, r.lost);
    CHECK_EQ(s.downs, 1);
    CHECK_EQ(s.ups, 1);
    CHECK(expected * s.leverX > 0);
    CHECK(std::fabs(s.leverX - expected) < 0.35 * std::fabs(expected));
    CHECK(std::fabs(static_cast<double>(s.leverY)) < 0.2 * std::fabs(expected));
    CHECK(!r.lost);
    CHECK(r.errFinal < 10.0);
}

TEST(ServoBrain_glisser_sans_saisie_le_pointeur_suit_le_doigt) {
    Sim s;
    s.grabOnPress = false;
    const DragResult r = RunDrag(s);
    std::printf("      glisser sans saisie : %d/%d, ecart en fin de glisser %.1f px, ecart final %.1f px\n", s.downs, s.ups, r.errDragEnd, r.errFinal);
    CHECK_EQ(s.downs, 1);
    CHECK_EQ(s.ups, 1);
    CHECK(r.errDragEnd < 40.0);
    CHECK(r.errFinal < 8.0);
}

TEST(ServoBrain_pointeur_teleporte_juste_avant_un_pincement_le_clic_se_fait_pres_du_doigt) {
    Sim s;
    SetupProd(s);
    s.teleportAt = 498;
    s.teleportBy = {0.3, 0.05};
    const Metrics m = Run(s, 760, [](int k) {
        Frame f;
        f.pinch = k >= 500 && k < 560;
        return f;
    });
    double worst = 0;
    for (const double e : s.pressErr) worst = std::max(worst, e);
    std::printf("      teleportation avant pincement : %d appui(s), pire ecart reel a l'appui %.1f px, ecart final %.1f px%s\n", s.downs, worst, MaxErr(m, 700, 760), s.downs == 0 ? " (aucun clic)" : "");
    CHECK(s.downs <= 1);
    CHECK_EQ(s.downs, s.ups);
    CHECK(worst < 40.0);
}

TEST(ServoBrain_plant_tourne_de_75_deg_le_pointeur_ne_tourne_pas_autour_du_doigt) {
    RunRotatedPlant("plant -75 deg, gain x1.1 / y x1.3", -75.0, 1.1, 1.3, -70.0);
}
