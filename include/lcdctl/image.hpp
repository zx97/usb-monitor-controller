// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - in-memory RGB/RGBA bitmap.
//
// Ported from https://github.com/mathoudebine/turing-smart-screen-python/
// (Pillow Image usage replaced by libpng/libjpeg decoding + simple transforms).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lcdctl {

// A simple interleaved image: `channels` is 3 (RGB) or 4 (RGBA).
class Image {
public:
    Image() = default;
    Image(int width, int height, int channels, std::uint8_t fill = 0);

    // Load a PNG/JPEG file (format decided from the file magic bytes).
    // Throws std::runtime_error on failure.
    static Image load(const std::string& path);

    int width() const { return width_; }
    int height() const { return height_; }
    int channels() const { return channels_; }
    bool empty() const { return data_.empty(); }

    const std::uint8_t* data() const { return data_.data(); }
    std::uint8_t* data() { return data_.data(); }
    std::size_t size() const { return data_.size(); }

    // Convert to 3 (RGB) or 4 (RGBA) channels, dropping/adding alpha (255).
    Image convert(int channels) const;

    Image crop(int x, int y, int width, int height) const;

    // Bilinear resize.
    Image resize(int width, int height) const;

    // Clockwise rotation, degrees must be 0/90/180/270.
    Image rotate(int degrees) const;

    // Paste `src` with its top-left corner at (x, y). Alpha is honoured when
    // `src` is RGBA; the destination must be RGB or RGBA.
    void paste(const Image& src, int x, int y);

private:
    int width_ = 0;
    int height_ = 0;
    int channels_ = 3;
    std::vector<std::uint8_t> data_;
};

}  // namespace lcdctl
