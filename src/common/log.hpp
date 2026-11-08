#pragma once

#include <string_view>

namespace project360::common {

enum class LogLevel { debug, info, warning, error };

void log(LogLevel level, std::string_view category, std::string_view message);

} // namespace project360::common
