// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - lcd-theme: inspect, validate and preview themes.
//
// Rendering goes through the same ThemeRenderer as the daemon and the editor,
// into an OffscreenCanvas: no screen and no hardware needed.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "lcdctl/image.hpp"
#include "lcdctl/logging.hpp"
#include "lcdctl/sensors.hpp"
#include "lcdctl/settings.hpp"
#include "lcdctl/theme.hpp"
#include "lcdctl/theme_render.hpp"
#include "lcdctl/types.hpp"

namespace {

namespace fs = std::filesystem;

const char* kUsage =
    "Usage: lcd-theme <command> [options]\n"
    "\n"
    "Commands:\n"
    "  list                          List themes found in --root\n"
    "  show <theme>                  Describe a theme (items, intervals, warnings)\n"
    "  validate <theme|directory>    Report structural problems\n"
    "  preview <theme|directory>     Render the theme to a PNG (no hardware)\n"
    "\n"
    "Options:\n"
    "  --root <dir>                  Themes root (default: res/themes, then the\n"
    "                                turing-smart-screen-python checkout)\n"
    "  --fonts <dir>                 Fonts root (default: res/fonts, then checkout)\n"
    "  --config <file>               Read paths from a config.yaml\n"
    "  --width <px> --height <px>    Screen size (default: theme background size)\n"
    "  --orientation <value>         portrait|landscape|reverse-portrait|reverse-landscape\n"
    "  --stub <spec>                 Sensor values: static | random | static:cpu=42,gpu=12\n"
    "  --output <file.png>, -o       Preview output (default: theme-preview.png)\n"
    "  --json                        Machine-readable list output\n"
    "  --verbose, -v                 Debug logging\n";

struct Options {
    std::map<std::string, std::string> values;
    std::vector<std::string> positional;

    bool flag(const std::string& name) const { return values.count(name) > 0; }
    std::string get(const std::string& name, const std::string& fallback = "") const {
        auto it = values.find(name);
        return it == values.end() ? fallback : it->second;
    }
    int getInt(const std::string& name, int fallback) const {
        auto it = values.find(name);
        if (it == values.end() || it->second.empty()) {
            return fallback;
        }
        return std::stoi(it->second);
    }
};

Options parseArgs(int argc, char** argv) {
    static const std::vector<std::string> booleanFlags = {"--json", "--verbose", "-v", "-h", "--help"};
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string token = argv[i];
        if (token == "-o") {
            token = "--output";
        } else if (token == "-v") {
            token = "--verbose";
        } else if (token == "-h") {
            token = "--help";
        }
        const bool known = token == "--root" || token == "--fonts" || token == "--config" ||
                           token == "--width" || token == "--height" || token == "--orientation" ||
                           token == "--stub" || token == "--output" || token == "--size";
        if (known) {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + token);
            }
            options.values[token] = argv[++i];
            continue;
        }
        if (std::find(booleanFlags.begin(), booleanFlags.end(), token) != booleanFlags.end()) {
            options.values[token] = "1";
            continue;
        }
        options.positional.push_back(token);
    }
    return options;
}

std::string resolveDirectory(const std::string& nameOrDir, const std::string& root) {
    std::error_code ec;
    if (fs::exists(nameOrDir, ec) && fs::is_directory(nameOrDir, ec)) {
        return nameOrDir;
    }
    if (root.empty()) {
        throw std::runtime_error("no theme root configured (use --root)");
    }
    const fs::path candidate = fs::path(root) / nameOrDir;
    if (!fs::exists(candidate, ec)) {
        throw std::runtime_error("theme not found: " + nameOrDir + " (looked in " + root + ")");
    }
    return candidate.string();
}

lcdctl::Size screenSize(const Options& options, const lcdctl::Theme& theme) {
    lcdctl::Size size{options.getInt("--width", 0), options.getInt("--height", 0)};
    const lcdctl::Size declared = theme.declaredSize();
    if (size.width <= 0) {
        size.width = declared.width > 0 ? declared.width : 320;
    }
    if (size.height <= 0) {
        size.height = declared.height > 0 ? declared.height : 480;
    }
    if (theme.orientation == lcdctl::Orientation::Landscape ||
        theme.orientation == lcdctl::Orientation::ReverseLandscape) {
        std::swap(size.width, size.height);
    }
    return size;
}

int commandList(const Options& options, const lcdctl::Settings& settings) {
    const std::string root = options.get("--root", settings.themeRoot);
    if (root.empty()) {
        std::cerr << "No theme root: pass --root or set app.THEME_ROOT in the config.\n";
        return 1;
    }
    const std::vector<lcdctl::ThemeInfo> themes = lcdctl::listThemes(root);
    const std::string sizeFilter = options.get("--size", "");

    if (options.flag("--json")) {
        std::cout << "[\n";
        for (std::size_t i = 0; i < themes.size(); ++i) {
            const lcdctl::ThemeInfo& theme = themes[i];
            std::cout << "  {\"name\":\"" << theme.name << "\",\"size\":\"" << theme.displaySize
                      << "\",\"author\":\"" << theme.author << "\",\"background\":"
                      << (theme.hasBackground ? "true" : "false") << "}"
                      << (i + 1 == themes.size() ? "\n" : ",\n");
        }
        std::cout << "]\n";
        return 0;
    }

    std::printf("%-34s %-7s %s\n", "THEME", "SIZE", "AUTHOR");
    for (const lcdctl::ThemeInfo& theme : themes) {
        if (!sizeFilter.empty() && theme.displaySize != sizeFilter) {
            continue;
        }
        std::printf("%-34s %-7s %s\n", theme.name.c_str(), theme.displaySize.c_str(),
                    theme.author.c_str());
    }
    std::printf("\n%d theme(s) in %s\n", static_cast<int>(themes.size()), root.c_str());
    return 0;
}

const char* kindName(lcdctl::ThemeItem::Kind kind) {
    switch (kind) {
        case lcdctl::ThemeItem::Kind::Text:
            return "TEXT";
        case lcdctl::ThemeItem::Kind::ProgressBar:
            return "GRAPH";
        case lcdctl::ThemeItem::Kind::LineGraph:
            return "LINE_GRAPH";
        case lcdctl::ThemeItem::Kind::Radial:
            return "RADIAL";
    }
    return "?";
}

int commandShow(const Options& options, const lcdctl::Settings& settings,
                const std::string& nameOrDir) {
    const std::string root = options.get("--root", settings.themeRoot);
    const std::string directory = resolveDirectory(nameOrDir, root);
    const lcdctl::Theme theme = lcdctl::Theme::load(directory, settings.fontsRoot);

    std::printf("theme      : %s\n", theme.name.c_str());
    std::printf("directory  : %s\n", theme.directory.c_str());
    std::printf("author     : %s\n", theme.author.c_str());
    std::printf("orientation: %s\n", lcdctl::orientationName(theme.orientation));
    const lcdctl::Size size = theme.declaredSize();
    std::printf("size       : %dx%d%s%s\n", size.width, size.height,
                theme.displaySize.empty() ? "" : "  (declared ",
                theme.displaySize.empty() ? "" : (theme.displaySize + "\")").c_str());
    std::printf("images     : %d, static texts: %d, items: %d\n",
                static_cast<int>(theme.images.size()), static_cast<int>(theme.texts.size()),
                static_cast<int>(theme.items.size()));

    int visible = 0;
    for (const lcdctl::ThemeItem& item : theme.items) {
        if (item.show) ++visible;
    }
    std::printf("visible    : %d\n\n", visible);

    std::printf("%-4s %-10s %-4s %s\n", "SHOW", "VIEW", "SEC", "KEY / METRIC");
    for (const lcdctl::ThemeItem& item : theme.items) {
        std::printf("%-4s %-10s %-4d %s  [%s]\n", item.show ? "yes" : "-", kindName(item.kind),
                    item.interval, item.key.c_str(), item.metric.c_str());
    }

    if (!theme.warnings.empty()) {
        std::printf("\nWarnings:\n");
        for (const std::string& warning : theme.warnings) {
            std::printf("  - %s\n", warning.c_str());
        }
    }
    return 0;
}

int commandValidate(const Options& options, const lcdctl::Settings& settings,
                    const std::string& nameOrDir) {
    const std::string root = options.get("--root", settings.themeRoot);
    const std::string directory = resolveDirectory(nameOrDir, root);
    const lcdctl::Theme theme = lcdctl::Theme::load(directory, settings.fontsRoot);
    const lcdctl::Size size = screenSize(options, theme);

    const std::vector<std::string> problems = theme.validate(size.width, size.height);
    for (const std::string& warning : theme.warnings) {
        std::printf("warning: %s\n", warning.c_str());
    }
    for (const std::string& problem : problems) {
        std::printf("error: %s\n", problem.c_str());
    }
    if (problems.empty()) {
        std::printf("OK: %s is valid for %dx%d\n", theme.name.c_str(), size.width, size.height);
        return 0;
    }
    return 1;
}

int commandPreview(const Options& options, const lcdctl::Settings& settings,
                   const std::string& nameOrDir) {
    const std::string root = options.get("--root", settings.themeRoot);
    const std::string directory = resolveDirectory(nameOrDir, root);
    const lcdctl::Theme theme = lcdctl::Theme::load(directory, settings.fontsRoot);
    const lcdctl::Size size = screenSize(options, theme);

    lcdctl::SensorSettings sensorSettings = settings.sensorSettings();
    const std::string stub = options.get("--stub", "static");
    auto sensors = lcdctl::makeStubSensors(stub, sensorSettings);
    lcdctl::Metrics metrics;
    sensors->refresh(metrics);

    lcdctl::OffscreenCanvas canvas(size, lcdctl::Color{0, 0, 0});
    lcdctl::ThemeRenderer renderer(theme, canvas);
    renderer.drawBackground();
    const int transmitted = renderer.drawAll(metrics);

    const std::string output = options.get("--output", "theme-preview.png");
    lcdctl::savePng(output, canvas.framebuffer());

    std::printf("Rendered %s (%dx%d) with stub '%s': %d item(s), %d warning(s) -> %s\n",
                theme.name.c_str(), size.width, size.height, stub.c_str(), transmitted,
                static_cast<int>(theme.warnings.size()), output.c_str());
    for (const std::string& warning : theme.warnings) {
        std::printf("  warning: %s\n", warning.c_str());
    }
    return 0;
}

int run(int argc, char** argv) {
    const Options options = parseArgs(argc, argv);
    if (options.flag("--help") || options.positional.empty()) {
        std::cout << kUsage;
        return options.flag("--help") ? 0 : 1;
    }

    lcdctl::Settings settings;
    const std::string configPath = options.get("--config", "");
    if (!configPath.empty()) {
        settings = lcdctl::Settings::load(configPath);
    } else {
        settings.fillDefaultPaths();
    }
    if (options.values.count("--root")) {
        settings.themeRoot = options.get("--root");
    }
    if (options.values.count("--fonts")) {
        settings.fontsRoot = options.get("--fonts");
    }

    const std::string command = options.positional[0];
    if (command == "list") {
        return commandList(options, settings);
    }
    if (options.positional.size() < 2) {
        throw std::invalid_argument(command + " requires a theme name or directory");
    }
    const std::string target = options.positional[1];
    if (command == "show") {
        return commandShow(options, settings, target);
    }
    if (command == "validate") {
        return commandValidate(options, settings, target);
    }
    if (command == "preview") {
        return commandPreview(options, settings, target);
    }
    throw std::invalid_argument("unknown command: " + command);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseArgs(argc, argv);
        if (options.flag("--verbose") || options.flag("-v")) {
            lcdctl::setLogLevel(lcdctl::LogLevel::Debug);
        }
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << std::endl;
        return 1;
    }
}
