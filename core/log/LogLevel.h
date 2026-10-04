#pragma once
#include <string_view>

namespace hands::log {

// NOTE C++ : `enum class` est un enum « fortement typé » : pas de conversion
// implicite en int, et les valeurs s'écrivent LogLevel::Info (comme en C#).
enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

// Convertit "error"/"warn"/"info"/"debug" (insensible à la casse) ; `def` sinon.
LogLevel ParseLogLevel(std::string_view text, LogLevel def);
const char* LogLevelName(LogLevel level);

}  // namespace hands::log
