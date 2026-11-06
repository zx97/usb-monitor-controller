// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/lcd_comm.hpp"

#include <algorithm>
#include <stdexcept>

#include "lcdctl/draw.hpp"
#include "lcdctl/logging.hpp"

namespace lcdctl {

LcdComm::LcdComm(std::unique_ptr<Transport> transport, int displayWidth, int displayHeight)
    : transport_(std::move(transport)), displayWidth_(displayWidth), displayHeight_(displayHeight) {}

int LcdComm::getWidth() const {
    return (orientation_ == Orientation::Portrait || orientation_ == Orientation::ReversePortrait)
               ? displayWidth_
               : displayHeight_;
}

int LcdComm::getHeight() const {
    return (orientation_ == Orientation::Portrait || orientation_ == Orientation::ReversePortrait)
               ? displayHeight_
               : displayWidth_;
}

void LcdComm::writeData(const std::vector<std::uint8_t>& data) { writeLine(data); }

void LcdComm::writeLine(const std::vector<std::uint8_t>& line) {
    if (!transport_ || !transport_->isOpen()) {
        throw std::runtime_error("transport is not open");
    }
    transport_->write(line);
}

void LcdComm::sendLine(const std::vector<std::uint8_t>& line) { writeLine(line); }

std::vector<std::uint8_t> LcdComm::readData(std::size_t size) {
    if (!transport_ || !transport_->isOpen()) {
        throw std::runtime_error("transport is not open");
    }
    return transport_->read(size, 1000);
}

void LcdComm::flushInput() {
    if (transport_) {
        transport_->flushInput();
    }
}

const Image& LcdComm::cachedImage(const std::string& path) {
    auto it = imageCache_.find(path);
    if (it == imageCache_.end()) {
        LCDCTL_DEBUG << "Loading image into cache: " << path;
        it = imageCache_.emplace(path, Image::load(path)).first;
    }
    return it->second;
}

std::shared_ptr<Font> LcdComm::cachedFont(const std::string& path, int size) {
    const std::string key = path + "#" + std::to_string(size);
    auto it = fontCache_.find(key);
    if (it == fontCache_.end()) {
        it = fontCache_.emplace(key, std::make_shared<Font>(path, size)).first;
    }
    return it->second;
}

void LcdComm::displayBitmap(const std::string& path, int x, int y, int width, int height) {
    Image image = cachedImage(path);
    if (width != 0 && height != 0 && (width != image.width() || height != image.height())) {
        image = image.resize(width, height);
    }
    displayPILImage(image, x, y, width, height);
}

void LcdComm::fillScreen(Color color) {
    const int width = getWidth();
    const int height = getHeight();
    Image image(width, height, 3);
    for (int py = 0; py < height; ++py) {
        std::uint8_t* row = image.data() + static_cast<std::size_t>(py) * width * image.channels();
        for (int px = 0; px < width; ++px) {
            std::uint8_t* pixel = row + static_cast<std::size_t>(px) * image.channels();
            pixel[0] = color.r;
            pixel[1] = color.g;
            pixel[2] = color.b;
        }
    }
    displayPILImage(image, 0, 0);
}

void LcdComm::displayText(const std::string& text, int x, int y, const TextStyle& style) {
    drawText(*this, text, x, y, style);
}

void LcdComm::displayProgressBar(int x, int y, int width, int height, int minValue,
                                 int maxValue, int value, Color barColor, bool barOutline,
                                 Color backgroundColor, const std::string& backgroundImage,
                                 bool reverseDirection) {
    ProgressBarStyle style;
    style.minValue = minValue;
    style.maxValue = maxValue;
    style.barColor = barColor;
    style.barOutline = barOutline;
    style.backgroundColor = backgroundColor;
    style.backgroundImage = backgroundImage;
    style.reverseDirection = reverseDirection;
    drawProgressBar(*this, x, y, width, height, value, style);
}

void LcdComm::displayLineGraph(int x, int y, int width, int height,
                               const std::vector<float>& values, float minValue, float maxValue,
                               bool autoscale, Color lineColor, int lineWidth, bool graphAxis,
                               Color axisColor, Color backgroundColor,
                               const std::string& backgroundImage) {
    LineGraphStyle style;
    style.minValue = minValue;
    style.maxValue = maxValue;
    style.autoscale = autoscale;
    style.lineColor = lineColor;
    style.lineWidth = lineWidth;
    style.graphAxis = graphAxis;
    style.axisColor = axisColor;
    style.backgroundColor = backgroundColor;
    style.backgroundImage = backgroundImage;
    drawLineGraph(*this, x, y, width, height, values, style);
}

void LcdComm::displayRadialProgressBar(int xc, int yc, int radius, int barWidth, int value,
                                       const RadialStyle& style) {
    drawRadialProgressBar(*this, xc, yc, radius, barWidth, value, style);
}

}  // namespace lcdctl
