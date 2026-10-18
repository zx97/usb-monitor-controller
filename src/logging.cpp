// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/logging.hpp"

#include <mutex>

namespace lcdctl {
namespace {
LogLevel g_level = LogLevel::Info;
std::mutex g_mutex;
}  // namespace

void setLogLevel(LogLevel level) { g_level = level; }
LogLevel logLevel() { return g_level; }

void logMessage(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) > static_cast<int>(g_level)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::ostream& out = (level == LogLevel::Error) ? std::cerr : std::cout;
    out << logLevelName(level) << ": " << message << std::endl;
}

}  // namespace lcdctl
