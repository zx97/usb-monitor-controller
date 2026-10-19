// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/console.hpp"

#include <algorithm>
#include <stdexcept>

#include "lcdctl/logging.hpp"
#include "lcdctl/paths.hpp"

namespace lcdctl {
namespace {

void fill(Image& image, Color color) {
    for (std::size_t i = 0; i + 2 < image.size(); i += 3) {
        image.data()[i + 0] = color.r;
        image.data()[i + 1] = color.g;
        image.data()[i + 2] = color.b;
    }
}

}  // namespace

TextConsole::TextConsole(LcdComm& lcd, Options options)
    : lcd_(lcd), options_(std::move(options)) {
    const std::string fontPath = options_.font.empty() ? defaultFontPath() : options_.font;
    font_ = std::make_shared<Font>(fontPath, options_.fontSize);
    lineHeight_ = std::max(1, font_->lineHeight() + options_.lineGap);
    capacity_ = std::max(1, options_.height / lineHeight_);

    const int areaHeight = capacity_ * lineHeight_;
    if (options_.x < 0 || options_.y < 0 || options_.width <= 0 ||
        options_.x + options_.width > lcd_.getWidth() ||
        options_.y + areaHeight > lcd_.getHeight()) {
        throw std::invalid_argument("console area does not fit on the display");
    }
    LCDCTL_DEBUG << "TextConsole " << options_.width << "x" << areaHeight << " at (" << options_.x
                 << "," << options_.y << "), " << capacity_ << " lines of " << lineHeight_ << "px";
}

void TextConsole::clear() {
    lines_.clear();
    Image blank(options_.width, capacity_ * lineHeight_, 3);
    fill(blank, options_.background);
    lcd_.displayPILImage(blank, options_.x, options_.y);
}

void TextConsole::draw(int firstLine, int count, int drawHeight) {
    Image image(options_.width, drawHeight, 3);
    fill(image, options_.background);
    for (int i = 0; i < count; ++i) {
        const int index = firstLine + i;
        if (index < 0 || index >= static_cast<int>(lines_.size())) {
            continue;
        }
        font_->draw(image, 0, i * lineHeight_, lines_[index], options_.textColor);
    }
    lcd_.displayPILImage(image, options_.x, options_.y + firstLine * lineHeight_);
}

void TextConsole::addLine(const std::string& text) {
    if (static_cast<int>(lines_.size()) < capacity_) {
        // Still filling up: nothing above changes, so only draw the new line.
        lines_.push_back(text);
        const int index = static_cast<int>(lines_.size()) - 1;
        draw(index, 1, lineHeight_);
        return;
    }

    // Full: shift everything up by one line and draw the whole area.
    lines_.pop_front();
    lines_.push_back(text);
    draw(0, capacity_, capacity_ * lineHeight_);
}

}  // namespace lcdctl
