#include "StringUtil.h"

namespace hands::util {

std::string ToLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string_view Trim(std::string_view s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

std::vector<std::string> SplitList(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(sep, start);
        if (end == std::string_view::npos) end = s.size();
        const std::string_view item = Trim(s.substr(start, end - start));
        if (!item.empty()) out.emplace_back(item);
        start = end + 1;
    }
    return out;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    return ToLower(a) == ToLower(b);
}

std::string FileNameOf(std::string_view path) {
    const size_t pos = path.find_last_of("\\/");
    return std::string(pos == std::string_view::npos ? path : path.substr(pos + 1));
}

}  // namespace hands::util
