// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - theme renderer.
//
// Turns a Theme + a Metrics snapshot into drawPrimitive() calls on a
// DrawSurface. The same renderer drives the panel (LcdComm) and the offscreen
// preview/editor (OffscreenCanvas), so what the editor shows is exactly what is
// sent over USB.

#pragma once

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "lcdctl/draw.hpp"
#include "lcdctl/sensors.hpp"
#include "lcdctl/theme.hpp"

namespace lcdctl {

// A DrawSurface backed by an in-memory framebuffer. Used for `--simulate`
// previews, the theme tests and the Qt editor.
class OffscreenCanvas : public DrawSurface {
public:
    explicit OffscreenCanvas(Size size, Color fill = Color{0, 0, 0});

    Size size() const override { return {canvas_.width(), canvas_.height()}; }
    const Image& cachedImage(const std::string& path) override;
    std::shared_ptr<Font> cachedFont(const std::string& path, int size) override;
    void displayPILImage(const Image& image, int x, int y) override;

    const Image& framebuffer() const { return canvas_; }
    Image& framebuffer() { return canvas_; }

    // Forget cached files (called when the theme directory changes).
    void clearCaches();

private:
    Image canvas_;
    std::unordered_map<std::string, Image> images_;
    std::unordered_map<std::string, std::shared_ptr<Font>> fonts_;
};

// One metric formatted for display.
struct MetricReading {
    bool available = false;
    double value = 0.0;     // numeric value used by bars / graphs
    std::string text;       // formatted text, without padding
    std::string unit;       // "", "%", "°C", " GHz", " M", " G", " FPS", " M/s"...
    int minSize = 0;        // right-align width from the theme (MIN_SIZE)
};

MetricReading readMetric(const std::string& metricId, const Metrics& metrics);

class ThemeRenderer {
public:
    ThemeRenderer(const Theme& theme, DrawSurface& surface);

    // Static images, then static texts (drawn once per theme change).
    void drawBackground();

    // Draw one themed item. Returns true when something was transmitted
    // (false when the rendered value did not change since the last call).
    bool drawItem(const ThemeItem& item, const Metrics& metrics);

    // Draw every visible item once. Returns the number of transmitted items.
    int drawAll(const Metrics& metrics);

    // Forget change-detection state and graph histories.
    void reset();

    int lastTransmittedCount() const { return lastTransmitted_; }

private:
    const Theme& theme_;
    DrawSurface& surface_;
    std::unordered_map<std::string, std::string> lastValue_;
    std::map<std::string, std::deque<float>> history_;
    int lastTransmitted_ = 0;
};

}  // namespace lcdctl
