// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - pixel format serialization.
//
// Ported from library/lcd/serialize.py of
// https://github.com/mathoudebine/turing-smart-screen-python/

#pragma once

#include <cstdint>
#include <vector>

#include "lcdctl/image.hpp"

namespace lcdctl {

// RGB565, 2 bytes per pixel. When `bigEndian` is false the 16-bit value is
// written little-endian (rev A, WeAct), otherwise big-endian (rev B, D).
std::vector<std::uint8_t> imageToRGB565(const Image& image, bool bigEndian);

// BGR, 3 bytes per pixel.
std::vector<std::uint8_t> imageToBGR(const Image& image);

// BGRA, 4 bytes per pixel.
std::vector<std::uint8_t> imageToBGRA(const Image& image);

// Rev C 3-byte variant: [6-bit B + 2-bit A, 6-bit G + 2-bit A, 8-bit R].
std::vector<std::uint8_t> imageToCompressedBGRA(const Image& image);

// Split `data` into consecutive chunks of at most `chunkSize` bytes.
std::vector<std::vector<std::uint8_t>> chunked(const std::vector<std::uint8_t>& data,
                                               std::size_t chunkSize);

}  // namespace lcdctl
