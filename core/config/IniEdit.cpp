#include "IniEdit.h"

#include <vector>

#include "../util/StringUtil.h"

namespace hands::config {

namespace {

// Découpe en lignes en gardant leur fin de ligne (la dernière peut ne pas en avoir).
std::vector<std::string> SplitKeepEol(std::string_view text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) nl = text.size() - 1;
        lines.emplace_back(text.substr(start, nl - start + 1));
        start = nl + 1;
    }
    return lines;
}

std::string Trimmed(std::string_view s) { return std::string(hands::util::Trim(s)); }

}  // namespace

std::string IniSetValue(std::string_view text, std::string_view section, std::string_view key, std::string_view value) {
    std::vector<std::string> lines = SplitKeepEol(text);
    const std::string eol = text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    const std::string newLine = std::string(key) + " = " + std::string(value);

    bool inSection = false, sectionFound = false;
    size_t sectionEnd = 0;  // index (exclu) de la dernière ligne non vide de la section
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = Trimmed(lines[i]);
        if (!t.empty() && t.front() == '[') {
            const size_t close = t.find(']');
            const std::string name = close == std::string::npos ? "" : Trimmed(t.substr(1, close - 1));
            inSection = hands::util::EqualsIgnoreCase(name, std::string(section));
            if (inSection) { sectionFound = true; sectionEnd = i + 1; }
            continue;
        }
        if (!inSection) continue;
        if (!t.empty()) sectionEnd = i + 1;
        if (!t.empty() && t.front() != ';' && t.front() != '#') {
            const size_t eq = t.find('=');
            if (eq != std::string::npos && hands::util::EqualsIgnoreCase(Trimmed(t.substr(0, eq)), std::string(key))) {
                lines[i] = newLine + eol;
                std::string out;
                for (const auto& l : lines) out += l;
                return out;
            }
            sectionEnd = i + 1;
        }
    }

    if (sectionFound) {
        if (sectionEnd > 0 && lines[sectionEnd - 1].find('\n') == std::string::npos) lines[sectionEnd - 1] += eol;
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(sectionEnd), newLine + eol);
    } else {
        if (!lines.empty() && lines.back().find('\n') == std::string::npos) lines.back() += eol;
        if (!lines.empty()) lines.emplace_back(eol);
        lines.emplace_back("[" + std::string(section) + "]" + eol);
        lines.emplace_back(newLine + eol);
    }
    std::string out;
    for (const auto& l : lines) out += l;
    return out;
}

}  // namespace hands::config
