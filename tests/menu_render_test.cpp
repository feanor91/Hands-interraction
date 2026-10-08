// menu_render_test.cpp — dessine un menu d'exemple, vérifie quelques pixels et écrit une image BMP pour contrôle visuel.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "menu_render.h"

using namespace layer;

namespace {
int g_failures = 0;
#define EXPECT(cond, msg)                                          \
    do {                                                           \
        if (!(cond)) {                                             \
            std::printf("[ECHEC] %s (ligne %d)\n", msg, __LINE__); \
            ++g_failures;                                          \
        }                                                          \
    } while (0)

bool WriteBmp(const char* path, const std::vector<uint32_t>& px, uint32_t w, uint32_t h) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    const uint32_t imgSize = w * h * 4, fileSize = 54 + imgSize, offset = 54, dib = 40;
    uint8_t hd[54] = {'B', 'M'};
    std::memcpy(hd + 2, &fileSize, 4);
    std::memcpy(hd + 10, &offset, 4);
    std::memcpy(hd + 14, &dib, 4);
    const int32_t sw = static_cast<int32_t>(w), sh = -static_cast<int32_t>(h);
    std::memcpy(hd + 18, &sw, 4);
    std::memcpy(hd + 22, &sh, 4);
    const uint16_t pl = 1, bpp = 32;
    std::memcpy(hd + 26, &pl, 2);
    std::memcpy(hd + 28, &bpp, 2);
    std::memcpy(hd + 34, &imgSize, 4);
    std::fwrite(hd, 1, 54, f);
    std::fwrite(px.data(), 4, px.size(), f);
    std::fclose(f);
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "menu_render_test.bmp";
    std::vector<MenuRow> rows = {
        {L"Rayon (pour viser) :", false},
        {L"   Comme le casque : de l'épaule  ✔", true},
        {L"   Par le point de contact, depuis le poignet", true},
        {L"Doigt du pointeur : majeur", true},
        {L"Clic : pouce sur le bout de l'index", true},
        {L"Relâchement du clic : normal (21 / 27 mm)", true},
        {L"Deux mains actives : OUI", true},
        {L"Fermer le menu", true},
    };
    std::vector<uint32_t> px;
    RenderMenuBitmap(L"Mains nues OpenXR — réglages", L"Pincez pour choisir · pincez les deux mains pour fermer", rows, 3, &px);
    const uint32_t w = kMenuWidth, h = MenuHeight(rows.size());
    EXPECT(px.size() == static_cast<size_t>(w) * h, "taille du tampon");

    // Pixels de controle : en-tete bleu (R=34,G=84,B=150), fond, ligne survolee (R=46,G=120,B=210), ligne normale
    auto at = [&](uint32_t x, uint32_t y) { return px[static_cast<size_t>(y) * w + x]; };
    EXPECT(at(3, 3) == 0xFF225496u, "en-tete bleu");
    EXPECT(at(2, h - 3) == 0xFF161A22u, "fond sombre en bas");
    const uint32_t hoverY = kMenuHeaderH + 3 * kMenuRowH + 4;
    EXPECT(at(8, hoverY) == 0xFF2E78D2u, "ligne survolee en surbrillance");
    EXPECT(at(8, kMenuHeaderH + 1 * kMenuRowH + 4) == 0xFF242A36u, "ligne selectionnable non survolee");
    EXPECT(at(8, kMenuHeaderH + 4) == 0xFF161A22u, "ligne d'intitule : pas de fond de bouton");

    // Du texte clair existe dans l'en-tete et dans une ligne (pixels presque blancs)
    auto whites = [&](uint32_t y0, uint32_t y1) {
        int n = 0;
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t p = at(x, y);
                if ((p & 0xFF) > 200 && ((p >> 8) & 0xFF) > 200 && ((p >> 16) & 0xFF) > 200) ++n;
            }
        return n;
    };
    EXPECT(whites(4, 40) > 40, "titre ecrit");
    EXPECT(whites(kMenuHeaderH + kMenuRowH, kMenuHeaderH + 2 * kMenuRowH) > 40, "texte d'une ligne ecrit");
    EXPECT(whites(h - 8, h) == 0, "pas de texte dans le pied");

    // Correspondance pixel -> ligne
    EXPECT(MenuRowAtPixel(100, 10, rows.size()) == -1, "en-tete : aucune ligne");
    EXPECT(MenuRowAtPixel(100, kMenuHeaderH + 1, rows.size()) == 0, "premiere ligne");
    EXPECT(MenuRowAtPixel(100, kMenuHeaderH + kMenuRowH * 3 + 5, rows.size()) == 3, "quatrieme ligne");
    EXPECT(MenuRowAtPixel(100, h - 2, rows.size()) == -1, "pied : aucune ligne");
    EXPECT(MenuRowAtPixel(-5, 100, rows.size()) == -1 && MenuRowAtPixel(w + 5, 100, rows.size()) == -1, "hors largeur");

    EXPECT(WriteBmp(out, px, w, h), "ecriture du BMP");
    if (g_failures == 0) std::printf("menu_render_test : OK (image : %s)\n", out);
    return g_failures == 0 ? 0 : 1;
}
