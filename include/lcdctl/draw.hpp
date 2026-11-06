// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - drawing primitives shared by the on-screen drivers
// and the offscreen (theme preview / editor) renderer.
//
// Ported from library/lcd/lcd_comm.py of
// https://github.com/mathoudebine/turing-smart-screen-python/
//
// The primitives build a small Image and hand it to a DrawSurface. LcdComm is
// the on-screen surface (it forwards the crop over USB), OffscreenCanvas is the
// preview surface (it pastes into a full framebuffer). Exactly one
// implementation of each primitive => what the editor shows is what the panel
// receives.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "lcdctl/font.hpp"
#include "lcdctl/image.hpp"
#include "lcdctl/types.hpp"

namespace lcdctl {

// Where drawing primitives deliver their pixels.
class DrawSurface {
public:
    virtual ~DrawSurface() = default;

    // Portrait-oriented surface size (after the driver's orientation swap).
    virtual Size size() const = 0;

    // File loading is cached by the surface: the drivers cache on the device
    // instance, the offscreen canvas caches for the lifetime of a render pass.
    virtual const Image& cachedImage(const std::string& path) = 0;
    virtual std::shared_ptr<Font> cachedFont(const std::string& path, int size) = 0;

    // Deliver `image` with its top-left corner at (x, y).
    virtual void displayPILImage(const Image& image, int x, int y) = 0;

    int width() const { return size().width; }
    int height() const { return size().height; }

    // Bounding box of the last text drawn at a given origin. The Python project
    // unions the new box with the previous one so a shorter value repaints the
    // stale glyph fragments of the older one (no ghosting).
    const std::unordered_map<std::uint64_t, Rect>& textBBoxes() const { return textBBoxCache_; }
    std::unordered_map<std::uint64_t, Rect>& textBBoxes() { return textBBoxCache_; }

    static std::uint64_t bboxKey(int x, int y) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
               static_cast<std::uint32_t>(y);
    }

private:
    std::unordered_map<std::uint64_t, Rect> textBBoxCache_;
};

// Options for drawText(). Defaults mirror the Python project.
struct TextStyle {
    int x = 0;                     // anchor position (see `anchor`)
    int y = 0;
    int width = 0;                 // 0 => shrink to the text
    int height = 0;                // 0 => derived from the font size / text
    std::string font;              // empty => bundled Roboto Mono
    int fontSize = 20;
    Color fontColor{0, 0, 0};
    Color backgroundColor{255, 255, 255};
    std::string backgroundImage;   // empty => solid backgroundColor
    std::string align = "left";    // "left" | "center" | "right"
    // Pillow-style two-letter anchor: l|m|r + t|a|m|s|b|d.
    // "la" (left ascender) is the Pillow default and reproduces the historical
    // behaviour of this library; themes mostly use "lt"/"lm"/"mm".
    std::string anchor = "la";
};

struct ProgressBarStyle {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int value = 0;
    int minValue = 0;
    int maxValue = 100;
    Color barColor{0, 0, 0};
    bool barOutline = true;
    Color backgroundColor{255, 255, 255};
    std::string backgroundImage;
    bool reverseDirection = false;
};

struct LineGraphStyle {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    float minValue = 0;
    float maxValue = 100;
    bool autoscale = false;
    Color lineColor{0, 0, 0};
    int lineWidth = 2;
    bool graphAxis = true;
    Color axisColor{0, 0, 0};
    Color backgroundColor{255, 255, 255};
    std::string backgroundImage;
};

struct RadialStyle {
    int x = 0;                     // centre
    int y = 0;
    int radius = 1;
    int barWidth = 1;
    int value = 0;
    int minValue = 0;
    int maxValue = 100;
    double angleStart = 0;
    double angleEnd = 360;
    int angleSep = 5;
    int angleSteps = 10;
    bool clockwise = true;
    bool withText = true;
    std::string text;              // empty => "NN%"
    std::string font;
    int fontSize = 20;
    Color fontColor{0, 0, 0};
    Color barColor{0, 0, 0};
    Color backgroundColor{255, 255, 255};
    std::string backgroundImage;
    Color barBackgroundColor{0, 0, 0};
    bool drawBarBackground = false;
};

// --------------------------------------------------------------------------
// Primitives
// --------------------------------------------------------------------------

// Draw `text` anchored at (x, y). Returns the rectangle that was transmitted.
// When `style.width`/`height` are both set the text is laid out inside that box
// and the whole box is repainted; otherwise only the ink is repainted (unioned
// with the previous box drawn at the same origin, see DrawSurface::textBBoxes).
Rect drawText(DrawSurface& surface, const std::string& text, int x, int y,
              const TextStyle& style);

Rect drawProgressBar(DrawSurface& surface, int x, int y, int width, int height,
                     int value, const ProgressBarStyle& style);

Rect drawLineGraph(DrawSurface& surface, int x, int y, int width, int height,
                   const std::vector<float>& values, const LineGraphStyle& style);

Rect drawRadialProgressBar(DrawSurface& surface, int xc, int yc, int radius,
                           int barWidth, int value, const RadialStyle& style);

// Style-driven overloads: geometry (and the value for bars/radials) is read
// from the style structure. Used by the theme renderer and the editor.
inline Rect drawText(DrawSurface& surface, const std::string& text, const TextStyle& style) {
    return drawText(surface, text, style.x, style.y, style);
}
inline Rect drawProgressBar(DrawSurface& surface, const ProgressBarStyle& style) {
    return drawProgressBar(surface, style.x, style.y, style.width, style.height, style.value,
                           style);
}
inline Rect drawLineGraph(DrawSurface& surface, const std::vector<float>& values,
                          const LineGraphStyle& style) {
    return drawLineGraph(surface, style.x, style.y, style.width, style.height, values, style);
}
inline Rect drawRadialProgressBar(DrawSurface& surface, const RadialStyle& style) {
    return drawRadialProgressBar(surface, style.x, style.y, style.radius, style.barWidth,
                                 style.value, style);
}

}  // namespace lcdctl
