// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - C++ controller for the small USB LCD screens
// supported by turing-smart-screen-python.
//
// Ported from https://github.com/mathoudebine/turing-smart-screen-python/
// Original Python code Copyright (C) 2021 Matthieu Houdebine (mathoudebine)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace lcdctl {

// Screen orientation. Numeric values match the Python project so that any
// protocol that transmits the raw value keeps working.
enum class Orientation {
    ReversePortrait = 1,
    Portrait = 0,
    Landscape = 2,
    ReverseLandscape = 3,
};

const char* orientationName(Orientation orientation);
bool parseOrientation(const std::string& text, Orientation& out);

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

// Accepts the same strings as the Python `parse_color()`:
//   "red", "#ff0000", "255, 0, 0", "hsl(0,100%,50%)"...
// Throws std::invalid_argument on a value it cannot parse.
Color parseColor(const std::string& text);

struct Size {
    int width = 0;
    int height = 0;
};

// Axis-aligned rectangle; width/height are always >= 0 when produced by the
// drawing primitives.
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    int right() const { return x + width; }
    int bottom() const { return y + height; }
    bool empty() const { return width <= 0 || height <= 0; }

    static Rect fromCorners(int left, int top, int right, int bottom) {
        return {left, top, right - left, bottom - top};
    }

    Rect united(const Rect& other) const {
        if (empty()) return other;
        if (other.empty()) return *this;
        const int l = std::min(x, other.x);
        const int t = std::min(y, other.y);
        const int r = std::max(right(), other.right());
        const int b = std::max(bottom(), other.bottom());
        return fromCorners(l, t, r, b);
    }

    Rect clipped(int maxWidth, int maxHeight) const {
        const int l = std::max(0, x);
        const int t = std::max(0, y);
        const int r = std::min(maxWidth, right());
        const int b = std::min(maxHeight, bottom());
        return fromCorners(l, t, r, b);
    }
};

}  // namespace lcdctl
