// Detecteur du pointeur de MSFS sur de VRAIS recadrages natifs de l'image du jeu (tests/data, captures de la couche du 11 nov. 2026).
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "BmpLoader.h"
#include "TestFramework.h"
#include "servo/PointerDetect.h"

using namespace hands::servo;

namespace {

using testbmp::Img;
using testbmp::LoadBmp;

struct Case {
    std::string kind, file;
    double x = 0, y = 0;
};

std::vector<Case> LoadCases() {
    std::vector<Case> v;
    std::ifstream f(std::string(HANDS_TESTDATA_DIR) + "/pointer_cases.txt");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        Case c;
        if (ss >> c.kind >> c.file >> c.x >> c.y) v.push_back(c);
    }
    return v;
}

}  // namespace

TEST(PointerDetect_pointeurs_reels_trouves_au_bon_endroit) {
    const auto cases = LoadCases();
    CHECK(cases.size() >= 8);
    int found = 0, wanted = 0;
    for (const Case& c : cases) {
        if (c.kind == "neg") continue;
        ++wanted;
        Img img;
        CHECK(LoadBmp(std::string(HANDS_TESTDATA_DIR) + "/" + c.file, &img));
        PointerCandidate out[8];
        const int n = DetectPointer(img.rgba.data(), img.w * 4, img.w, img.h, DetectParams(), nullptr, out, 8);
        const double tol = c.kind == "clip" ? 3.5 : 2.5;
        const bool ok = n > 0 && std::fabs(out[0].x - c.x) <= tol && std::fabs(out[0].y - c.y) <= tol;
        std::printf("      %-22s attendu (%.1f, %.1f) -> %s", c.file.c_str(), c.x, c.y, n ? "" : "RIEN");
        if (n) std::printf("(%.0f, %.0f) score %.2f aire %d %s", out[0].x, out[0].y, out[0].score, out[0].area, out[0].hand ? "main" : "fleche");
        std::printf("%s\n", ok ? "" : "   <-- ECHEC");
        CHECK(ok);
        if (ok) ++found;
    }
    CHECK_EQ(found, wanted);
}

TEST(PointerDetect_scenes_sans_pointeur_aucun_candidat_serieux) {
    for (const Case& c : LoadCases()) {
        if (c.kind != "neg") continue;
        Img img;
        CHECK(LoadBmp(std::string(HANDS_TESTDATA_DIR) + "/" + c.file, &img));
        PointerCandidate out[8];
        const int n = DetectPointer(img.rgba.data(), img.w * 4, img.w, img.h, DetectParams(), nullptr, out, 8);
        const double best = n ? out[0].score : 0.0;
        std::printf("      %-22s %d candidat(s), meilleur score %.2f\n", c.file.c_str(), n, best);
        CHECK(best < 0.4);
        // reglages de suivi (graines plus basses, contour un peu plus tolerant), avec un a priori au milieu de la scene : aucun faux positif non plus
        DetectPrior prior;
        prior.valid = true;
        prior.x = img.w * 0.5;
        prior.y = img.h * 0.5;
        prior.sigma = 120;
        const int nt = DetectPointer(img.rgba.data(), img.w * 4, img.w, img.h, TrackingParams(), &prior, out, 8);
        CHECK(nt == 0 || out[0].score < 0.25);
    }
}

TEST(PointerDetect_roi_vide_ou_sans_blanc) {
    std::vector<uint8_t> black(64 * 64 * 4, 0);
    PointerCandidate out[4];
    CHECK_EQ(DetectPointer(black.data(), 64 * 4, 64, 64, DetectParams(), nullptr, out, 4), 0);
    CHECK_EQ(DetectPointer(nullptr, 0, 0, 0, DetectParams(), nullptr, out, 4), 0);
}

TEST(PointerDetect_grand_panneau_blanc_ne_ralentit_pas_et_ne_donne_rien) {
    // 400x300 tout blanc : une seule grosse region, marquee une fois (pas de remplissage par graine)
    std::vector<uint8_t> white(400 * 300 * 4, 255);
    PointerCandidate out[4];
    CHECK_EQ(DetectPointer(white.data(), 400 * 4, 400, 300, DetectParams(), nullptr, out, 4), 0);
}

TEST(PointerDetect_a_priori_favorise_le_candidat_proche) {
    Img img;
    CHECK(LoadBmp(std::string(HANDS_TESTDATA_DIR) + "/ptr_arrow_medium.bmp", &img));
    PointerCandidate a[4], b[4];
    DetectPrior near;
    near.valid = true;
    near.x = 58;
    near.y = 19;
    near.sigma = 30;
    DetectPrior far;
    far.valid = true;
    far.x = 400;
    far.y = 400;
    far.sigma = 30;
    const int na = DetectPointer(img.rgba.data(), img.w * 4, img.w, img.h, DetectParams(), &near, a, 4);
    const int nb = DetectPointer(img.rgba.data(), img.w * 4, img.w, img.h, DetectParams(), &far, b, 4);
    CHECK(na > 0 && nb > 0);
    CHECK(a[0].score > b[0].score * 1.5);
}
