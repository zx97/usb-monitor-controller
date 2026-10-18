// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/lcd_comm.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "lcdctl/logging.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace lcdctl {
namespace {

void fillRect(Image& image, int x, int y, int width, int height, Color color) {
    const int endX = std::min(image.width(), x + width);
    const int endY = std::min(image.height(), y + height);
    for (int py = std::max(0, y); py < endY; ++py) {
        std::uint8_t* row = image.data() + static_cast<std::size_t>(py) * image.width() * image.channels();
        for (int px = std::max(0, x); px < endX; ++px) {
            std::uint8_t* pixel = row + static_cast<std::size_t>(px) * image.channels();
            pixel[0] = color.r;
            pixel[1] = color.g;
            pixel[2] = color.b;
            if (image.channels() == 4) {
                pixel[3] = 255;
            }
        }
    }
}

void drawLine(Image& image, int x0, int y0, int x1, int y1, Color color, int thickness) {
    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    const int half = std::max(0, thickness / 2);
    for (;;) {
        fillRect(image, x0 - half, y0 - half, thickness, thickness, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * error;
        if (e2 >= dy) {
            error += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void drawDisc(Image& image, int cx, int cy, int radius, Color color) {
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) {
                fillRect(image, cx + x, cy + y, 1, 1, color);
            }
        }
    }
}

// Draw a thick arc as a series of discs sampled along the circle.
void drawThickArc(Image& image, int cx, int cy, double radius, double startDeg, double endDeg,
                  int thickness, Color color) {
    double sweep = endDeg - startDeg;
    while (sweep < 0) {
        sweep += 360;
    }
    while (sweep >= 360) {
        sweep -= 360;
    }
    const double step = 0.5;
    const int discRadius = std::max(1, thickness / 2);
    for (double angle = 0; angle <= sweep; angle += step) {
        const double radians = (startDeg + angle) * M_PI / 180.0;
        const int x = static_cast<int>(std::lround(cx + radius * std::cos(radians)));
        const int y = static_cast<int>(std::lround(cy + radius * std::sin(radians)));
        drawDisc(image, x, y, discRadius, color);
    }
}

}  // namespace

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

void LcdComm::displayText(const std::string& text, int x, int y, const TextStyle& style) {
    if (text.empty()) {
        return;
    }
    if (style.fontSize <= 0) {
        throw std::invalid_argument("font size must be > 0");
    }

    const int width = (style.width > 0) ? style.width : getWidth();
    const int height = (style.height > 0) ? style.height : style.fontSize;

    Image canvas;
    if (!style.backgroundImage.empty()) {
        canvas = cachedImage(style.backgroundImage);
    } else {
        canvas = Image(getWidth(), getHeight(), 3);
        fillRect(canvas, 0, 0, getWidth(), getHeight(), style.backgroundColor);
    }

    const std::string fontPath = style.font.empty() ? defaultFontPath() : style.font;
    const std::shared_ptr<Font> font = cachedFont(fontPath, style.fontSize);

    // Horizontal alignment inside [x, x + width].
    int drawX = x;
    if (style.align == "center") {
        const TextMetrics metrics = font->measure(text);
        drawX = x + (width - (metrics.right - metrics.left)) / 2;
    } else if (style.align == "right") {
        const TextMetrics metrics = font->measure(text);
        drawX = x + width - (metrics.right - metrics.left);
    }
    drawX = std::max(0, std::min(drawX, getWidth()));

    const TextMetrics metrics = font->measure(text);
    int left = drawX + metrics.left;
    int top = std::max(0, y);
    int right = drawX + metrics.right;
    int bottom = top + (metrics.bottom - metrics.top);

    font->draw(canvas, drawX, top, text, style.fontColor);

    left = std::max(left, 0);
    top = std::max(top, 0);
    right = std::min(right, getWidth());
    bottom = std::min(bottom, getHeight());
    if (right <= left || bottom <= top) {
        return;
    }

    const Image cropped = canvas.crop(left, top, right - left, bottom - top);
    displayPILImage(cropped, left, top);
    (void)height;
}

void LcdComm::displayProgressBar(int x, int y, int width, int height, int minValue, int maxValue,
                                 int value, Color barColor, bool barOutline, Color backgroundColor,
                                 const std::string& backgroundImage, bool reverseDirection) {
    if (minValue >= maxValue) {
        throw std::invalid_argument("progress bar requires min < max");
    }
    value = std::max(minValue, std::min(value, maxValue));

    Image bar;
    if (!backgroundImage.empty()) {
        bar = cachedImage(backgroundImage).crop(x, y, width, height);
    } else {
        bar = Image(width, height, 3);
        fillRect(bar, 0, 0, width, height, backgroundColor);
    }

    const double fraction = static_cast<double>(value - minValue) / (maxValue - minValue);
    if (width > height) {
        int filled = static_cast<int>(fraction * width) - 1;
        filled = std::max(0, filled);
        const int x0 = reverseDirection ? (width - 1 - filled) : 0;
        const int x1 = reverseDirection ? (width - 1) : filled;
        fillRect(bar, x0, 0, x1 - x0 + 1, height, barColor);
    } else {
        int filled = static_cast<int>(fraction * height) - 1;
        filled = std::max(0, filled);
        const int y0 = reverseDirection ? 0 : (height - 1 - filled);
        const int y1 = reverseDirection ? filled : (height - 1);
        fillRect(bar, 0, y0, width, y1 - y0 + 1, barColor);
    }

    if (barOutline) {
        fillRect(bar, 0, 0, width, 1, barColor);
        fillRect(bar, 0, height - 1, width, 1, barColor);
        fillRect(bar, 0, 0, 1, height, barColor);
        fillRect(bar, width - 1, 0, 1, height, barColor);
    }

    displayPILImage(bar, x, y);
}

void LcdComm::displayLineGraph(int x, int y, int width, int height,
                               const std::vector<float>& values, float minValue, float maxValue,
                               bool autoscale, Color lineColor, int lineWidth, bool graphAxis,
                               Color axisColor, Color backgroundColor,
                               const std::string& backgroundImage) {
    if (values.empty()) {
        return;
    }

    Image graph;
    if (!backgroundImage.empty()) {
        graph = cachedImage(backgroundImage).crop(x, y, width, height);
    } else {
        graph = Image(width, height, 3);
        fillRect(graph, 0, 0, width, height, backgroundColor);
    }

    if (autoscale) {
        float trueMin = maxValue;
        float trueMax = minValue;
        for (float v : values) {
            if (std::isnan(v)) continue;
            trueMin = std::min(trueMin, v);
            trueMax = std::max(trueMax, v);
        }
        if (trueMin != maxValue && trueMax != minValue) {
            minValue = std::max(trueMin - 5, minValue);
            maxValue = std::min(trueMax + 5, maxValue);
        }
    }
    if (maxValue == minValue) {
        maxValue = minValue + 1;
    }

    const double step = static_cast<double>(width) / values.size();
    const double yScale = height / static_cast<double>(maxValue - minValue);

    int previousX = 0;
    int previousY = 0;
    bool hasPrevious = false;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (std::isnan(values[i])) {
            hasPrevious = false;
            continue;
        }
        const float clamped = std::min(maxValue, std::max(minValue, values[i]));
        const int px = static_cast<int>(i * step);
        const int py = static_cast<int>(height - (clamped - minValue) * yScale);
        if (hasPrevious) {
            drawLine(graph, previousX, previousY, px, py, lineColor, lineWidth);
        }
        previousX = px;
        previousY = py;
        hasPrevious = true;
    }

    if (graphAxis) {
        drawLine(graph, 0, height - 1, width - 1, height - 1, axisColor, 1);
        drawLine(graph, 0, 0, 0, height - 1, axisColor, 1);
    }

    displayPILImage(graph, x, y);
}

void LcdComm::displayRadialProgressBar(int xc, int yc, int radius, int barWidth, int value,
                                       const RadialStyle& style) {
    if (barWidth <= 0 || barWidth > radius) {
        throw std::invalid_argument("radial bar width must be within [1, radius]");
    }
    value = std::max(style.minValue, std::min(value, style.maxValue));

    const int diameter = 2 * radius;
    Image bar;
    if (!style.backgroundImage.empty()) {
        bar = cachedImage(style.backgroundImage).crop(xc - radius, yc - radius, diameter, diameter);
    } else {
        bar = Image(diameter, diameter, 3);
        fillRect(bar, 0, 0, diameter, diameter, style.backgroundColor);
    }

    double start = style.angleStart;
    double end = style.angleEnd;
    double sweep = end - start;
    while (sweep < 0) sweep += 360;
    while (sweep >= 360) sweep -= 360;
    if (sweep == 0) {
        sweep = 360;
    }

    const double fraction =
        static_cast<double>(value - style.minValue) / (style.maxValue - style.minValue);

    if (style.drawBarBackground) {
        drawThickArc(bar, radius, radius, radius - barWidth / 2.0, start, start + sweep, barWidth,
                     style.barBackgroundColor);
    }
    drawThickArc(bar, radius, radius, radius - barWidth / 2.0, start, start + fraction * sweep,
                 barWidth, style.barColor);

    if (style.withText) {
        std::string text = style.text;
        if (text.empty()) {
            text = std::to_string(static_cast<int>(fraction * 100 + 0.5)) + "%";
        }
        const std::string fontPath = style.font.empty() ? defaultFontPath() : style.font;
        const std::shared_ptr<Font> font = cachedFont(fontPath, style.fontSize);
        const TextMetrics metrics = font->measure(text);
        const int textWidth = metrics.right - metrics.left;
        const int textHeight = metrics.bottom - metrics.top;
        font->draw(bar, radius - textWidth / 2, radius - textHeight / 2, text, style.fontColor);
    }

    displayPILImage(bar, xc - radius, yc - radius);
}

}  // namespace lcdctl
