#include "IniEdit.h"

#include <algorithm>
#include <cctype>
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

namespace {

struct TmplKey {
    std::string key;
    std::vector<std::string> lines;  // commentaires qui la precedent + la ligne de la cle (sans fin de ligne)
};
struct TmplSection {
    std::string name;
    std::string header;             // ligne « [nom] » telle quelle
    std::vector<std::string> body;  // toutes les lignes de la section (sans fin de ligne), pour une section manquante
    std::vector<TmplKey> keys;
};

std::string NoEol(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

bool IsHeader(const std::string& t, std::string* name) {
    if (t.empty() || t.front() != '[') return false;
    const size_t close = t.find(']');
    *name = close == std::string::npos ? "" : Trimmed(t.substr(1, close - 1));
    return true;
}

// Nom de la cle d'une ligne active « cle = valeur » (vide sinon).
std::string KeyOf(const std::string& t) {
    if (t.empty() || t.front() == ';' || t.front() == '#') return {};
    const size_t eq = t.find('=');
    return eq == std::string::npos ? std::string() : Trimmed(t.substr(0, eq));
}

// Cle d'un commentaire « ; cle = valeur » (une cle desactivee a la main), vide sinon.
std::string CommentedKeyOf(const std::string& t) {
    if (t.empty() || (t.front() != ';' && t.front() != '#')) return {};
    const std::string rest = Trimmed(t.substr(1));
    const size_t eq = rest.find('=');
    if (eq == std::string::npos) return {};
    const std::string k = Trimmed(rest.substr(0, eq));
    if (k.empty()) return {};
    for (char c : k)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-')) return {};
    return k;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::string IniMergeMissing(std::string_view user, std::string_view tmpl, std::vector<std::string>* added) {
    // --- Modele : sections, cles et commentaires qui precedent chaque cle ---
    std::vector<TmplSection> sections;
    {
        std::vector<std::string> pending;  // commentaires depuis la derniere cle / ligne vide
        for (const std::string& raw : SplitKeepEol(tmpl)) {
            const std::string line = NoEol(raw);
            const std::string t = Trimmed(line);
            std::string name;
            if (IsHeader(t, &name)) {
                sections.push_back({name, line, {}, {}});
                pending.clear();
                continue;
            }
            if (sections.empty()) continue;  // preambule : ignore
            TmplSection& sec = sections.back();
            sec.body.push_back(line);
            if (t.empty()) {
                pending.clear();
                continue;
            }
            if (t.front() == ';' || t.front() == '#') {
                pending.push_back(line);
                continue;
            }
            const std::string k = KeyOf(t);
            if (k.empty()) {
                pending.clear();
                continue;
            }
            TmplKey tk{k, pending};
            tk.lines.push_back(line);
            sec.keys.push_back(std::move(tk));
            pending.clear();
        }
    }
    for (auto& sec : sections)
        while (!sec.body.empty() && Trimmed(sec.body.back()).empty()) sec.body.pop_back();

    // --- Fichier de l'utilisateur : sections, cles presentes (actives ou en commentaire), fin de chaque section ---
    std::vector<std::string> lines = SplitKeepEol(user);
    const std::string eol = user.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    struct UserSection {
        std::string name;
        size_t end = 0;                 // index (exclu) de la derniere ligne non vide
        std::vector<std::string> keys;  // en minuscules
    };
    std::vector<UserSection> us;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = Trimmed(NoEol(lines[i]));
        std::string name;
        if (IsHeader(t, &name)) {
            us.push_back({name, i + 1, {}});
            continue;
        }
        if (us.empty() || t.empty()) continue;
        us.back().end = i + 1;
        std::string k = KeyOf(t);
        if (k.empty()) k = CommentedKeyOf(t);
        if (!k.empty()) us.back().keys.push_back(Lower(k));
    }
    auto findUser = [&](const std::string& name) -> UserSection* {
        for (auto& s : us)
            if (hands::util::EqualsIgnoreCase(s.name, name)) return &s;
        return nullptr;
    };

    // --- Ajouts : en fin des sections existantes ; sections manquantes en fin de fichier ---
    struct Insert {
        size_t at;
        std::vector<std::string> lines;
    };
    std::vector<Insert> inserts;
    std::vector<std::string> tail;
    for (const TmplSection& sec : sections) {
        UserSection* u = findUser(sec.name);
        if (!u) {
            if (sec.keys.empty()) continue;
            tail.push_back("");
            tail.push_back(sec.header);
            for (const auto& l : sec.body) tail.push_back(l);
            if (added)
                for (const auto& k : sec.keys) added->push_back(sec.name + "." + k.key);
            continue;
        }
        Insert ins{u->end, {}};
        for (const TmplKey& k : sec.keys) {
            const std::string lk = Lower(k.key);
            bool have = false;
            for (const auto& x : u->keys) have = have || x == lk;
            if (have) continue;
            u->keys.push_back(lk);  // cle en double dans le modele : une seule fois
            for (const auto& l : k.lines) ins.lines.push_back(l);
            if (added) added->push_back(sec.name + "." + k.key);
        }
        if (!ins.lines.empty()) inserts.push_back(std::move(ins));
    }
    if (inserts.empty() && tail.empty()) return std::string(user);

    // De la fin vers le debut : les index des insertions suivantes restent valides.
    std::stable_sort(inserts.begin(), inserts.end(), [](const Insert& a, const Insert& b) { return a.at > b.at; });
    for (const Insert& ins : inserts) {
        if (ins.at > 0 && lines[ins.at - 1].find('\n') == std::string::npos) lines[ins.at - 1] += eol;
        std::vector<std::string> add;
        for (const auto& l : ins.lines) add.push_back(l + eol);
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(ins.at), add.begin(), add.end());
    }
    if (!tail.empty()) {
        if (!lines.empty() && lines.back().find('\n') == std::string::npos) lines.back() += eol;
        const size_t from = lines.empty() ? 1 : 0;  // fichier vide : pas de ligne blanche en tete
        for (size_t i = from; i < tail.size(); ++i) lines.push_back(tail[i] + eol);
    }
    std::string out;
    for (const auto& l : lines) out += l;
    return out;
}

}  // namespace hands::config
