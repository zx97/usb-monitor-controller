// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/paths.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace lcdctl {
namespace {

bool fileExists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

std::string executableDir() {
#if defined(__linux__)
    std::array<char, 4096> buffer{};
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length > 0) {
        std::string path(buffer.data(), static_cast<std::size_t>(length));
        const std::size_t slash = path.find_last_of('/');
        if (slash != std::string::npos) {
            return path.substr(0, slash);
        }
    }
#elif defined(__APPLE__)
    std::array<char, 4096> buffer{};
    std::uint32_t size = buffer.size();
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        std::string path(buffer.data());
        const std::size_t slash = path.find_last_of('/');
        if (slash != std::string::npos) {
            return path.substr(0, slash);
        }
    }
#elif defined(_WIN32)
    std::array<char, 4096> buffer{};
    const DWORD length = GetModuleFileNameA(nullptr, buffer.data(), buffer.size());
    if (length > 0) {
        std::string path(buffer.data(), length);
        const std::size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos) {
            return path.substr(0, slash);
        }
    }
#endif
    return "";
}

}  // namespace

std::string resourceRoot() {
    std::vector<std::string> candidates;

    const std::string exeDir = executableDir();
    if (!exeDir.empty()) {
        candidates.push_back(exeDir);
        candidates.push_back(exeDir + "/..");
    }
    // Current and parent working directories (running from the build tree).
    candidates.push_back(".");
    candidates.push_back("..");
#ifdef LCDCTL_SOURCE_DIR
    candidates.push_back(LCDCTL_SOURCE_DIR);
#endif

    for (const std::string& candidate : candidates) {
        if (fileExists(candidate + "/res/fonts/RobotoMono-Regular.ttf")) {
            return candidate;
        }
    }
    return "";
}

std::string defaultFontPath() {
    const std::string root = resourceRoot();
    if (!root.empty()) {
        return root + "/res/fonts/RobotoMono-Regular.ttf";
    }
#ifdef LCDCTL_SOURCE_DIR
    return std::string(LCDCTL_SOURCE_DIR) + "/res/fonts/RobotoMono-Regular.ttf";
#else
    return "res/fonts/RobotoMono-Regular.ttf";
#endif
}

}  // namespace lcdctl
