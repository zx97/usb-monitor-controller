// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/theme.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

#include "lcdctl/image.hpp"
#include "lcdctl/logging.hpp"
#include "lcdctl/paths.hpp"

namespace lcdctl {
namespace {

namespace fs = std::filesystem;

// Read a child node. Two yaml-cpp gotchas are handled here:
//   * `YAML::Node()` is a *defined* null node (truthy), so "absent" must be
//     NodeType::Undefined, otherwise every missing key looks present;
//   * `node[key]` on a missing key yields an invalid node whose accessors throw.
// Cloning keeps the parsing side read-only and free of lifetime surprises.
YAML::Node child(const YAML::Node& node, const std::string& key) {
    if (!node || !node.IsMap()) {
        return YAML::Node(YAML::NodeType::Undefined);
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar()) {
            continue;
        }
        if (it->first.Scalar() == key) {
            return YAML::Clone(it->second);
        }
    }
    return YAML::Node(YAML::NodeType::Undefined);
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

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool asBool(const YAML::Node& node, bool fallback) {
    if (!node) {
        return fallback;
    }
    if (node.IsScalar()) {
        const std::string text = lower(scalar(node));
        if (text == "true" || text == "yes" || text == "on" || text == "1") {
            return true;
        }
        if (text == "false" || text == "no" || text == "off" || text == "0" || text.empty()) {
            return false;
        }
    }
    try {
        return node.as<bool>();
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
        try {
            return static_cast<int>(node.as<double>());
        } catch (...) {
            return fallback;
        }
    }
}

double asDouble(const YAML::Node& node, double fallback) {
    if (!node) {
        return fallback;
    }
    try {
        return node.as<double>();
    } catch (...) {
        return fallback;
    }
}

Color asColor(const YAML::Node& node, Color fallback) {
    if (!node) {
        return fallback;
    }
    try {
        if (node.IsSequence() && node.size() >= 3) {
            Color color;
            color.r = static_cast<std::uint8_t>(node[0].as<int>());
            color.g = static_cast<std::uint8_t>(node[1].as<int>());
            color.b = static_cast<std::uint8_t>(node[2].as<int>());
            return color;
        }
        const std::string text = scalar(node);
        if (!text.empty()) {
            return parseColor(text);
        }
    } catch (const std::exception&) {
        // A malformed colour must never break a theme.
    }
    return fallback;
}

int intervalOf(const YAML::Node& node, int inherited) {
    const YAML::Node interval = child(node, "INTERVAL");
    return interval ? asInt(interval, inherited) : inherited;
}

void parseTextStyle(const YAML::Node& node, Theme& theme, const std::string& context,
                    TextStyle& style) {
    style.x = asInt(child(node, "X"), 0);
    style.y = asInt(child(node, "Y"), 0);
    style.width = asInt(child(node, "WIDTH"), 0);
    style.height = asInt(child(node, "HEIGHT"), 0);
    style.fontSize = asInt(child(node, "FONT_SIZE"), 10);
    style.fontColor = asColor(child(node, "FONT_COLOR"), Color{0, 0, 0});
    style.backgroundColor = asColor(child(node, "BACKGROUND_COLOR"), Color{255, 255, 255});
    style.align = scalar(child(node, "ALIGN"), "left");
    style.anchor = scalar(child(node, "ANCHOR"), "lt");

    const std::string image = scalar(child(node, "BACKGROUND_IMAGE"), "");
    if (!image.empty()) {
        style.backgroundImage = (fs::path(theme.directory) / image).string();
    }

    const std::string font = scalar(child(node, "FONT"), "");
    if (!font.empty()) {
        style.font = resolveFontPath(theme.fontsRoot, theme.directory, font);
        std::error_code ec;
        if (!fs::exists(style.font, ec)) {
            style.font.clear();
            if (theme.warnings.size() < 40) {
                theme.warnings.push_back(context + ": font not found: " + font +
                                         " (using the bundled default)");
            }
        }
    }
}

// One metric displayed through up to four views.
struct ViewSet {
    std::string key;
    std::string metric;
    int interval = 1;
    int valueMin = 0;
    int valueMax = 100;
};

void addTextItem(const YAML::Node& text, const ViewSet& set, const std::string& metricSuffix,
                 const std::string& keySuffix, int defaultMinSize, Theme& theme) {
    if (!text) {
        return;
    }
    ThemeItem item;
    item.kind = ThemeItem::Kind::Text;
    item.key = set.key + keySuffix;
    item.metric = set.metric + metricSuffix;
    item.interval = set.interval;
    item.show = asBool(child(text, "SHOW"), false);
    item.showUnit = asBool(child(text, "SHOW_UNIT"), true);
    item.minSize = asInt(child(text, "MIN_SIZE"), defaultMinSize);
    item.valueMin = set.valueMin;
    item.valueMax = set.valueMax;
    parseTextStyle(text, theme, item.key, item.text);
    theme.items.push_back(std::move(item));
}

void addControls(const YAML::Node& section, const ViewSet& set, Theme& theme,
                 bool skipText = false) {
    if (!skipText) {
        addTextItem(child(section, "TEXT"), set, "", "/TEXT", 0, theme);
    }

    if (const YAML::Node graph = child(section, "GRAPH"); graph) {
        ThemeItem item;
        item.kind = ThemeItem::Kind::ProgressBar;
        item.key = set.key + "/GRAPH";
        item.metric = set.metric;
        item.interval = set.interval;
        item.show = asBool(child(graph, "SHOW"), false);
        item.valueMin = asInt(child(graph, "MIN_VALUE"), set.valueMin);
        item.valueMax = asInt(child(graph, "MAX_VALUE"), set.valueMax);
        item.bar.x = asInt(child(graph, "X"), 0);
        item.bar.y = asInt(child(graph, "Y"), 0);
        item.bar.width = asInt(child(graph, "WIDTH"), 0);
        item.bar.height = asInt(child(graph, "HEIGHT"), 0);
        item.bar.minValue = item.valueMin;
        item.bar.maxValue = item.valueMax;
        item.bar.barColor = asColor(child(graph, "BAR_COLOR"), Color{0, 0, 0});
        item.bar.barOutline = asBool(child(graph, "BAR_OUTLINE"), false);
        item.bar.backgroundColor = asColor(child(graph, "BACKGROUND_COLOR"), Color{255, 255, 255});
        item.bar.reverseDirection = asBool(child(graph, "REVERSE_DIRECTION"), false);
        const std::string image = scalar(child(graph, "BACKGROUND_IMAGE"), "");
        if (!image.empty()) {
            item.bar.backgroundImage = (fs::path(theme.directory) / image).string();
        }
        theme.items.push_back(std::move(item));
    }

    if (const YAML::Node radial = child(section, "RADIAL"); radial) {
        ThemeItem item;
        item.kind = ThemeItem::Kind::Radial;
        item.key = set.key + "/RADIAL";
        item.metric = set.metric;
        item.interval = set.interval;
        item.show = asBool(child(radial, "SHOW"), false);
        item.showUnit = asBool(child(radial, "SHOW_UNIT"), true);
        item.valueMin = asInt(child(radial, "MIN_VALUE"), set.valueMin);
        item.valueMax = asInt(child(radial, "MAX_VALUE"), set.valueMax);
        item.radial.x = asInt(child(radial, "X"), 0);
        item.radial.y = asInt(child(radial, "Y"), 0);
        item.radial.radius = asInt(child(radial, "RADIUS"), 1);
        item.radial.barWidth = asInt(child(radial, "WIDTH"), 1);
        item.radial.minValue = item.valueMin;
        item.radial.maxValue = item.valueMax;
        item.radial.angleStart = asDouble(child(radial, "ANGLE_START"), 0);
        item.radial.angleEnd = asDouble(child(radial, "ANGLE_END"), 360);
        item.radial.angleSteps = asInt(child(radial, "ANGLE_STEPS"), 1);
        item.radial.angleSep = asInt(child(radial, "ANGLE_SEP"), 0);
        item.radial.clockwise = asBool(child(radial, "CLOCKWISE"), false);
        item.radial.withText = asBool(child(radial, "SHOW_TEXT"), false);
        item.radial.barColor = asColor(child(radial, "BAR_COLOR"), Color{0, 0, 0});
        item.radial.backgroundColor = asColor(child(radial, "BACKGROUND_COLOR"), Color{0, 0, 0});
        item.radial.barBackgroundColor =
            asColor(child(radial, "BAR_BACKGROUND_COLOR"), Color{0, 0, 0});
        item.radial.drawBarBackground = asBool(child(radial, "DRAW_BAR_BACKGROUND"), false);
        item.radial.fontSize = asInt(child(radial, "FONT_SIZE"), 10);
        item.radial.fontColor = asColor(child(radial, "FONT_COLOR"), Color{0, 0, 0});
        const std::string font = scalar(child(radial, "FONT"), "");
        if (!font.empty()) {
            item.radial.font = resolveFontPath(theme.fontsRoot, theme.directory, font);
        }
        const std::string image = scalar(child(radial, "BACKGROUND_IMAGE"), "");
        if (!image.empty()) {
            item.radial.backgroundImage = (fs::path(theme.directory) / image).string();
        }
        theme.items.push_back(std::move(item));
    }

    if (const YAML::Node lineGraph = child(section, "LINE_GRAPH"); lineGraph) {
        ThemeItem item;
        item.kind = ThemeItem::Kind::LineGraph;
        item.key = set.key + "/LINE_GRAPH";
        item.metric = set.metric;
        item.interval = set.interval;
        item.show = asBool(child(lineGraph, "SHOW"), false);
        item.graph.x = asInt(child(lineGraph, "X"), 0);
        item.graph.y = asInt(child(lineGraph, "Y"), 0);
        item.graph.width = asInt(child(lineGraph, "WIDTH"), 0);
        item.graph.height = asInt(child(lineGraph, "HEIGHT"), 0);
        item.graph.minValue =
            static_cast<float>(asDouble(child(lineGraph, "MIN_VALUE"), set.valueMin));
        item.graph.maxValue =
            static_cast<float>(asDouble(child(lineGraph, "MAX_VALUE"), set.valueMax));
        item.graph.autoscale = asBool(child(lineGraph, "AUTOSCALE"), false);
        item.graph.lineColor = asColor(child(lineGraph, "LINE_COLOR"), Color{0, 0, 0});
        item.graph.lineWidth = asInt(child(lineGraph, "LINE_WIDTH"), 2);
        item.graph.graphAxis = asBool(child(lineGraph, "AXIS"), false);
        item.graph.axisColor = asColor(child(lineGraph, "AXIS_COLOR"), item.graph.lineColor);
        item.graph.backgroundColor = asColor(child(lineGraph, "BACKGROUND_COLOR"), Color{0, 0, 0});
        const std::string image = scalar(child(lineGraph, "BACKGROUND_IMAGE"), "");
        if (!image.empty()) {
            item.graph.backgroundImage = (fs::path(theme.directory) / image).string();
        }
        item.historySize = std::clamp(asInt(child(lineGraph, "HISTORY_SIZE"), 10), 2, 200);
        theme.items.push_back(std::move(item));
    }
}

void parseStats(const YAML::Node& stats, Theme& theme) {
    // ---- CPU -------------------------------------------------------------
    if (const YAML::Node cpu = child(stats, "CPU"); cpu) {
        const int sectionInterval = intervalOf(cpu, 1);

        for (const char* name : {"PERCENTAGE", "FREQUENCY", "TEMPERATURE", "FAN_SPEED"}) {
            const YAML::Node node = child(cpu, name);
            if (!node) {
                continue;
            }
            const std::string metric =
                std::string("cpu.") + lower(name == std::string("FAN_SPEED") ? "fan" : name);
            ViewSet set{"STATS/CPU/" + std::string(name), metric, intervalOf(node, sectionInterval),
                        0, 100};
            addControls(node, set, theme);
        }

        if (const YAML::Node load = child(cpu, "LOAD"); load) {
            const int interval = intervalOf(load, sectionInterval);
            for (const char* part : {"ONE", "FIVE", "FIFTEEN"}) {
                const YAML::Node text = child(child(load, part), "TEXT");
                addTextItem(text, ViewSet{"STATS/CPU/LOAD/" + std::string(part),
                                          std::string("cpu.load.") + lower(part), interval, 0, 100},
                            "", "/TEXT", 3, theme);
            }
        }
    }

    // ---- GPU -------------------------------------------------------------
    if (const YAML::Node gpu = child(stats, "GPU"); gpu) {
        const int sectionInterval = intervalOf(gpu, 1);
        struct GpuView {
            const char* name;
            const char* metric;
            int max;
        };
        static const GpuView views[] = {{"PERCENTAGE", "gpu.percentage", 100},
                                        {"MEMORY_PERCENT", "gpu.memoryPercent", 100},
                                        {"TEMPERATURE", "gpu.temperature", 100},
                                        {"FREQUENCY", "gpu.frequency", 3000},
                                        {"FAN_SPEED", "gpu.fan", 100},
                                        {"FPS", "gpu.fps", 200}};
        for (const GpuView& view : views) {
            const YAML::Node node = child(gpu, view.name);
            if (!node) {
                continue;
            }
            ViewSet set{"STATS/GPU/" + std::string(view.name), view.metric,
                        intervalOf(node, sectionInterval), 0, view.max};
            addControls(node, set, theme);
        }
        // Backward-compatible GPU/MEMORY: TEXT is absolute MB, GRAPH/RADIAL
        // use the relative percentage (same split as stats.py).
        if (const YAML::Node memory = child(gpu, "MEMORY"); memory) {
            ViewSet set{"STATS/GPU/MEMORY", "gpu.memory", intervalOf(memory, sectionInterval), 0, 100};
            addControls(memory, set, theme, /*skipText=*/true);
            addTextItem(child(memory, "TEXT"),
                        ViewSet{set.key, "gpu.memory.bytes", set.interval, 0, 100}, "", "/TEXT", 5,
                        theme);
        }
        if (const YAML::Node formatted = child(gpu, "FORMATTED"); formatted) {
            addTextItem(child(formatted, "TEXT"),
                        ViewSet{"STATS/GPU/FORMATTED", "gpu.formatted", sectionInterval, 0, 100}, "",
                        "/TEXT", 0, theme);
        }
    }

    // ---- MEMORY ----------------------------------------------------------
    if (const YAML::Node memory = child(stats, "MEMORY"); memory) {
        const int sectionInterval = intervalOf(memory, 1);
        for (const char* name : {"VIRTUAL", "SWAP"}) {
            const YAML::Node node = child(memory, name);
            if (!node) {
                continue;
            }
            const std::string metric = std::string("mem.") + lower(name);
            ViewSet set{"STATS/MEMORY/" + std::string(name), metric,
                        intervalOf(node, sectionInterval), 0, 100};
            addControls(node, set, theme);
            for (const char* part : {"USED", "FREE", "TOTAL"}) {
                addTextItem(child(child(node, part), "TEXT"),
                            ViewSet{set.key + "/" + part, metric + "." + lower(part), set.interval, 0,
                                    100},
                            "", "/TEXT", 5, theme);
            }
            addTextItem(child(node, "PERCENT_TEXT"),
                        ViewSet{set.key, metric + ".percent", set.interval, 0, 100}, "",
                        "/PERCENT_TEXT", 3, theme);
        }
    }

    // ---- DISK ------------------------------------------------------------
    if (const YAML::Node disk = child(stats, "DISK"); disk) {
        const int sectionInterval = intervalOf(disk, 1);
        if (const YAML::Node used = child(disk, "USED"); used) {
            ViewSet set{"STATS/DISK/USED", "disk.used", intervalOf(used, sectionInterval), 0, 100};
            // TEXT shows bytes here, not the percentage: added separately.
            addControls(used, set, theme, /*skipText=*/true);
            addTextItem(child(used, "TEXT"),
                        ViewSet{set.key, "disk.used.bytes", set.interval, 0, 100}, "", "/TEXT", 5,
                        theme);
            addTextItem(child(used, "PERCENT_TEXT"),
                        ViewSet{set.key, "disk.percent", set.interval, 0, 100}, "",
                        "/PERCENT_TEXT", 3, theme);
        }
        for (const char* part : {"FREE", "TOTAL"}) {
            addTextItem(child(child(disk, part), "TEXT"),
                        ViewSet{std::string("STATS/DISK/") + part,
                                std::string("disk.") + lower(part) + ".bytes", sectionInterval, 0,
                                100},
                        "", "/TEXT", 5, theme);
        }
    }

    // ---- NET -------------------------------------------------------------
    if (const YAML::Node net = child(stats, "NET"); net) {
        const int sectionInterval = intervalOf(net, 1);
        for (const char* card : {"ETH", "WLO"}) {
            const YAML::Node cardNode = child(net, card);
            if (!cardNode) {
                continue;
            }
            for (const char* view : {"UPLOAD", "DOWNLOAD", "UPLOADED", "DOWNLOADED"}) {
                const YAML::Node node = child(cardNode, view);
                if (!node) {
                    continue;
                }
                const std::string metric = std::string("net.") + lower(card) + "." + lower(view);
                ViewSet set{std::string("STATS/NET/") + card + "/" + view, metric,
                            intervalOf(node, sectionInterval), 0, 1000000000};
                addControls(node, set, theme);
            }
        }
    }

    // ---- DATE ------------------------------------------------------------
    if (const YAML::Node date = child(stats, "DATE"); date) {
        const int sectionInterval = intervalOf(date, 1);
        for (const char* part : {"DAY", "HOUR"}) {
            addTextItem(child(child(date, part), "TEXT"),
                        ViewSet{std::string("STATS/DATE/") + part,
                                std::string("date.") + lower(part), sectionInterval, 0, 100},
                        "", "/TEXT", 0, theme);
        }
    }

    // ---- UPTIME ----------------------------------------------------------
    if (const YAML::Node uptime = child(stats, "UPTIME"); uptime) {
        const int sectionInterval = intervalOf(uptime, 1);
        for (const char* part : {"SECONDS", "FORMATTED"}) {
            addTextItem(child(child(uptime, part), "TEXT"),
                        ViewSet{std::string("STATS/UPTIME/") + part,
                                std::string("uptime.") + lower(part), sectionInterval, 0, 100},
                        "", "/TEXT", 0, theme);
        }
    }

    // ---- CUSTOM ----------------------------------------------------------
    if (const YAML::Node custom = child(stats, "CUSTOM"); custom && custom.IsMap()) {
        const int sectionInterval = intervalOf(custom, 1);
        for (auto it = custom.begin(); it != custom.end(); ++it) {
            const std::string name = scalar(it->first);
            if (name.empty() || name == "INTERVAL") {
                continue;
            }
            const YAML::Node entry = it->second;
            ViewSet set{"STATS/CUSTOM/" + name, "custom." + name,
                        intervalOf(entry, sectionInterval), 0, 100};
            addControls(entry, set, theme);
        }
    }

    for (const char* unsupported : {"WEATHER", "PING"}) {
        if (child(stats, unsupported)) {
            theme.warnings.push_back(std::string("STATS/") + unsupported +
                                     " is present but not implemented yet (ignored)");
        }
    }
}

void patchTextNode(YAML::Node node, const TextStyle& style) {
    if (!node || !node.IsMap()) {
        return;
    }
    node["X"] = style.x;
    node["Y"] = style.y;
    node["FONT_SIZE"] = style.fontSize;
    node["FONT_COLOR"] = std::to_string(style.fontColor.r) + ", " +
                         std::to_string(style.fontColor.g) + ", " +
                         std::to_string(style.fontColor.b);
    node["BACKGROUND_COLOR"] = std::to_string(style.backgroundColor.r) + ", " +
                               std::to_string(style.backgroundColor.g) + ", " +
                               std::to_string(style.backgroundColor.b);
    node["ALIGN"] = style.align;
    node["ANCHOR"] = style.anchor;
    if (style.width > 0) {
        node["WIDTH"] = style.width;
    }
    if (style.height > 0) {
        node["HEIGHT"] = style.height;
    }
}

// Walk (and create) a path inside a YAML document. yaml-cpp nodes are handles:
// `node.reset(child)` is required, a plain assignment would replace the value
// the handle points at and corrupt the document.
YAML::Node descend(YAML::Node root, const std::string& path) {
    YAML::Node node = root;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string part =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) {
            if (!node[part]) {
                node[part] = YAML::Node(YAML::NodeType::Map);
            }
            YAML::Node next = node[part];
            node.reset(next);
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return node;
}

}  // namespace

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

std::string normalizeDisplaySize(const std::string& raw) {
    std::string value = raw;
    while (!value.empty() && (value.front() == '"' || value.front() == '\'')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == '"' || value.back() == '\'')) {
        value.pop_back();
    }
    return value;
}

std::string resolveFontPath(const std::string& fontsDir, const std::string& themeDir,
                            const std::string& value) {
    if (value.empty()) {
        return "";
    }
    std::error_code ec;
    const fs::path direct(value);
    if (direct.is_absolute() && fs::exists(direct, ec)) {
        return direct.string();
    }
    if (!fontsDir.empty()) {
        const fs::path candidate = fs::path(fontsDir) / direct;
        if (fs::exists(candidate, ec)) {
            return candidate.string();
        }
    }
    if (!themeDir.empty()) {
        const fs::path candidate = fs::path(themeDir) / direct;
        if (fs::exists(candidate, ec)) {
            return candidate.string();
        }
    }
    const fs::path bundled =
        fs::path(resourceRoot()) / "fonts" / direct.filename();
    if (fs::exists(bundled, ec)) {
        return bundled.string();
    }
    // Development convenience: the upstream checkout ships the full font tree.
    // LCDCTL_EXTRA_FONTS points at another font root (colon-separated).
    std::vector<fs::path> extraRoots;
    if (const char* extra = std::getenv("LCDCTL_EXTRA_FONTS"); extra && *extra) {
        std::string list(extra);
        std::size_t start = 0;
        while (start <= list.size()) {
            const std::size_t colon = list.find(':', start);
            extraRoots.emplace_back(list.substr(start, colon == std::string::npos
                                                           ? std::string::npos
                                                           : colon - start));
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
    }
    if (!fontsDir.empty()) {
        extraRoots.emplace_back(fs::path(fontsDir).parent_path().parent_path() /
                                "turing-smart-screen-python" / "res" / "fonts");
    }
    const std::string root = resourceRoot();
    if (!root.empty()) {
        extraRoots.emplace_back(fs::path(root).parent_path() /
                                "turing-smart-screen-python" / "res" / "fonts");
    }
    for (const fs::path& extraRoot : extraRoots) {
        if (extraRoot.empty()) {
            continue;
        }
        const fs::path candidate = extraRoot / direct;
        if (fs::exists(candidate, ec)) {
            return candidate.string();
        }
    }
    return "";
}

Rect ThemeItem::bounds() const {
    switch (kind) {
        case Kind::Text:
            return Rect{text.x, text.y, text.width, text.height};
        case Kind::ProgressBar:
            return Rect{bar.x, bar.y, bar.width, bar.height};
        case Kind::LineGraph:
            return Rect{graph.x, graph.y, graph.width, graph.height};
        case Kind::Radial:
            return Rect{radial.x - radial.radius, radial.y - radial.radius, 2 * radial.radius,
                        2 * radial.radius};
    }
    return {};
}

Theme Theme::load(const std::string& directory, const std::string& fontsDir) {
    const fs::path path = fs::path(directory) / "theme.yaml";
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        throw std::runtime_error("theme.yaml not found in " + directory);
    }
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("cannot read " + path.string());
    }
    const std::string text{std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>()};
    return fromYaml(text, directory, fontsDir);
}

Theme Theme::fromYaml(const std::string& yamlText, const std::string& directory,
                      const std::string& fontsDir) {
    Theme theme;
    theme.rawYaml = yamlText;
    theme.directory = directory;
    theme.fontsRoot = fontsDir;
    theme.name = fs::path(directory).filename().string();

    YAML::Node root;
    try {
        root = YAML::Load(yamlText);
    } catch (const std::exception& error) {
        throw std::runtime_error("invalid theme YAML in " + directory + ": " + error.what());
    }
    if (!root || !root.IsMap()) {
        throw std::runtime_error("invalid theme YAML in " + directory + ": not a mapping");
    }

    theme.author = scalar(child(root, "author"));

    if (const YAML::Node display = child(root, "display"); display) {
        const std::string orientation = scalar(child(display, "DISPLAY_ORIENTATION"), "portrait");
        parseOrientation(orientation, theme.orientation);
        theme.displaySize = normalizeDisplaySize(scalar(child(display, "DISPLAY_SIZE")));
        if (const YAML::Node led = child(display, "DISPLAY_RGB_LED"); led) {
            theme.backplateLed = asColor(led, Color{0, 0, 0});
            theme.hasBackplateLed = true;
        }
    }

    if (const YAML::Node images = child(root, "static_images"); images && images.IsMap()) {
        for (auto it = images.begin(); it != images.end(); ++it) {
            const YAML::Node entry = it->second;
            ThemeImage image;
            image.name = scalar(it->first);
            const std::string rawPath = scalar(child(entry, "PATH"));
            image.path = rawPath.empty() ? "" : (fs::path(directory) / rawPath).string();
            image.x = asInt(child(entry, "X"), 0);
            image.y = asInt(child(entry, "Y"), 0);
            image.width = asInt(child(entry, "WIDTH"), 0);
            image.height = asInt(child(entry, "HEIGHT"), 0);
            std::error_code fileEc;
            if (image.path.empty() || !fs::exists(image.path, fileEc)) {
                theme.warnings.push_back("static_images/" + image.name +
                                         ": file not found: " + rawPath);
            }
            theme.images.push_back(std::move(image));
        }
    }

    if (const YAML::Node texts = child(root, "static_text"); texts && texts.IsMap()) {
        for (auto it = texts.begin(); it != texts.end(); ++it) {
            const YAML::Node entry = it->second;
            ThemeText text;
            text.name = scalar(it->first);
            text.text = scalar(child(entry, "TEXT"));
            text.x = asInt(child(entry, "X"), 0);
            text.y = asInt(child(entry, "Y"), 0);
            parseTextStyle(entry, theme, "static_text/" + text.name, text.style);
            // X/Y are duplicated on ThemeText for the editor; keep both in sync.
            text.style.x = text.x;
            text.style.y = text.y;
            theme.texts.push_back(std::move(text));
        }
    }

    if (const YAML::Node stats = child(root, "STATS"); stats) {
        parseStats(stats, theme);
    }

    return theme;
}

Theme Theme::loadByName(const std::string& name, const std::string& themesRoot,
                        const std::string& fontsDir) {
    return load((fs::path(themesRoot) / name).string(), fontsDir);
}

const ThemeItem* Theme::findItem(const std::string& key) const {
    for (const ThemeItem& item : items) {
        if (item.key == key) {
            return &item;
        }
    }
    return nullptr;
}

ThemeItem* Theme::findItem(const std::string& key) {
    for (ThemeItem& item : items) {
        if (item.key == key) {
            return &item;
        }
    }
    return nullptr;
}

Size Theme::declaredSize() const {
    for (const ThemeImage& image : images) {
        if (image.width > 0 && image.height > 0 && image.name == "BACKGROUND") {
            return {image.width, image.height};
        }
    }
    for (const ThemeImage& image : images) {
        if (image.width > 0 && image.height > 0) {
            return {image.width, image.height};
        }
    }
    return {};
}

std::vector<std::string> Theme::validate(int screenWidth, int screenHeight) const {
    std::vector<std::string> problems;
    if (screenWidth <= 0) {
        screenWidth = 1 << 20;
    }
    if (screenHeight <= 0) {
        screenHeight = 1 << 20;
    }

    for (const ThemeImage& image : images) {
        if (image.path.empty()) {
            problems.push_back("static_images/" + image.name + ": PATH is missing");
        }
    }

    for (const ThemeText& text : texts) {
        if (text.text.empty()) {
            problems.push_back("static_text/" + text.name + ": TEXT is empty");
        }
        if (text.x > screenWidth || text.y > screenHeight) {
            problems.push_back("static_text/" + text.name + ": outside the screen (" +
                               std::to_string(text.x) + "," + std::to_string(text.y) + ")");
        }
    }

    int shown = 0;
    for (const ThemeItem& item : items) {
        if (!item.show) {
            continue;
        }
        ++shown;
        const Rect rect = item.bounds();
        if (rect.x > screenWidth || rect.y > screenHeight) {
            problems.push_back(item.key + ": outside the screen (" + std::to_string(rect.x) + "," +
                               std::to_string(rect.y) + ")");
        }
        switch (item.kind) {
            case ThemeItem::Kind::Text:
                if (item.text.fontSize <= 0) {
                    problems.push_back(item.key + ": FONT_SIZE must be > 0");
                }
                if (item.text.anchor.size() < 2) {
                    problems.push_back(item.key + ": ANCHOR must be two characters (e.g. lt)");
                }
                break;
            case ThemeItem::Kind::ProgressBar:
                if (item.bar.width <= 0 || item.bar.height <= 0) {
                    problems.push_back(item.key + ": GRAPH needs WIDTH and HEIGHT > 0");
                }
                break;
            case ThemeItem::Kind::LineGraph:
                if (item.graph.width <= 0 || item.graph.height <= 0) {
                    problems.push_back(item.key + ": LINE_GRAPH needs WIDTH and HEIGHT > 0");
                }
                break;
            case ThemeItem::Kind::Radial:
                if (item.radial.radius <= 0 || item.radial.barWidth <= 0) {
                    problems.push_back(item.key + ": RADIAL needs RADIUS and WIDTH > 0");
                }
                break;
        }
    }
    if (shown == 0) {
        problems.push_back("no visible stats item: every SHOW flag is false or missing");
    }
    return problems;
}

std::vector<ThemeInfo> listThemes(const std::string& themesRoot) {
    std::vector<ThemeInfo> themes;
    std::error_code ec;
    if (!fs::is_directory(themesRoot, ec)) {
        return themes;
    }
    for (const auto& entry : fs::directory_iterator(themesRoot, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        const fs::path yaml = entry.path() / "theme.yaml";
        if (!fs::exists(yaml, ec)) {
            continue;
        }
        ThemeInfo info;
        info.name = entry.path().filename().string();
        info.directory = entry.path().string();
        try {
            YAML::Node root = YAML::LoadFile(yaml.string());
            info.author = scalar(child(root, "author"));
            if (const YAML::Node display = child(root, "display"); display) {
                info.displaySize = normalizeDisplaySize(scalar(child(display, "DISPLAY_SIZE")));
            }
        } catch (const std::exception&) {
            info.author = "(invalid YAML)";
        }
        std::error_code backgroundEc;
        info.hasBackground = fs::exists(entry.path() / "background.png", backgroundEc);
        themes.push_back(std::move(info));
    }
    std::sort(themes.begin(), themes.end(),
              [](const ThemeInfo& a, const ThemeInfo& b) { return a.name < b.name; });
    return themes;
}

std::string Theme::toYaml() const {
    YAML::Node root;
    if (!rawYaml.empty()) {
        try {
            root = YAML::Load(rawYaml);
        } catch (const std::exception&) {
            root = YAML::Node(YAML::NodeType::Map);
        }
    }
    if (!root || !root.IsMap()) {
        root = YAML::Node(YAML::NodeType::Map);
    }

    if (!author.empty()) {
        root["author"] = author;
    }

    for (const ThemeImage& image : images) {
        YAML::Node node = descend(root, "static_images/" + image.name);
        node["X"] = image.x;
        node["Y"] = image.y;
        if (image.width > 0) node["WIDTH"] = image.width;
        if (image.height > 0) node["HEIGHT"] = image.height;
    }

    for (const ThemeText& text : texts) {
        YAML::Node node = descend(root, "static_text/" + text.name);
        node["TEXT"] = text.text;
        patchTextNode(node, text.style);
    }

    for (const ThemeItem& item : items) {
        YAML::Node node = descend(root, item.key);
        node["SHOW"] = item.show;
        if (item.interval > 0) {
            node["INTERVAL"] = item.interval;
        }
        switch (item.kind) {
            case ThemeItem::Kind::Text:
                patchTextNode(node, item.text);
                node["SHOW_UNIT"] = item.showUnit;
                if (item.minSize > 0) node["MIN_SIZE"] = item.minSize;
                break;
            case ThemeItem::Kind::ProgressBar:
                node["X"] = item.bar.x;
                node["Y"] = item.bar.y;
                node["WIDTH"] = item.bar.width;
                node["HEIGHT"] = item.bar.height;
                node["MIN_VALUE"] = item.bar.minValue;
                node["MAX_VALUE"] = item.bar.maxValue;
                node["BAR_OUTLINE"] = item.bar.barOutline;
                break;
            case ThemeItem::Kind::LineGraph:
                node["X"] = item.graph.x;
                node["Y"] = item.graph.y;
                node["WIDTH"] = item.graph.width;
                node["HEIGHT"] = item.graph.height;
                node["MIN_VALUE"] = item.graph.minValue;
                node["MAX_VALUE"] = item.graph.maxValue;
                node["AUTOSCALE"] = item.graph.autoscale;
                node["AXIS"] = item.graph.graphAxis;
                break;
            case ThemeItem::Kind::Radial:
                node["X"] = item.radial.x;
                node["Y"] = item.radial.y;
                node["RADIUS"] = item.radial.radius;
                node["WIDTH"] = item.radial.barWidth;
                node["MIN_VALUE"] = item.radial.minValue;
                node["MAX_VALUE"] = item.radial.maxValue;
                node["SHOW_TEXT"] = item.radial.withText;
                break;
        }
    }

    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << root;
    return emitter.c_str();
}

}  // namespace lcdctl
