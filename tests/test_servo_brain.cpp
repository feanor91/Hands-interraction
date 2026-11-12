// Simulation de bout en bout de l'asservissement visuel : un « MSFS » simule (pointeur = integrateur de comptes de souris, latence L, gain inconnu, curseur
// masque apres 3 s sans mouvement) dessine une VRAIE fleche (decoupee d'une capture native) dans une image synthetique ; le cerveau la cherche (balayage),
// mesure le gain, puis suit un bout d'index qui bouge. Verifie les conventions de coordonnees, la machine a etats et la tenue aux erreurs de gain / latence.
#include <cmath>
#include <cstdint>
#include <cstdio>
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
    double gainRatio = 1.0;             // gain reel / gain a priori du cerveau
    double fps = 90.0;
    bool hideIdle = true;               // MSFS masque le curseur apres 3 s sans mouvement
    bool startHidden = false;
    bool brightBg = false;
    V2 ptr{0.30, -0.25};                // direction (cap, site) du pointeur au depart
    double wallYaw = 1e9;
    double headSwayDeg = 0.0;           // balancement de la tete (cap) : amplitude en degres
    double headSwayHz = 0.4;
    bool headAnchored = false;          // le pointeur est colle a la tete (sinon : fixe dans le monde)
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

    Sim() {
        view.fov = {-0.94f, 0.70f, 0.77f, -0.96f};
        view.rect = {0, 0, 2688, 2880};
        view.pose = {{0, 0, 0, 1}, {0, 0, 0}};
        brain.SetLogger([this](int, const std::string& m) { logs.push_back(m); });
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
};

struct Metrics {
    std::vector<double> err;
};

// Joue `n` appels ; `script(k)` donne l'etat du doigt a l'appel k.
Metrics Run(Sim& s, int n, const std::function<Frame(int)>& script) {
    Metrics m;
    s.cnt.assign(static_cast<size_t>(n) + 8, {0, 0});
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
        // le « jeu » applique les comptes envoyes il y a L appels
        if (k >= s.L) {
            const auto c = s.cnt[static_cast<size_t>(k - s.L)];
            if (c.first != 0 || c.second != 0) {
                s.ptr.a += s.TrueGain() * c.first;
                s.ptr.b += -s.TrueGain() * c.second;
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
        h.extended = true;
        h.tip = tip;
        h.knuckle = tip + em::Vec3{0.0f, -0.03f, 0.06f};
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
            fl.at = k + s.readLat;
            fl.req = out.request;
            s.Render(out.request, now, &fl.px);
        }
        const Px pp = s.PixelOf(s.PointerWorld()), fp = s.PixelOf(trueTip);
        m.err.push_back(std::hypot(pp.x - fp.x, pp.y - fp.y));
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

TEST(ServoBrain_tete_qui_se_balance_pointeur_colle_a_la_tete) {
    Sim s;
    s.headSwayDeg = 8.0;
    s.headAnchored = true;
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
    std::printf("      saut de 0.4 rad : retour a < 8 px apres %d images (%.1f s a 90 Hz)\n", back, back / 90.0);
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
