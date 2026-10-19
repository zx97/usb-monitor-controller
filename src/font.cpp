// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/font.hpp"

#include <algorithm>
#include <climits>
#include <stdexcept>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace lcdctl {

struct Font::Impl {
    FT_Library library = nullptr;
    FT_Face face = nullptr;

    ~Impl() {
        if (face) {
            FT_Done_Face(face);
        }
        if (library) {
            FT_Done_FreeType(library);
        }
    }
};

Font::Font(const std::string& path, int pixelSize) : pixelSize_(pixelSize) {
    impl_ = std::make_unique<Impl>();
    if (FT_Init_FreeType(&impl_->library) != 0) {
        throw std::runtime_error("FreeType: initialization failed");
    }
    if (FT_New_Face(impl_->library, path.c_str(), 0, &impl_->face) != 0) {
        FT_Done_FreeType(impl_->library);
        impl_->library = nullptr;
        throw std::runtime_error("FreeType: cannot load font " + path);
    }
    FT_Set_Pixel_Sizes(impl_->face, 0, static_cast<FT_UInt>(pixelSize));
}

Font::~Font() = default;

int Font::lineHeight() const {
    return static_cast<int>(impl_->face->size->metrics.height >> 6);
}

TextMetrics Font::measure(const std::string& text) const {
    FT_Face face = impl_->face;
    int pen = 0;
    int top = INT_MAX;
    int bottom = INT_MIN;

    for (unsigned char ch : text) {
        if (FT_Load_Char(face, ch, FT_LOAD_RENDER) != 0) {
            continue;
        }
        const int glyphTop = -face->glyph->bitmap_top;  // y grows downwards
        const int glyphBottom = glyphTop + static_cast<int>(face->glyph->bitmap.rows);
        top = std::min(top, glyphTop);
        bottom = std::max(bottom, glyphBottom);
        pen += static_cast<int>(face->glyph->advance.x >> 6);
    }

    TextMetrics metrics;
    if (top == INT_MAX) {
        return metrics;  // empty
    }
    const int ascender = static_cast<int>(face->size->metrics.ascender >> 6);
    metrics.left = 0;
    metrics.right = pen;
    metrics.top = ascender + top;
    metrics.bottom = ascender + bottom;
    return metrics;
}

std::vector<int> Font::prefixWidths(const std::string& text) const {
    std::vector<int> widths(text.size() + 1, 0);
    if (!impl_ || !impl_->face) {
        return widths;
    }
    FT_Face face = impl_->face;
    int total = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (FT_Load_Char(face, static_cast<unsigned char>(text[i]), FT_LOAD_DEFAULT) == 0) {
            total += static_cast<int>(face->glyph->advance.x >> 6);
        }
        widths[i + 1] = total;
    }
    return widths;
}

void Font::draw(Image& dst, int x, int y, const std::string& text, Color color) const {
    FT_Face face = impl_->face;
    if (dst.channels() != 3 && dst.channels() != 4) {
        throw std::invalid_argument("Font::draw requires an RGB or RGBA destination");
    }
    const int baseline = y + static_cast<int>(face->size->metrics.ascender >> 6);
    int pen = x;

    for (unsigned char ch : text) {
        if (FT_Load_Char(face, ch, FT_LOAD_RENDER) != 0) {
            continue;
        }
        const FT_Bitmap& bitmap = face->glyph->bitmap;
        const int glyphX = pen + face->glyph->bitmap_left;
        const int glyphY = baseline - face->glyph->bitmap_top;

        for (unsigned row = 0; row < bitmap.rows; ++row) {
            for (unsigned col = 0; col < bitmap.width; ++col) {
                const int px = glyphX + static_cast<int>(col);
                const int py = glyphY + static_cast<int>(row);
                if (px < 0 || py < 0 || px >= dst.width() || py >= dst.height()) {
                    continue;
                }
                const int coverage = bitmap.buffer[row * bitmap.pitch + col];
                if (coverage == 0) {
                    continue;
                }
                std::uint8_t* pixel =
                    dst.data() + (static_cast<std::size_t>(py) * dst.width() + px) * dst.channels();
                const int alpha = coverage;
                pixel[0] = static_cast<std::uint8_t>((color.r * alpha + pixel[0] * (255 - alpha)) / 255);
                pixel[1] = static_cast<std::uint8_t>((color.g * alpha + pixel[1] * (255 - alpha)) / 255);
                pixel[2] = static_cast<std::uint8_t>((color.b * alpha + pixel[2] * (255 - alpha)) / 255);
                if (dst.channels() == 4) {
                    pixel[3] = 255;
                }
            }
        }
        pen += static_cast<int>(face->glyph->advance.x >> 6);
    }
}

}  // namespace lcdctl
