// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/types.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace lcdctl {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string trim(const std::string& value) {
    std::size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::invalid_argument("invalid hex digit");
}

const std::map<std::string, Color>& namedColors() {
    // Subset of the CSS/Pillow color names, with Pillow's exact values so that
    // themes and CLI arguments behave identically.
    static const std::map<std::string, Color> colors = {
        {"black", {0, 0, 0}},         {"white", {255, 255, 255}},
        {"red", {255, 0, 0}},         {"green", {0, 128, 0}},
        {"lime", {0, 255, 0}},        {"blue", {0, 0, 255}},
        {"yellow", {255, 255, 0}},    {"cyan", {0, 255, 255}},
        {"aqua", {0, 255, 255}},      {"magenta", {255, 0, 255}},
        {"fuchsia", {255, 0, 255}},   {"gray", {128, 128, 128}},
        {"grey", {128, 128, 128}},    {"silver", {192, 192, 192}},
        {"maroon", {128, 0, 0}},      {"olive", {128, 128, 0}},
        {"navy", {0, 0, 128}},        {"purple", {128, 0, 128}},
        {"teal", {0, 128, 128}},      {"orange", {255, 165, 0}},
        {"pink", {255, 192, 203}},    {"brown", {165, 42, 42}},
        {"gold", {255, 215, 0}},      {"violet", {238, 130, 238}},
        {"indigo", {75, 0, 130}},     {"turquoise", {64, 224, 208}},
        {"silver", {192, 192, 192}},  {"darkgreen", {0, 100, 0}},
        {"darkred", {139, 0, 0}},     {"darkblue", {0, 0, 139}},
    };
    return colors;
}

}  // namespace

const char* orientationName(Orientation orientation) {
    switch (orientation) {
        case Orientation::Portrait:        return "portrait";
        case Orientation::Landscape:       return "landscape";
        case Orientation::ReversePortrait: return "reverse-portrait";
        case Orientation::ReverseLandscape:return "reverse-landscape";
    }
    return "unknown";
}

bool parseOrientation(const std::string& text, Orientation& out) {
    const std::string value = lower(trim(text));
    if (value == "portrait" || value == "0") {
        out = Orientation::Portrait;
    } else if (value == "landscape" || value == "2") {
        out = Orientation::Landscape;
    } else if (value == "reverse-portrait" || value == "reverse_portrait" || value == "1") {
        out = Orientation::ReversePortrait;
    } else if (value == "reverse-landscape" || value == "reverse_landscape" || value == "3") {
        out = Orientation::ReverseLandscape;
    } else {
        return false;
    }
    return true;
}

Color parseColor(const std::string& text) {
    const std::string value = trim(text);

    // "r, g, b"
    {
        std::vector<std::string> parts;
        std::stringstream stream(value);
        std::string part;
        while (std::getline(stream, part, ',')) {
            parts.push_back(trim(part));
        }
        if (parts.size() == 3) {
            try {
                int r = std::stoi(parts[0]);
                int g = std::stoi(parts[1]);
                int b = std::stoi(parts[2]);
                return Color{static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                             static_cast<std::uint8_t>(b)};
            } catch (const std::exception&) {
                // fall through to the other formats
            }
        }
    }

    // "#rrggbb" / "#rgb"
    if (!value.empty() && value[0] == '#') {
        const std::string hex = value.substr(1);
        if (hex.size() == 6) {
            return Color{static_cast<std::uint8_t>(hexNibble(hex[0]) * 16 + hexNibble(hex[1])),
                         static_cast<std::uint8_t>(hexNibble(hex[2]) * 16 + hexNibble(hex[3])),
                         static_cast<std::uint8_t>(hexNibble(hex[4]) * 16 + hexNibble(hex[5]))};
        }
        if (hex.size() == 3) {
            return Color{static_cast<std::uint8_t>(hexNibble(hex[0]) * 17),
                         static_cast<std::uint8_t>(hexNibble(hex[1]) * 17),
                         static_cast<std::uint8_t>(hexNibble(hex[2]) * 17)};
        }
        throw std::invalid_argument("invalid hex color: " + text);
    }

    // Named color
    const auto& colors = namedColors();
    auto it = colors.find(lower(value));
    if (it != colors.end()) {
        return it->second;
    }

    throw std::invalid_argument("unknown color: " + text);
}

}  // namespace lcdctl
