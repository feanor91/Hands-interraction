// menu_render.cpp — voir menu_render.h.
#include "menu_render.h"

#include <windows.h>

namespace layer {

int MenuRowAtPixel(int x, int y, size_t rows) {
    if (x < 0 || x >= static_cast<int>(kMenuWidth) || y < static_cast<int>(kMenuHeaderH)) return -1;
    const int row = (y - static_cast<int>(kMenuHeaderH)) / static_cast<int>(kMenuRowH);
    return row < static_cast<int>(rows) ? row : -1;
}

void RenderMenuBitmap(const std::wstring& title, const std::wstring& status, const std::vector<MenuRow>& rows, int hover, std::vector<uint32_t>* pixels) {
    const int w = static_cast<int>(kMenuWidth), h = static_cast<int>(MenuHeight(rows.size()));
    pixels->assign(static_cast<size_t>(w) * h, 0xFF000000u);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // lignes de haut en bas
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);

    auto brush = [](COLORREF c) { return CreateSolidBrush(c); };
    auto fill = [&](int x, int y, int cx, int cy, COLORREF c) {
        HBRUSH b = brush(c);
        RECT r{x, y, x + cx, y + cy};
        FillRect(dc, &r, b);
        DeleteObject(b);
    };
    LOGFONTW lf{};
    lf.lfHeight = -28;
    lf.lfWeight = FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    HFONT font = CreateFontIndirectW(&lf);
    lf.lfHeight = -28;
    lf.lfWeight = FW_BOLD;
    HFONT fontBig = CreateFontIndirectW(&lf);
    lf.lfHeight = -17;
    lf.lfWeight = FW_NORMAL;
    HFONT fontSmall = CreateFontIndirectW(&lf);

    fill(0, 0, w, h, RGB(22, 26, 34));                         // fond
    fill(0, 0, w, static_cast<int>(kMenuHeaderH), RGB(34, 84, 150));  // en-tête
    SelectObject(dc, fontBig);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rt{16, 6, w - 16, 40};
    DrawTextW(dc, title.c_str(), -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, fontSmall);
    SetTextColor(dc, RGB(205, 225, 250));
    RECT rs{16, 40, w - 16, static_cast<int>(kMenuHeaderH) - 4};
    DrawTextW(dc, status.c_str(), -1, &rs, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, font);
    for (size_t i = 0; i < rows.size(); ++i) {
        const int y = static_cast<int>(kMenuHeaderH + i * kMenuRowH);
        const bool sel = rows[i].selectable;
        if (sel && static_cast<int>(i) == hover) fill(4, y + 2, w - 8, static_cast<int>(kMenuRowH) - 4, RGB(46, 120, 210));
        else if (sel) fill(4, y + 2, w - 8, static_cast<int>(kMenuRowH) - 4, RGB(36, 42, 54));
        SetTextColor(dc, sel ? RGB(255, 255, 255) : RGB(150, 170, 200));
        RECT rr{20, y, w - 12, y + static_cast<int>(kMenuRowH)};
        DrawTextW(dc, rows[i].text.c_str(), -1, &rr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    GdiFlush();
    const uint32_t* src = static_cast<const uint32_t*>(bits);
    for (size_t i = 0; i < pixels->size(); ++i) (*pixels)[i] = src[i] | 0xFF000000u;  // GDI ne renseigne pas l'alpha : opaque

    SelectObject(dc, oldBmp);
    DeleteObject(font);
    DeleteObject(fontBig);
    DeleteObject(fontSmall);
    DeleteObject(bmp);
    DeleteDC(dc);
}

}  // namespace layer
