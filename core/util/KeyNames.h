// KeyNames.h — nom de touche (hands.ini) -> code de touche virtuelle Windows. Portable (pas d'appel Windows), teste.
// Accepte : LSHIFT RSHIFT SHIFT LCTRL RCTRL CTRL LALT RALT ALT, F1..F24, A..Z, 0..9, SPACE ENTER TAB ESC HOME END INSERT DELETE, ou un code (0xA0 / 160).
// Vide, « none » ou inconnu : 0 (pas de touche).
#pragma once

#include <cctype>
#include <cstdlib>
#include <string>

namespace hands::util {

inline int ParseVirtualKey(const std::string& s) {
    std::string t;
    for (char c : s)
        if (c != ' ' && c != '\t') t += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (t.empty() || t == "NONE") return 0;
    struct N {
        const char* name;
        int vk;
    };
    static const N names[] = {{"LSHIFT", 0xA0}, {"RSHIFT", 0xA1}, {"SHIFT", 0x10}, {"LCTRL", 0xA2}, {"RCTRL", 0xA3}, {"CTRL", 0x11}, {"LALT", 0xA4},
                              {"RALT", 0xA5}, {"ALT", 0x12}, {"SPACE", 0x20}, {"ENTER", 0x0D}, {"TAB", 0x09}, {"ESC", 0x1B}, {"HOME", 0x24},
                              {"END", 0x23}, {"INSERT", 0x2D}, {"DELETE", 0x2E}};
    for (const N& n : names)
        if (t == n.name) return n.vk;
    if (t.size() >= 2 && t[0] == 'F' && std::isdigit(static_cast<unsigned char>(t[1]))) {
        const int n = std::atoi(t.c_str() + 1);
        if (n >= 1 && n <= 24) return 0x70 + n - 1;
        return 0;
    }
    if (t.size() == 1 && ((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= '0' && t[0] <= '9'))) return t[0];
    if (t.size() > 2 && t[0] == '0' && t[1] == 'X') {
        const long v = std::strtol(t.c_str() + 2, nullptr, 16);
        return v > 0 && v < 256 ? static_cast<int>(v) : 0;
    }
    if (std::isdigit(static_cast<unsigned char>(t[0]))) {
        const long v = std::strtol(t.c_str(), nullptr, 10);
        return v > 0 && v < 256 ? static_cast<int>(v) : 0;
    }
    return 0;
}

}  // namespace hands::util
