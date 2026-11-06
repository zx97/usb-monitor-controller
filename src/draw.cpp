// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/draw.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

#include "lcdctl/logging.hpp"
#include "lcdctl/paths.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace lcdctl {
namespace {

void fillRect(Image& image, int x, int y, int width, int height, Color color) {
    const int endX = std::min(image.width(), x + width);
    const int endY = std::min(image.height(), y + height);
    for (int py = std::max(0, y); py < endY; ++py) {
        std::uint8_t* row = image.data() + static_cast<std::size_t>(py) * image.width() * image.channels();
        for (int px = std::max(0, x); px < endX; ++px) {
            std::uint8_t* pixel = row + static_cast<std::size_t>(px) * image.channels();
            pixel[0] = color.r;
            pixel[1] = color.g;
            pixel[2] = color.b;
            if (image.channels() == 4) {
                pixel[3] = 255;
            }
        }
    }
}

void drawLine(Image& image, int x0, int y0, int x1, int y1, Color color, int thickness) {
    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    const int half = std::max(0, thickness / 2);
    for (;;) {
        fillRect(image, x0 - half, y0 - half, thickness, thickness, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * error;
        if (e2 >= dy) {
            error += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void drawDisc(Image& image, int cx, int cy, int radius, Color color) {
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) {
                fillRect(image, cx + x, cy + y, 1, 1, color);
            }
        }
    }
}

void drawThickArc(Image& image, int cx, int cy, double radius, double startDeg, double endDeg,
                  int thickness, Color color) {
    double sweep = endDeg - startDeg;
    while (sweep < 0) {
        sweep += 360;
    }
    while (sweep >= 360) {
        sweep -= 360;
    }
    const double step = 0.5;
    const int discRadius = std::max(1, thickness / 2);
    for (double angle = 0; angle <= sweep; angle += step) {
        const double radians = (startDeg + angle) * M_PI / 180.0;
        const int x = static_cast<int>(std::lround(cx + radius * std::cos(radians)));
        const int y = static_cast<int>(std::lround(cy + radius * std::sin(radians)));
        drawDisc(image, x, y, discRadius, color);
    }
}

std::string resolveFont(const std::string& font) {
    return font.empty() ? defaultFontPath() : font;
}

// Build the canvas the primitive draws onto: either the matching crop of the
// theme background image, or a solid rectangle. Returns an image of `rect`
// size, so full-screen backgrounds are never copied for a small widget.
Image makeCanvas(DrawSurface& surface, const Rect& rect, const std::string& backgroundImage,
                 Color backgroundColor) {
    if (!backgroundImage.empty()) {
        Image canvas = surface.cachedImage(backgroundImage).crop(rect.x, rect.y, rect.width, rect.height);
        if (canvas.channels() != 3 && canvas.channels() != 4) {
            canvas = canvas.convert(3);
        }
        return canvas;
    }
    Image canvas(rect.width, rect.height, 3);
    fillRect(canvas, 0, 0, rect.width, rect.height, backgroundColor);
    return canvas;
}

}  // namespace

Rect drawText(DrawSurface& surface, const std::string& text, int x, int y,
              const TextStyle& style) {
    if (text.empty()) {
        return {};
    }
    if (style.fontSize <= 0) {
        throw std::invalid_argument("font size must be > 0");
    }

    const std::shared_ptr<Font> font = surface.cachedFont(resolveFont(style.font), style.fontSize);
    const TextMetrics ink = font->inkBox(text);
    const int advance = font->measure(text).right;
    if (ink.right <= ink.left && ink.bottom <= ink.top) {
        return {};  // nothing to paint (spaces only)
    }

    const char horizontal = style.anchor.empty() ? 'l' : style.anchor[0];
    const char vertical = style.anchor.size() < 2 ? 'a' : style.anchor[1];

    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    int penX = 0;
    int baseline = 0;

    const bool boxed = style.width > 0 && style.height > 0;
    if (boxed) {
        left = x;
        top = y;
        right = x + style.width;
        bottom = y + style.height;

        const int anchorX = (horizontal == 'm')   ? left + style.width / 2
                            : (horizontal == 'r') ? right
                                                  : left;
        const int anchorY = (vertical == 'm')            ? top + style.height / 2
                            : (vertical == 'b' || vertical == 'd') ? bottom
                                                                  : top;

        penX = (horizontal == 'm')   ? anchorX - advance / 2
               : (horizontal == 'r') ? anchorX - advance
                                     : anchorX;
        switch (vertical) {
            case 't':
                baseline = anchorY - ink.top;
                break;
            case 'a':
                baseline = anchorY + font->ascent();
                break;
            case 'm':
                baseline = anchorY + (font->ascent() - font->descent()) / 2;
                break;
            case 's':
                baseline = anchorY;
                break;
            case 'b':
            case 'd':
            default:
                baseline = anchorY - font->descent();
                break;
        }
    } else {
        // Pillow semantics: no box => the anchor point is inside the text.
        penX = (horizontal == 'm')   ? x - advance / 2
               : (horizontal == 'r') ? x - advance
                                     : x;
        switch (vertical) {
            case 't':
                baseline = y - ink.top;
                break;
            case 'a':
                baseline = y + font->ascent();
                break;
            case 'm':
                baseline = y + (font->ascent() - font->descent()) / 2;
                break;
            case 's':
                baseline = y;
                break;
            case 'b':
            case 'd':
            default:
                baseline = y - font->descent();
                break;
        }
        left = penX + ink.left;
        right = penX + ink.right;
        top = baseline + ink.top;
        bottom = baseline + ink.bottom;

        // Union with the previous box drawn at the same origin so a shorter
        // value repaints the stale glyphs of the previous one (no ghosting).
        const std::uint64_t key = DrawSurface::bboxKey(x, y);
        auto& cache = surface.textBBoxes();
        const Rect current = Rect::fromCorners(left, top, right, bottom);
        auto it = cache.find(key);
        if (it != cache.end()) {
            const Rect merged = it->second.united(current);
            left = merged.x;
            top = merged.y;
            right = merged.right();
            bottom = merged.bottom();
        }
        cache[key] = current;
    }

    const Rect clipped = Rect::fromCorners(left, top, right, bottom)
                             .clipped(surface.width(), surface.height());
    if (clipped.empty()) {
        return {};
    }

    Image canvas = makeCanvas(surface, clipped, style.backgroundImage, style.backgroundColor);
    font->drawAtBaseline(canvas, penX - clipped.x, baseline - clipped.y, text, style.fontColor);
    surface.displayPILImage(canvas, clipped.x, clipped.y);
    return clipped;
}

Rect drawProgressBar(DrawSurface& surface, int x, int y, int width, int height, int value,
                     const ProgressBarStyle& style) {
    if (style.minValue >= style.maxValue) {
        throw std::invalid_argument("progress bar requires min < max");
    }
    if (width <= 0 || height <= 0) {
        return {};
    }
    value = std::max(style.minValue, std::min(value, style.maxValue));

    const Rect rect = Rect::fromCorners(x, y, x + width, y + height)
                          .clipped(surface.width(), surface.height());
    if (rect.empty()) {
        return {};
    }

    Image bar = makeCanvas(surface, rect, style.backgroundImage, style.backgroundColor);

    const double fraction =
        static_cast<double>(value - style.minValue) / (style.maxValue - style.minValue);
    if (width > height) {
        int filled = static_cast<int>(fraction * width) - 1;
        filled = std::max(0, filled);
        const int x0 = style.reverseDirection ? (width - 1 - filled) : 0;
        const int x1 = style.reverseDirection ? (width - 1) : filled;
        fillRect(bar, x0, 0, x1 - x0 + 1, height, style.barColor);
    } else {
        int filled = static_cast<int>(fraction * height) - 1;
        filled = std::max(0, filled);
        const int y0 = style.reverseDirection ? 0 : (height - 1 - filled);
        const int y1 = style.reverseDirection ? filled : (height - 1);
        fillRect(bar, 0, y0, width, y1 - y0 + 1, style.barColor);
    }

    if (style.barOutline) {
        fillRect(bar, 0, 0, width, 1, style.barColor);
        fillRect(bar, 0, height - 1, width, 1, style.barColor);
        fillRect(bar, 0, 0, 1, height, style.barColor);
        fillRect(bar, width - 1, 0, 1, height, style.barColor);
    }

    surface.displayPILImage(bar, rect.x, rect.y);
    return rect;
}

Rect drawLineGraph(DrawSurface& surface, int x, int y, int width, int height,
                   const std::vector<float>& values, const LineGraphStyle& style) {
    if (values.empty() || width <= 0 || height <= 0) {
        return {};
    }

    const Rect rect = Rect::fromCorners(x, y, x + width, y + height)
                          .clipped(surface.width(), surface.height());
    if (rect.empty()) {
        return {};
    }

    Image graph = makeCanvas(surface, rect, style.backgroundImage, style.backgroundColor);

    float minValue = style.minValue;
    float maxValue = style.maxValue;
    if (style.autoscale) {
        float trueMin = maxValue;
        float trueMax = minValue;
        bool any = false;
        for (float v : values) {
            if (std::isnan(v)) continue;
            trueMin = std::min(trueMin, v);
            trueMax = std::max(trueMax, v);
            any = true;
        }
        if (any && trueMin != maxValue && trueMax != minValue) {
            minValue = std::max(trueMin - 5, minValue);
            maxValue = std::min(trueMax + 5, maxValue);
        }
    }
    if (maxValue == minValue) {
        maxValue = minValue + 1;
    }

    const double step = static_cast<double>(width) / values.size();
    const double yScale = height / static_cast<double>(maxValue - minValue);

    int previousX = 0;
    int previousY = 0;
    bool hasPrevious = false;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (std::isnan(values[i])) {
            hasPrevious = false;
            continue;
        }
        const float clamped = std::min(maxValue, std::max(minValue, values[i]));
        const int px = static_cast<int>(i * step);
        const int py = static_cast<int>(height - (clamped - minValue) * yScale);
        if (hasPrevious) {
            drawLine(graph, previousX, previousY, px, py, style.lineColor, style.lineWidth);
        }
        previousX = px;
        previousY = py;
        hasPrevious = true;
    }

    if (style.graphAxis) {
        drawLine(graph, 0, height - 1, width - 1, height - 1, style.axisColor, 1);
        drawLine(graph, 0, 0, 0, height - 1, style.axisColor, 1);
    }

    surface.displayPILImage(graph, rect.x, rect.y);
    return rect;
}

Rect drawRadialProgressBar(DrawSurface& surface, int xc, int yc, int radius, int barWidth,
                           int value, const RadialStyle& style) {
    if (barWidth <= 0 || barWidth > radius) {
        throw std::invalid_argument("radial bar width must be within [1, radius]");
    }
    value = std::max(style.minValue, std::min(value, style.maxValue));

    const int diameter = 2 * radius;
    const Rect rect = Rect::fromCorners(xc - radius, yc - radius, xc + radius, yc + radius)
                          .clipped(surface.width(), surface.height());
    if (rect.empty()) {
        return {};
    }

    Image bar = makeCanvas(surface, rect, style.backgroundImage, style.backgroundColor);

    double start = style.angleStart;
    double end = style.angleEnd;
    double sweep = end - start;
    while (sweep < 0) sweep += 360;
    while (sweep >= 360) sweep -= 360;
    if (sweep == 0) {
        sweep = 360;
    }

    const double fraction =
        static_cast<double>(value - style.minValue) / (style.maxValue - style.minValue);

    // Radius is relative to the (possibly clipped) canvas.
    const double cx = radius;
    const double cy = radius;

    if (style.drawBarBackground) {
        drawThickArc(bar, static_cast<int>(cx), static_cast<int>(cy), radius - barWidth / 2.0, start,
                     start + sweep, barWidth, style.barBackgroundColor);
    }
    drawThickArc(bar, static_cast<int>(cx), static_cast<int>(cy), radius - barWidth / 2.0, start,
                 start + fraction * sweep, barWidth, style.barColor);

    if (style.withText) {
        std::string text = style.text;
        if (text.empty()) {
            text = std::to_string(static_cast<int>(fraction * 100 + 0.5)) + "%";
        }
        const std::shared_ptr<Font> font =
            surface.cachedFont(resolveFont(style.font), style.fontSize);
        const TextMetrics ink = font->inkBox(text);
        const int advance = font->measure(text).right;
        const int penX = static_cast<int>(cx) - advance / 2;
        const int baseline =
            static_cast<int>(cy) - (ink.top + ink.bottom) / 2;
        font->drawAtBaseline(bar, penX, baseline, text, style.fontColor);
    }

    surface.displayPILImage(bar, rect.x, rect.y);
    return rect;
}

}  // namespace lcdctl
