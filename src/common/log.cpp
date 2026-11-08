#include "common/log.hpp"

#include <iostream>
#include <mutex>

namespace project360::common {
namespace {
std::mutex output_mutex;

std::string_view label(LogLevel level) {
    switch (level) {
    case LogLevel::debug: return "DEBUG";
    case LogLevel::info: return "INFO";
    case LogLevel::warning: return "WARN";
    case LogLevel::error: return "ERROR";
    }
    return "UNKNOWN";
}
} // namespace

void log(LogLevel level, std::string_view category, std::string_view message) {
    const std::lock_guard lock(output_mutex);
    std::clog << '[' << label(level) << "] [" << category << "] " << message << '\n';
}

} // namespace project360::common
