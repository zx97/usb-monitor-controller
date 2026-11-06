// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/settings.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

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
    return envOr("HOME", "");
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
    const std::string home = homeDir();
    if (!home.empty()) {
        return (fs::path(home) / ".config" / "usb-monitor-controller" / "config.yaml").string();
    }
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
            settings.displayReverse = asBool(display["DISPLAY_REVERSE"], settings.displayReverse);
            settings.resetOnStartup = asBool(display["RESET_ON_STARTUP"], settings.resetOnStartup);
        }
        if (const YAML::Node app = root["app"]; app) {
            settings.themeRoot = scalar(app["THEME_ROOT"], settings.themeRoot);
            settings.fontsRoot = scalar(app["FONTS_DIR"], settings.fontsRoot);
            settings.diskMount = scalar(app["DISK_MOUNT"], settings.diskMount);
            settings.trayIcon = asBool(app["TRAY_ICON"], settings.trayIcon);
            settings.startAtLogin = asBool(app["START_AT_LOGIN"], settings.startAtLogin);
            settings.logFile = scalar(app["LOG_FILE"], settings.logFile);
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
    display["DISPLAY_REVERSE"] = displayReverse;
    display["RESET_ON_STARTUP"] = resetOnStartup;

    YAML::Node app = root["app"];
    app["THEME_ROOT"] = themeRoot;
    app["FONTS_DIR"] = fontsRoot;
    app["DISK_MOUNT"] = diskMount;
    app["TRAY_ICON"] = trayIcon;
    app["START_AT_LOGIN"] = startAtLogin;
    if (!logFile.empty()) {
        app["LOG_FILE"] = logFile;
    }

    std::ofstream stream(target);
    if (!stream) {
        throw std::runtime_error("cannot write " + target.string());
    }
    stream << YAML::Dump(root) << '\n';
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
    const std::string home = homeDir();
    if (home.empty()) {
        return "";
    }
    return (fs::path(home) / ".config" / "autostart" / "usb-monitor-controller.desktop").string();
}

bool Settings::autostartEnabled() {
    const std::string path = autostartFilePath();
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return fs::exists(path, ec);
}

bool Settings::setAutostartEnabled(bool enabled, const std::string& command) {
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

    std::ofstream stream(path);
    if (!stream) {
        return false;
    }
    stream << "[Desktop Entry]\n"
           << "Type=Application\n"
           << "Name=USB Monitor Controller\n"
           << "Comment=System monitor on a USB LCD screen\n"
           << "Exec=" << executable << " --tray\n"
           << "Icon=video-display\n"
           << "Terminal=false\n"
           << "X-KDE-autostart-after=panel\n"
           << "X-GNOME-Autostart-enabled=true\n"
           << "Categories=System;Monitor;\n";
    return true;
}

}  // namespace lcdctl
