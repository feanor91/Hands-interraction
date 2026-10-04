#include "IniFile.h"

#include <charconv>

#include "../util/StringUtil.h"

namespace hands::config {

using hands::util::Trim;
using hands::util::ToLower;

std::string IniFile::MakeKey(std::string_view section, std::string_view key) {
    return ToLower(section) + "." + ToLower(key);
}

IniFile IniFile::Parse(std::string_view text, std::vector<std::string>* warnings) {
    IniFile ini;
    std::string section;  // section courante ("" = avant toute section)
    int lineNo = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++lineNo;

        // Retire un éventuel BOM UTF-8 sur la première ligne (Notepad en ajoute).
        if (lineNo == 1 && line.size() >= 3 && line.substr(0, 3) == "\xEF\xBB\xBF") line.remove_prefix(3);

        // Commentaire de fin de ligne : on coupe au premier ';' ou '#'.
        const size_t c = line.find_first_of(";#");
        if (c != std::string_view::npos) line = line.substr(0, c);
        line = Trim(line);
        if (line.empty()) continue;

        if (line.front() == '[') {
            if (line.back() != ']') {
                if (warnings) warnings->push_back("ligne " + std::to_string(lineNo) + " : section mal fermee");
                continue;
            }
            section = std::string(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            if (warnings) warnings->push_back("ligne " + std::to_string(lineNo) + " : '=' manquant, ignoree");
            continue;
        }
        const std::string_view key = Trim(line.substr(0, eq));
        const std::string_view value = Trim(line.substr(eq + 1));
        if (key.empty()) {
            if (warnings) warnings->push_back("ligne " + std::to_string(lineNo) + " : cle vide, ignoree");
            continue;
        }
        ini.values_[MakeKey(section, key)] = std::string(value);
    }
    return ini;
}

bool IniFile::Has(std::string_view section, std::string_view key) const {
    return values_.count(MakeKey(section, key)) != 0;
}

std::string IniFile::GetString(std::string_view section, std::string_view key, std::string_view def) const {
    auto it = values_.find(MakeKey(section, key));
    return it == values_.end() ? std::string(def) : it->second;
}

bool IniFile::GetBool(std::string_view section, std::string_view key, bool def) const {
    auto it = values_.find(MakeKey(section, key));
    if (it == values_.end()) return def;
    const std::string v = ToLower(it->second);
    if (v == "true" || v == "yes" || v == "on" || v == "1") return true;
    if (v == "false" || v == "no" || v == "off" || v == "0") return false;
    return def;  // valeur illisible : on garde le défaut
}

int IniFile::GetInt(std::string_view section, std::string_view key, int def) const {
    auto it = values_.find(MakeKey(section, key));
    if (it == values_.end()) return def;
    int result = 0;
    const char* b = it->second.data();
    const char* e = b + it->second.size();
    auto [ptr, ec] = std::from_chars(b, e, result);
    return (ec == std::errc() && ptr == e) ? result : def;
}

double IniFile::GetDouble(std::string_view section, std::string_view key, double def) const {
    auto it = values_.find(MakeKey(section, key));
    if (it == values_.end()) return def;
    // std::from_chars ne dépend PAS de la locale (contrairement à strtod, qui
    // attendrait une virgule décimale si le jeu a appelé setlocale en français).
    double v = 0.0;
    const char* b = it->second.data();
    const char* e = b + it->second.size();
    auto [ptr, ec] = std::from_chars(b, e, v);
    return (ec == std::errc() && ptr == e) ? v : def;
}

std::vector<std::string> IniFile::GetList(std::string_view section, std::string_view key,
                                          const std::vector<std::string>& def) const {
    auto it = values_.find(MakeKey(section, key));
    if (it == values_.end()) return def;
    return hands::util::SplitList(it->second, ',');
}

}  // namespace hands::config
