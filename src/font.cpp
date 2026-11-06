// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/font.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <stdexcept>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace lcdctl {

namespace {

// Minimal UTF-8 decoder: themes are full of "°C" and accented month names, and
// FreeType wants code points, not bytes.
std::uint32_t nextCodepoint(const std::string& text, std::size_t& index) {
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    std::uint32_t codepoint = lead;
    std::size_t length = 1;
    if (lead >= 0xF0) {
        codepoint = lead & 0x07;
        length = 4;
    } else if (lead >= 0xE0) {
        codepoint = lead & 0x0F;
        length = 3;
    } else if (lead >= 0xC0) {
        codepoint = lead & 0x1F;
        length = 2;
    }
    if (length > 1 && index + length <= text.size()) {
        for (std::size_t i = 1; i < length; ++i) {
            const unsigned char continuation = static_cast<unsigned char>(text[index + i]);
            if ((continuation & 0xC0) != 0x80) {
                length = 1;
                codepoint = lead;
                break;
            }
            codepoint = (codepoint << 6) | (continuation & 0x3F);
        }
    } else if (length > 1) {
        length = 1;
        codepoint = lead;
    }
    index += length;
    return codepoint;
}

}  // namespace

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

    for (std::size_t index = 0; index < text.size();) {
        const std::uint32_t ch = nextCodepoint(text, index);
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
    // Byte-indexed: widths[i] is the advance width of text[0..i). Every byte of
    // a multi-byte code point maps to the same cumulative width, so callers that
    // index by byte offset keep working with UTF-8 text.
    std::vector<int> widths(text.size() + 1, 0);
    if (!impl_ || !impl_->face) {
        return widths;
    }
    FT_Face face = impl_->face;
    int total = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        const std::size_t start = index;
        const std::uint32_t ch = nextCodepoint(text, index);
        widths[start] = total;
        if (FT_Load_Char(face, ch, FT_LOAD_DEFAULT) == 0) {
            total += static_cast<int>(face->glyph->advance.x >> 6);
        }
        for (std::size_t i = start + 1; i < index; ++i) {
            widths[i] = total;
        }
    }
    widths[text.size()] = total;
    return widths;
}

void Font::draw(Image& dst, int x, int y, const std::string& text, Color color) const {
    drawAtBaseline(dst, x, y + ascent(), text, color);
}

int Font::ascent() const {
    return static_cast<int>(impl_->face->size->metrics.ascender >> 6);
}

int Font::descent() const {
    return -static_cast<int>(impl_->face->size->metrics.descender >> 6);
}

TextMetrics Font::inkBox(const std::string& text) const {
    FT_Face face = impl_->face;
    int pen = 0;
    int left = INT_MAX;
    int top = INT_MAX;
    int right = INT_MIN;
    int bottom = INT_MIN;
    for (std::size_t index = 0; index < text.size();) {
        const std::uint32_t ch = nextCodepoint(text, index);
        if (FT_Load_Char(face, ch, FT_LOAD_RENDER) != 0) {
            continue;
        }
        const int glyphLeft = pen + face->glyph->bitmap_left;
        const int glyphTop = -static_cast<int>(face->glyph->bitmap_top);
        left = std::min(left, glyphLeft);
        right = std::max(right, glyphLeft + static_cast<int>(face->glyph->bitmap.width));
        top = std::min(top, glyphTop);
        bottom = std::max(bottom, glyphTop + static_cast<int>(face->glyph->bitmap.rows));
        pen += static_cast<int>(face->glyph->advance.x >> 6);
    }
    TextMetrics metrics;
    if (left == INT_MAX) {
        return metrics;
    }
    // Keep the advance width as the horizontal extent: Pillow anchors on the
    // layout width, not on the ink, so centring stays stable while digits change.
    metrics.left = std::min(left, 0);
    metrics.right = std::max(right, pen);
    metrics.top = top;
    metrics.bottom = bottom;
    return metrics;
}

void Font::drawAtBaseline(Image& dst, int x, int baseline, const std::string& text,
                          Color color) const {
    FT_Face face = impl_->face;
    if (dst.channels() != 3 && dst.channels() != 4) {
        throw std::invalid_argument("Font::draw requires an RGB or RGBA destination");
    }
    int pen = x;

    for (std::size_t index = 0; index < text.size();) {
        const std::uint32_t ch = nextCodepoint(text, index);
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
                pixel[0] = static_cast<std::uint8_t>((color.r * coverage + pixel[0] * (255 - coverage)) / 255);
                pixel[1] = static_cast<std::uint8_t>((color.g * coverage + pixel[1] * (255 - coverage)) / 255);
                pixel[2] = static_cast<std::uint8_t>((color.b * coverage + pixel[2] * (255 - coverage)) / 255);
                if (dst.channels() == 4) {
                    pixel[3] = 255;
                }
            }
        }
        pen += static_cast<int>(face->glyph->advance.x >> 6);
    }
}

}  // namespace lcdctl
