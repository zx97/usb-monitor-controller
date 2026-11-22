// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - application configuration.
//
// Reads the same config.yaml shape as the Python project (config: / display:)
// so an existing file keeps working, and adds an "app:" section for the new
// paths and daemon options.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "lcdctl/sensors.hpp"
#include "lcdctl/types.hpp"

namespace lcdctl {

struct Settings {
    // config:
    std::string comPort = "AUTO";
    std::string themeName;
    std::string hwSensors = "AUTO";     // AUTO|LINUX|STUB|STATIC|RANDOM
    std::string ethInterface;
    std::string wloInterface;
    std::string cpuFan = "AUTO";
    std::string pingAddress = "127.0.0.1";

    // display:
    std::string revision = "AUTO";
    int brightness = 50;
    // Which side of the panel is up. "AUTO" follows the theme layout; a fixed
    // value lets the user mount the screen on any of its four sides.
    //   portrait | landscape | reverse-portrait | reverse-landscape
    std::string displayOrientation = "AUTO";
    bool displayReverse = false;        // upstream compatibility (flips 180°)
    bool forceOrientation = false;      // apply even if it changes the aspect
    bool resetOnStartup = true;

    // app: (this project)
    std::string themeRoot;              // res/themes by default
    std::string fontsRoot;              // res/fonts by default
    std::string diskMount;   // empty => the filesystem holding $HOME
    bool trayIcon = true;               // show the KDE tray icon
    bool startAtLogin = false;          // write an XDG autostart entry / Run key
    bool trayTipShown = false;          // the "where is the tray icon" hint was shown
    std::string logFile;
    std::vector<std::pair<std::string, std::string>> customSensors;  // name -> command

    // Resolve the default theme/fonts roots (resource dir + environment).
    void fillDefaultPaths();

    static Settings load(const std::string& path);
    void save(const std::string& path) const;

    SensorSettings sensorSettings() const;

    // Autostart entry (KDE / any XDG desktop): ~/.config/autostart/<name>.desktop
    static std::string autostartFilePath();
    static bool autostartEnabled();
    static bool setAutostartEnabled(bool enabled, const std::string& command = "");
};

// Orientation actually sent to the panel. The theme fixes the layout aspect
// (portrait/landscape); the 180° flip is always allowed, a 90°/270° rotation is
// reported as a warning unless `Settings::forceOrientation` is set.
struct OrientationDecision {
    Orientation orientation = Orientation::Portrait;
    std::string warning;   // empty when there is nothing to report
};

OrientationDecision resolveOrientation(const Settings& settings,
                                       Orientation themeOrientation);

// The config file used when --config is not given:
// $LCDCTL_CONFIG, ./config.yaml, or ~/.config/usb-monitor-controller/config.yaml
std::string defaultConfigPath();

}  // namespace lcdctl
