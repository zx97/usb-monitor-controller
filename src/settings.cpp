// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/settings.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "lcdctl/logging.hpp"
#include "lcdctl/paths.hpp"

namespace lcdctl {
namespace {

namespace fs = std::filesystem;

std::string envOr(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : fallback;
}

std::string homeDir() {
#if defined(_WIN32)
    return envOr("USERPROFILE", "");
#else
    return envOr("HOME", "");
#endif
}

std::string scalar(const YAML::Node& node, const std::string& fallback = "") {
    if (!node || !node.IsScalar()) {
        return fallback;
    }
    try {
        return node.as<std::string>();
    } catch (...) {
        return fallback;
    }
}

int asInt(const YAML::Node& node, int fallback) {
    if (!node) {
        return fallback;
    }
    try {
        return node.as<int>();
    } catch (...) {
        return fallback;
    }
}

bool asBool(const YAML::Node& node, bool fallback) {
    if (!node || !node.IsScalar()) {
        return fallback;
    }
    const std::string text = scalar(node);
    if (text == "true" || text == "True" || text == "TRUE" || text == "1" || text == "yes") {
        return true;
    }
    if (text == "false" || text == "False" || text == "FALSE" || text == "0" || text == "no") {
        return false;
    }
    return fallback;
}

fs::path expanded(const std::string& path) {
    if (!path.empty() && path[0] == '~') {
        return fs::path(homeDir()) / path.substr(1);
    }
    return fs::path(path);
}

std::string joinIfExists(const fs::path& dir, const char* sub) {
    std::error_code ec;
    if (dir.empty()) {
        return "";
    }
    const fs::path candidate = dir / sub;
    return fs::is_directory(candidate, ec) ? candidate.string() : std::string();
}

}  // namespace

std::string defaultConfigPath() {
    const std::string fromEnv = envOr("LCDCTL_CONFIG", "");
    if (!fromEnv.empty()) {
        return fromEnv;
    }
    std::error_code ec;
    if (fs::exists("config.yaml", ec)) {
        return "config.yaml";
    }
#if defined(_WIN32)
    const std::string appData = envOr("APPDATA", homeDir());
    if (!appData.empty()) {
        return (fs::path(appData) / "usb-monitor-controller" / "config.yaml").string();
    }
#else
    const std::string home = homeDir();
    if (!home.empty()) {
        return (fs::path(home) / ".config" / "usb-monitor-controller" / "config.yaml").string();
    }
#endif
    return "config.yaml";
}

void Settings::fillDefaultPaths() {
    std::error_code ec;

    if (themeRoot.empty()) {
        themeRoot = envOr("LCDCTL_THEME_ROOT", "");
    }
    if (themeRoot.empty()) {
        const std::string root = resourceRoot();
        if (!root.empty()) {
            themeRoot = joinIfExists(fs::path(root) / "res", "themes");
        }
    }
    if (themeRoot.empty()) {
        // Development checkout: the Python project's themes work unchanged.
        for (const char* candidate : {"../turing-smart-screen-python/res/themes",
                                      "turing-smart-screen-python/res/themes"}) {
            if (fs::is_directory(candidate, ec)) {
                themeRoot = fs::absolute(candidate).lexically_normal().string();
                break;
            }
        }
    }

    if (fontsRoot.empty()) {
        fontsRoot = envOr("LCDCTL_FONTS_DIR", "");
    }
    if (fontsRoot.empty()) {
        const std::string root = resourceRoot();
        if (!root.empty()) {
            fontsRoot = joinIfExists(fs::path(root) / "res", "fonts");
        }
    }
    if (fontsRoot.empty()) {
        for (const char* candidate : {"../turing-smart-screen-python/res/fonts",
                                      "turing-smart-screen-python/res/fonts"}) {
            if (fs::is_directory(candidate, ec)) {
                fontsRoot = fs::absolute(candidate).lexically_normal().string();
                break;
            }
        }
    }
}

Settings Settings::load(const std::string& path) {
    Settings settings;
    std::error_code ec;
    if (!path.empty() && fs::exists(path, ec)) {
        YAML::Node root;
        try {
            root = YAML::LoadFile(path);
        } catch (const std::exception& error) {
            throw std::runtime_error("invalid configuration " + path + ": " + error.what());
        }

        if (const YAML::Node config = root["config"]; config) {
            settings.comPort = scalar(config["COM_PORT"], settings.comPort);
            settings.themeName = scalar(config["THEME"], settings.themeName);
            settings.hwSensors = scalar(config["HW_SENSORS"], settings.hwSensors);
            settings.ethInterface = scalar(config["ETH"], settings.ethInterface);
            settings.wloInterface = scalar(config["WLO"], settings.wloInterface);
            settings.cpuFan = scalar(config["CPU_FAN"], settings.cpuFan);
            settings.pingAddress = scalar(config["PING"], settings.pingAddress);
        }
        if (const YAML::Node display = root["display"]; display) {
            settings.revision = scalar(display["REVISION"], settings.revision);
            settings.brightness = asInt(display["BRIGHTNESS"], settings.brightness);
            settings.displayOrientation =
                scalar(display["DISPLAY_ORIENTATION"], settings.displayOrientation);
            settings.displayReverse = asBool(display["DISPLAY_REVERSE"], settings.displayReverse);
            settings.forceOrientation =
                asBool(display["FORCE_ORIENTATION"], settings.forceOrientation);
            settings.resetOnStartup = asBool(display["RESET_ON_STARTUP"], settings.resetOnStartup);
        }
        if (const YAML::Node app = root["app"]; app) {
            settings.themeRoot = scalar(app["THEME_ROOT"], settings.themeRoot);
            settings.fontsRoot = scalar(app["FONTS_DIR"], settings.fontsRoot);
            settings.diskMount = scalar(app["DISK_MOUNT"], settings.diskMount);
            settings.trayIcon = asBool(app["TRAY_ICON"], settings.trayIcon);
            settings.startAtLogin = asBool(app["START_AT_LOGIN"], settings.startAtLogin);
            settings.logFile = scalar(app["LOG_FILE"], settings.logFile);
            settings.trayTipShown = asBool(app["TRAY_TIP_SHOWN"], settings.trayTipShown);
            if (const YAML::Node custom = app["CUSTOM_SENSORS"]; custom && custom.IsMap()) {
                for (auto it = custom.begin(); it != custom.end(); ++it) {
                    settings.customSensors.emplace_back(scalar(it->first),
                                                        scalar(it->second));
                }
            }
        }
    }

    settings.fillDefaultPaths();
    return settings;
}

void Settings::save(const std::string& path) const {
    std::error_code ec;
    const fs::path target = expanded(path);
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
    }

    YAML::Node root;
    if (fs::exists(target, ec)) {
        try {
            root = YAML::LoadFile(target.string());
        } catch (const std::exception&) {
            root = YAML::Node(YAML::NodeType::Map);
        }
    }
    if (!root || !root.IsMap()) {
        root = YAML::Node(YAML::NodeType::Map);
    }

    YAML::Node config = root["config"];
    config["COM_PORT"] = comPort;
    config["THEME"] = themeName;
    config["HW_SENSORS"] = hwSensors;
    config["ETH"] = ethInterface;
    config["WLO"] = wloInterface;
    config["CPU_FAN"] = cpuFan;
    config["PING"] = pingAddress;

    YAML::Node display = root["display"];
    display["REVISION"] = revision;
    display["BRIGHTNESS"] = brightness;
    display["DISPLAY_ORIENTATION"] = displayOrientation;
    display["FORCE_ORIENTATION"] = forceOrientation;
    display["DISPLAY_REVERSE"] = displayReverse;
    display["RESET_ON_STARTUP"] = resetOnStartup;

    YAML::Node app = root["app"];
    app["THEME_ROOT"] = themeRoot;
    app["FONTS_DIR"] = fontsRoot;
    app["DISK_MOUNT"] = diskMount;
    app["TRAY_ICON"] = trayIcon;
    app["START_AT_LOGIN"] = startAtLogin;
    app["TRAY_TIP_SHOWN"] = trayTipShown;
    if (!logFile.empty()) {
        app["LOG_FILE"] = logFile;
    }

    std::ofstream stream(target);
    if (!stream) {
        throw std::runtime_error("cannot write " + target.string());
    }
    stream << YAML::Dump(root) << '\n';
}

OrientationDecision resolveOrientation(const Settings& settings,
                                       Orientation themeOrientation) {
    OrientationDecision decision;
    decision.orientation = themeOrientation;

    const std::string requested = settings.displayOrientation;
    if (!requested.empty() && requested != "AUTO") {
        Orientation parsed = themeOrientation;
        if (parseOrientation(requested, parsed)) {
            decision.orientation = parsed;
        } else {
            decision.warning = "unknown DISPLAY_ORIENTATION '" + requested +
                               "', keeping " + orientationName(themeOrientation);
        }
    } else if (settings.displayReverse) {
        decision.orientation = flippedOrientation(themeOrientation);
    }

    if (!sameAspect(decision.orientation, themeOrientation)) {
        const std::string message =
            std::string("orientation ") + orientationName(decision.orientation) +
            " does not match this theme (" + orientationName(themeOrientation) +
            " layout): the screen will be cropped";
        if (settings.forceOrientation) {
            decision.warning = message;
        } else {
            decision.warning = message + " — keeping " + orientationName(themeOrientation) +
                               " (set FORCE_ORIENTATION to override)";
            decision.orientation = themeOrientation;
        }
    }
    return decision;
}

SensorSettings Settings::sensorSettings() const {
    SensorSettings sensors;
    sensors.ethInterface = ethInterface;
    sensors.wloInterface = wloInterface;
    sensors.diskMount = diskMount;  // empty => $HOME filesystem
    sensors.cpuFan = cpuFan;
    sensors.customCommands = customSensors;
    return sensors;
}

std::string Settings::autostartFilePath() {
#if defined(_WIN32)
    return "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"
           "\\USB Monitor Controller";
#else
    const std::string home = homeDir();
    if (home.empty()) {
        return "";
    }
    return (fs::path(home) / ".config" / "autostart" / "usb-monitor-controller.desktop").string();
#endif
}

bool Settings::autostartEnabled() {
#if defined(_WIN32)
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    const LONG status = ::RegQueryValueExW(key, L"USB Monitor Controller", nullptr, nullptr, nullptr,
                                           nullptr);
    ::RegCloseKey(key);
    return status == ERROR_SUCCESS;
#else
    const std::string path = autostartFilePath();
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return fs::exists(path, ec);
#endif
}

bool Settings::setAutostartEnabled(bool enabled, const std::string& command) {
#if defined(_WIN32)
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr,
                          0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LONG status = ERROR_SUCCESS;
    if (enabled) {
        std::string executable = command;
        if (executable.empty()) {
            wchar_t buffer[MAX_PATH * 4] = {0};
            if (::GetModuleFileNameW(nullptr, buffer, MAX_PATH * 4) > 0) {
                executable = std::filesystem::path(buffer).string();
            }
        }
        const std::wstring value = std::wstring(L"\"") + std::wstring(executable.begin(), executable.end()) +
                                   L"\" --tray";
        status = ::RegSetValueExW(key, L"USB Monitor Controller", 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(value.c_str()),
                                  static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        status = ::RegDeleteValueW(key, L"USB Monitor Controller");
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    }
    ::RegCloseKey(key);
    return status == ERROR_SUCCESS;
#else
    const std::string path = autostartFilePath();
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    if (!enabled) {
        fs::remove(path, ec);
        return !ec;
    }
    fs::create_directories(fs::path(path).parent_path(), ec);

    std::string executable = command;
    if (executable.empty()) {
        executable = fs::read_symlink("/proc/self/exe", ec).string();
        if (ec) {
            executable = "lcd-monitor";
        }
    }

    // Use the bundled monitor icon when it is available (absolute path is
    // required for XDG entries outside the icon theme).
    std::string icon = "video-display";
    const std::string root = resourceRoot();
    if (!root.empty()) {
        const std::string candidate = root + "/res/icons/monitor-icon-17865/64.png";
        if (std::filesystem::exists(candidate, ec)) {
            icon = candidate;
        }
    }

    std::ofstream stream(path);
    if (!stream) {
        return false;
    }
    stream << "[Desktop Entry]\n"
           << "Type=Application\n"
           << "Name=USB Monitor Controller\n"
           << "Comment=System monitor on a USB LCD screen\n"
           << "Exec=" << executable << " --tray\n"
           << "Icon=" << icon << "\n"
           << "Terminal=false\n"
           << "X-KDE-autostart-after=panel\n"
           << "X-GNOME-Autostart-enabled=true\n"
           << "Categories=System;Monitor;\n";
    return true;
#endif
}

}  // namespace lcdctl
