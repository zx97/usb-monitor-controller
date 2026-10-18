// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/serialize.hpp"

#include <algorithm>

namespace lcdctl {

std::vector<std::uint8_t> imageToRGB565(const Image& source, bool bigEndian) {
    const Image image = (source.channels() == 3 || source.channels() == 4)
                            ? source
                            : source.convert(3);
    const int channels = image.channels();
    const std::size_t pixels = static_cast<std::size_t>(image.width()) * image.height();
    const std::uint8_t* src = image.data();

    std::vector<std::uint8_t> out(pixels * 2);
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t r = src[i * channels + 0];
        const std::uint8_t g = src[i * channels + 1];
        const std::uint8_t b = src[i * channels + 2];
        const std::uint16_t rgb565 =
            static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        if (bigEndian) {
            out[i * 2 + 0] = static_cast<std::uint8_t>(rgb565 >> 8);
            out[i * 2 + 1] = static_cast<std::uint8_t>(rgb565 & 0xFF);
        } else {
            out[i * 2 + 0] = static_cast<std::uint8_t>(rgb565 & 0xFF);
            out[i * 2 + 1] = static_cast<std::uint8_t>(rgb565 >> 8);
        }
    }
    return out;
}

std::vector<std::uint8_t> imageToBGR(const Image& source) {
    const Image image = source.convert(3);
    const std::size_t pixels = static_cast<std::size_t>(image.width()) * image.height();
    const std::uint8_t* src = image.data();

    std::vector<std::uint8_t> out(pixels * 3);
    for (std::size_t i = 0; i < pixels; ++i) {
        out[i * 3 + 0] = src[i * 3 + 2];  // B
        out[i * 3 + 1] = src[i * 3 + 1];  // G
        out[i * 3 + 2] = src[i * 3 + 0];  // R
    }
    return out;
}

std::vector<std::uint8_t> imageToBGRA(const Image& source) {
    const Image image = source.convert(4);
    const std::size_t pixels = static_cast<std::size_t>(image.width()) * image.height();
    const std::uint8_t* src = image.data();

    std::vector<std::uint8_t> out(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
        out[i * 4 + 0] = src[i * 4 + 2];  // B
        out[i * 4 + 1] = src[i * 4 + 1];  // G
        out[i * 4 + 2] = src[i * 4 + 0];  // R
        out[i * 4 + 3] = src[i * 4 + 3];  // A
    }
    return out;
}

std::vector<std::uint8_t> imageToCompressedBGRA(const Image& source) {
    const Image image = source.convert(4);
    const std::size_t pixels = static_cast<std::size_t>(image.width()) * image.height();
    const std::uint8_t* src = image.data();

    std::vector<std::uint8_t> out(pixels * 3);
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t r = src[i * 4 + 0];
        const std::uint8_t g = src[i * 4 + 1];
        const std::uint8_t b = src[i * 4 + 2];
        const std::uint8_t a = static_cast<std::uint8_t>(src[i * 4 + 3] >> 4);
        out[i * 3 + 0] = static_cast<std::uint8_t>((b & 0xFC) | (a >> 2));
        out[i * 3 + 1] = static_cast<std::uint8_t>((g & 0xFC) | (a & 0x02));
        out[i * 3 + 2] = r;
    }
    return out;
}

std::vector<std::vector<std::uint8_t>> chunked(const std::vector<std::uint8_t>& data,
                                               std::size_t chunkSize) {
    std::vector<std::vector<std::uint8_t>> chunks;
    if (chunkSize == 0) {
        return chunks;
    }
    for (std::size_t offset = 0; offset < data.size(); offset += chunkSize) {
        const std::size_t end = std::min(offset + chunkSize, data.size());
        chunks.emplace_back(data.begin() + offset, data.begin() + end);
    }
    return chunks;
}

}  // namespace lcdctl
