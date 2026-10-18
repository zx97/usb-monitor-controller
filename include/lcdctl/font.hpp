// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - TrueType text rendering (FreeType).
//
// Replaces the Pillow ImageFont usage of the Python project.

#pragma once

#include <memory>
#include <string>

#include "lcdctl/image.hpp"
#include "lcdctl/types.hpp"

namespace lcdctl {

struct TextMetrics {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

class Font {
public:
    // Throws std::runtime_error when the font file cannot be loaded.
    Font(const std::string& path, int pixelSize);
    ~Font();

    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    int pixelSize() const { return pixelSize_; }

    // Bounding box of `text` relative to the drawing origin (baseline anchor
    // is not exposed: the drawing helper positions the top-left corner).
    TextMetrics measure(const std::string& text) const;

    // Draw `text` on `dst` with its top-left corner at (x, y).
    // Only RGB and RGBA destinations are supported.
    void draw(Image& dst, int x, int y, const std::string& text, Color color) const;

    int lineHeight() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int pixelSize_ = 0;
};

}  // namespace lcdctl
