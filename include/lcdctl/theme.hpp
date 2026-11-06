// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - theme model (res/themes/<name>/theme.yaml).
//
// Ported from library/stats.py and library/display.py of
// https://github.com/mathoudebine/turing-smart-screen-python/
//
// A theme is a background image, a set of static texts and a tree of "stats"
// views (TEXT / GRAPH / LINE_GRAPH / RADIAL) bound to a metric. The YAML files
// written for the Python project are read as-is: nothing is converted, the
// upstream res/themes directory can be used directly.

#pragma once

#include <string>
#include <vector>

#include "lcdctl/draw.hpp"
#include "lcdctl/types.hpp"

namespace lcdctl {

// A metric the theme can display. Kept as a string id ("cpu.percentage") so
// the editor and the renderer can round-trip unknown/forward-compatible values.
struct ThemeItem {
    enum class Kind { Text, ProgressBar, LineGraph, Radial };

    Kind kind = Kind::Text;
    std::string key;      // unique: "STATS/CPU/PERCENTAGE/TEXT"
    std::string metric;   // "cpu.percentage", "mem.used", "net.eth.upload" ...
    int interval = 1;     // seconds; 0 => never refreshed (disabled)
    bool show = false;    // SHOW of the view

    // Style used by the matching primitive.
    TextStyle text;
    ProgressBarStyle bar;
    LineGraphStyle graph;
    RadialStyle radial;

    int historySize = 10;
    bool showUnit = true;
    int minSize = 0;
    int valueMin = 0;
    int valueMax = 100;

    // Region occupied on screen, in portrait coordinates (used for validation
    // and for the editor's selection boxes).
    Rect bounds() const;
};

struct ThemeImage {
    std::string name;     // key in the YAML map ("BACKGROUND")
    std::string path;     // resolved absolute path
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct ThemeText {
    std::string name;
    std::string text;
    int x = 0;
    int y = 0;
    TextStyle style;
};

struct Theme {
    std::string name;
    std::string directory;
    std::string fontsRoot;
    std::string author;
    std::string displaySize;      // DISPLAY_SIZE as written in the theme
    Orientation orientation = Orientation::Portrait;
    bool hasBackplateLed = false;
    Color backplateLed{0, 0, 0};

    std::vector<ThemeImage> images;
    std::vector<ThemeText> texts;
    std::vector<ThemeItem> items;

    // Non-fatal problems found while loading (missing font, unsupported key...).
    std::vector<std::string> warnings;

    // Load res/themes/<name>/theme.yaml. `fontsDir` is the root of the font
    // tree (the Python project uses res/fonts, with one directory per family).
    static Theme load(const std::string& directory, const std::string& fontsDir);

    // Same, looking the theme up by name under `themesRoot`.
    static Theme loadByName(const std::string& name, const std::string& themesRoot,
                            const std::string& fontsDir);

    // Structural checks: items outside the screen, missing files, unknown
    // sensors. Returns human-readable problems (empty => valid).
    std::vector<std::string> validate(int screenWidth, int screenHeight) const;

    const ThemeItem* findItem(const std::string& key) const;
    ThemeItem* findItem(const std::string& key);

    // Screen size declared by the theme (0x0 when DISPLAY_SIZE is missing).
    Size declaredSize() const;

    // Serialize back to YAML. `original` is the YAML document as loaded (kept so
    // unrelated keys and the file structure survive an editor save).
    std::string toYaml() const;
    static Theme fromYaml(const std::string& yamlText, const std::string& directory,
                          const std::string& fontsDir);

    // Raw document the theme was parsed from, re-emitted on save so keys the
    // editor does not understand are preserved.
    std::string rawYaml;
};

// A theme folder found under a themes root.
struct ThemeInfo {
    std::string name;
    std::string directory;
    std::string author;
    std::string displaySize;
    bool hasBackground = false;
};

// List every sub-directory of `themesRoot` that holds a theme.yaml.
std::vector<ThemeInfo> listThemes(const std::string& themesRoot);

// "3.5" for DISPLAY_SIZE: 3.5" (quotes and double-quote markers removed).
std::string normalizeDisplaySize(const std::string& raw);

// Fonts: resolve a theme FONT value ("roboto-mono/RobotoMono-Bold.ttf") against
// the bundled fonts dir, the theme dir, then fall back to the default font.
std::string resolveFontPath(const std::string& fontsDir, const std::string& themeDir,
                            const std::string& value);

}  // namespace lcdctl
