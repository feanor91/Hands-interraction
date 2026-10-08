// menu_render.h — dessin du menu des réglages (côté CPU, texte système Windows) dans un tampon de pixels BGRA.
// Fonctionnel avant d'être beau : un titre, des lignes de texte, la ligne survolée en surbrillance.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace layer {

constexpr uint32_t kMenuWidth = 600;
constexpr uint32_t kMenuHeaderH = 66;
constexpr uint32_t kMenuRowH = 54;
constexpr uint32_t kMenuFooterH = 12;

struct MenuRow {
    std::wstring text;
    bool selectable = true;  // faux : ligne d'information (intitulé de groupe), jamais surlignée
};

inline uint32_t MenuHeight(size_t rows) { return kMenuHeaderH + static_cast<uint32_t>(rows) * kMenuRowH + kMenuFooterH; }

// Indice de la ligne contenant le pixel (x, y) du menu, ou -1 hors des lignes.
int MenuRowAtPixel(int x, int y, size_t rows);

// Dessine le menu dans `pixels` (kMenuWidth x MenuHeight(rows), 4 octets par pixel : B, G, R, A=255). `hover` = ligne
// surlignée, ou -1. `status` = ligne d'état affichée dans l'en-tête sous le titre (peut être vide).
void RenderMenuBitmap(const std::wstring& title, const std::wstring& status, const std::vector<MenuRow>& rows, int hover, std::vector<uint32_t>* pixels);

}  // namespace layer
