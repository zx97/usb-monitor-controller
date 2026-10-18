// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - minimal logging facility.
//
// Ported from https://github.com/mathoudebine/turing-smart-screen-python/

#pragma once

#include <iostream>
#include <sstream>
#include <string>

namespace lcdctl {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void setLogLevel(LogLevel level);
LogLevel logLevel();

void logMessage(LogLevel level, const std::string& message);

inline const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn:  return "WARNING";
        case LogLevel::Info:  return "INFO";
        default:              return "DEBUG";
    }
}

// Stream-style helper: LCDCTL_LOG(Info) << "hello " << 42;
class LogStream {
public:
    explicit LogStream(LogLevel level) : level_(level) {}
    ~LogStream() { logMessage(level_, buffer_.str()); }

    template <typename T>
    LogStream& operator<<(const T& value) {
        buffer_ << value;
        return *this;
    }

private:
    LogLevel level_;
    std::ostringstream buffer_;
};

}  // namespace lcdctl

#define LCDCTL_LOG(level)                            \
    if (static_cast<int>(level) <=                   \
        static_cast<int>(::lcdctl::logLevel()))      \
        ::lcdctl::LogStream(::lcdctl::LogLevel::level)

#define LCDCTL_ERROR ::lcdctl::LogStream(::lcdctl::LogLevel::Error)
#define LCDCTL_WARN  ::lcdctl::LogStream(::lcdctl::LogLevel::Warn)
#define LCDCTL_INFO  ::lcdctl::LogStream(::lcdctl::LogLevel::Info)
#define LCDCTL_DEBUG ::lcdctl::LogStream(::lcdctl::LogLevel::Debug)
