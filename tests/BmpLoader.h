// BmpLoader.h — lecteur BMP minimal pour les tests (24 ou 32 bits non compresse, haut en bas ou bas en haut) -> RGBA.
#pragma once

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace testbmp {

struct Img {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

inline bool LoadBmp(const std::string& path, Img* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return false;
    auto u32 = [&](size_t o) { return static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8) | (static_cast<uint32_t>(d[o + 2]) << 16) | (static_cast<uint32_t>(d[o + 3]) << 24); };
    const uint32_t off = u32(10);
    const int w = static_cast<int32_t>(u32(18)), hh = static_cast<int32_t>(u32(22));
    const int bpp = d[28] | (d[29] << 8);
    if ((bpp != 24 && bpp != 32) || w <= 0 || hh == 0) return false;
    const int h = hh < 0 ? -hh : hh;
    const bool topDown = hh < 0;
    const size_t stride = ((static_cast<size_t>(w) * bpp + 31) / 32) * 4;
    out->w = w;
    out->h = h;
    out->rgba.assign(static_cast<size_t>(w) * h * 4, 255);
    for (int y = 0; y < h; ++y) {
        const size_t srcRow = off + stride * static_cast<size_t>(topDown ? y : h - 1 - y);
        if (srcRow + static_cast<size_t>(w) * (bpp / 8) > d.size()) return false;
        for (int x = 0; x < w; ++x) {
            const uint8_t* s = &d[srcRow + static_cast<size_t>(x) * (bpp / 8)];
            uint8_t* o = &out->rgba[(static_cast<size_t>(y) * w + x) * 4];
            o[0] = s[2];
            o[1] = s[1];
            o[2] = s[0];
        }
    }
    return true;
}

}  // namespace testbmp
