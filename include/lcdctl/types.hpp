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

}  // namespace lcdctl
