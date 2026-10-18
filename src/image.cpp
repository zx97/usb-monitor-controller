// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/image.hpp"

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include <png.h>

extern "C" {
#include <jpeglib.h>
}

namespace lcdctl {
namespace {

bool startsWith(const std::vector<std::uint8_t>& bytes, const std::uint8_t* magic,
                std::size_t length) {
    return bytes.size() >= length && std::memcmp(bytes.data(), magic, length) == 0;
}

std::vector<std::uint8_t> readWholeFile(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        throw std::runtime_error("cannot open image file: " + path);
    }
    std::vector<std::uint8_t> data;
    std::uint8_t buffer[65536];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        data.insert(data.end(), buffer, buffer + read);
    }
    std::fclose(file);
    return data;
}

// ---- JPEG error handling (libjpeg uses longjmp) ----
struct JpegErrorManager {
    jpeg_error_mgr base;
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX] = {0};
};

void jpegErrorExit(j_common_ptr info) {
    auto* manager = reinterpret_cast<JpegErrorManager*>(info->err);
    (*info->err->format_message)(info, manager->message);
    std::longjmp(manager->jump, 1);
}

Image loadJpeg(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        throw std::runtime_error("cannot open JPEG file: " + path);
    }

    jpeg_decompress_struct info{};
    JpegErrorManager manager{};
    info.err = jpeg_std_error(&manager.base);
    manager.base.error_exit = jpegErrorExit;

    if (setjmp(manager.jump)) {
        jpeg_destroy_decompress(&info);
        std::fclose(file);
        throw std::runtime_error(std::string("JPEG decode error: ") + manager.message);
    }

    jpeg_create_decompress(&info);
    jpeg_stdio_src(&info, file);
    jpeg_read_header(&info, TRUE);
    info.out_color_space = JCS_RGB;
    jpeg_start_decompress(&info);

    const int width = static_cast<int>(info.output_width);
    const int height = static_cast<int>(info.output_height);
    Image image(width, height, 4, 255);

    const int rowStride = width * info.output_components;
    std::vector<std::uint8_t> row(static_cast<std::size_t>(rowStride));
    while (info.output_scanline < info.output_height) {
        JSAMPROW rows[1] = {row.data()};
        const JDIMENSION line = info.output_scanline;
        jpeg_read_scanlines(&info, rows, 1);
        std::uint8_t* dst = image.data() + static_cast<std::size_t>(line) * width * 4;
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = row[x * 3 + 0];
            dst[x * 4 + 1] = row[x * 3 + 1];
            dst[x * 4 + 2] = row[x * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
    }

    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    std::fclose(file);
    return image;
}

}  // namespace

void savePng(const std::string& path, const Image& image) {
    const Image rgba = image.convert(4);
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        throw std::runtime_error("cannot open PNG file for writing: " + path);
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    if (!png || !info) {
        std::fclose(file);
        throw std::runtime_error("cannot create PNG write struct");
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(file);
        throw std::runtime_error("PNG encode error");
    }
    png_init_io(png, file);
    png_set_IHDR(png, info, rgba.width(), rgba.height(), 8, PNG_COLOR_TYPE_RGB_ALPHA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    std::vector<png_bytep> rows(rgba.height());
    for (int y = 0; y < rgba.height(); ++y) {
        rows[y] = const_cast<png_bytep>(rgba.data() + static_cast<std::size_t>(y) * rgba.width() * 4);
    }
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(file);
}

Image::Image(int width, int height, int channels, std::uint8_t fill)
    : width_(width), height_(height), channels_(channels) {
    if (width <= 0 || height <= 0 || (channels != 3 && channels != 4)) {
        throw std::invalid_argument("invalid image dimensions/channels");
    }
    data_.assign(static_cast<std::size_t>(width) * height * channels, fill);
}

Image Image::load(const std::string& path) {
    const std::vector<std::uint8_t> bytes = readWholeFile(path);

    static const std::uint8_t pngMagic[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    static const std::uint8_t jpegMagic[3] = {0xFF, 0xD8, 0xFF};

    if (startsWith(bytes, pngMagic, 8)) {
        png_image png{};
        png.version = PNG_IMAGE_VERSION;
        if (!png_image_begin_read_from_memory(&png, bytes.data(), bytes.size())) {
            throw std::runtime_error("PNG read error: " + path);
        }
        png.format = PNG_FORMAT_RGBA;
        Image image(static_cast<int>(png.width), static_cast<int>(png.height), 4);  // NOLINT
        if (!png_image_finish_read(&png, nullptr, image.data(), 0, nullptr)) {
            png_image_free(&png);
            throw std::runtime_error("PNG decode error: " + path);
        }
        png_image_free(&png);
        return image;
    }

    if (startsWith(bytes, jpegMagic, 3)) {
        return loadJpeg(path);
    }

    throw std::runtime_error("unsupported image format (expected PNG or JPEG): " + path);
}

Image Image::convert(int channels) const {
    if (channels != 3 && channels != 4) {
        throw std::invalid_argument("convert() channels must be 3 or 4");
    }
    if (channels == channels_) {
        return *this;
    }

    Image out(width_, height_, channels, 255);
    const std::size_t pixels = static_cast<std::size_t>(width_) * height_;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t r = data_[i * channels_ + 0];
        const std::uint8_t g = data_[i * channels_ + 1];
        const std::uint8_t b = data_[i * channels_ + 2];
        const std::uint8_t a = (channels_ == 4) ? data_[i * channels_ + 3] : 255;
        out.data_[i * channels + 0] = r;
        out.data_[i * channels + 1] = g;
        out.data_[i * channels + 2] = b;
        if (channels == 4) {
            out.data_[i * channels + 3] = a;
        }
    }
    return out;
}

Image Image::crop(int x, int y, int width, int height) const {
    // Clamp to the source bounds; never produce an empty image.
    x = std::max(0, std::min(x, width_ - 1));
    y = std::max(0, std::min(y, height_ - 1));
    width = std::max(1, std::min(width, width_ - x));
    height = std::max(1, std::min(height, height_ - y));

    Image out(width, height, channels_);
    const std::size_t rowBytes = static_cast<std::size_t>(width) * channels_;
    for (int row = 0; row < height; ++row) {
        const std::uint8_t* src = data_.data() + (static_cast<std::size_t>(y + row) * width_ + x) * channels_;
        std::memcpy(out.data_.data() + static_cast<std::size_t>(row) * rowBytes, src, rowBytes);
    }
    return out;
}

Image Image::resize(int width, int height) const {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("resize() dimensions must be positive");
    }
    if (width == width_ && height == height_) {
        return *this;
    }

    Image out(width, height, channels_);
    const double xRatio = static_cast<double>(width_) / width;
    const double yRatio = static_cast<double>(height_) / height;
    for (int y = 0; y < height; ++y) {
        const double srcY = (y + 0.5) * yRatio - 0.5;
        int y0 = static_cast<int>(std::floor(srcY));
        const double fy = srcY - y0;
        int y1 = std::min(y0 + 1, height_ - 1);
        y0 = std::max(0, std::min(y0, height_ - 1));
        for (int x = 0; x < width; ++x) {
            const double srcX = (x + 0.5) * xRatio - 0.5;
            int x0 = static_cast<int>(std::floor(srcX));
            const double fx = srcX - x0;
            int x1 = std::min(x0 + 1, width_ - 1);
            x0 = std::max(0, std::min(x0, width_ - 1));
            for (int c = 0; c < channels_; ++c) {
                const double top = data_[(static_cast<std::size_t>(y0) * width_ + x0) * channels_ + c] * (1 - fx) +
                                   data_[(static_cast<std::size_t>(y0) * width_ + x1) * channels_ + c] * fx;
                const double bottom = data_[(static_cast<std::size_t>(y1) * width_ + x0) * channels_ + c] * (1 - fx) +
                                      data_[(static_cast<std::size_t>(y1) * width_ + x1) * channels_ + c] * fx;
                out.data_[(static_cast<std::size_t>(y) * width + x) * channels_ + c] =
                    static_cast<std::uint8_t>(std::lround(top * (1 - fy) + bottom * fy));
            }
        }
    }
    return out;
}

Image Image::rotate(int degrees) const {
    int normalized = ((degrees % 360) + 360) % 360;
    if (normalized == 0) {
        return *this;
    }
    if (normalized != 90 && normalized != 180 && normalized != 270) {
        throw std::invalid_argument("rotate() supports 0/90/180/270 only");
    }

    const bool swap = (normalized == 90 || normalized == 270);
    const int outWidth = swap ? height_ : width_;
    const int outHeight = swap ? width_ : height_;
    Image out(outWidth, outHeight, channels_);

    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            int nx = 0;
            int ny = 0;
            if (normalized == 90) {         // clockwise
                nx = height_ - 1 - y;
                ny = x;
            } else if (normalized == 180) {
                nx = width_ - 1 - x;
                ny = height_ - 1 - y;
            } else {                        // 270 clockwise
                nx = y;
                ny = width_ - 1 - x;
            }
            const std::uint8_t* src = data_.data() + (static_cast<std::size_t>(y) * width_ + x) * channels_;
            std::uint8_t* dst = out.data_.data() + (static_cast<std::size_t>(ny) * outWidth + nx) * channels_;
            std::memcpy(dst, src, channels_);
        }
    }
    return out;
}

void Image::paste(const Image& src, int x, int y) {
    const Image source = (src.channels() == channels_) ? src : src.convert(channels_);
    const int startX = std::max(0, -x);
    const int startY = std::max(0, -y);
    const int endX = std::min(source.width(), width_ - x);
    const int endY = std::min(source.height(), height_ - y);

    for (int sy = startY; sy < endY; ++sy) {
        for (int sx = startX; sx < endX; ++sx) {
            const std::uint8_t* sp = source.data() + (static_cast<std::size_t>(sy) * source.width() + sx) * channels_;
            std::uint8_t* dp = data_.data() + (static_cast<std::size_t>(y + sy) * width_ + (x + sx)) * channels_;
            if (channels_ == 4) {
                const int alpha = sp[3];
                if (alpha == 255) {
                    dp[0] = sp[0];
                    dp[1] = sp[1];
                    dp[2] = sp[2];
                    dp[3] = 255;
                } else if (alpha > 0) {
                    for (int c = 0; c < 3; ++c) {
                        dp[c] = static_cast<std::uint8_t>((sp[c] * alpha + dp[c] * (255 - alpha)) / 255);
                    }
                    dp[3] = 255;
                }
            } else {
                dp[0] = sp[0];
                dp[1] = sp[1];
                dp[2] = sp[2];
            }
        }
    }
}

}  // namespace lcdctl
