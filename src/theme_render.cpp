// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/theme_render.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

#include "lcdctl/logging.hpp"

namespace lcdctl {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::string padNumber(long long value, int minSize) {
    std::string text = std::to_string(value);
    while (static_cast<int>(text.size()) < minSize) {
        text.insert(text.begin(), ' ');
    }
    return text;
}

std::string padNumber(const std::string& text, int minSize) {
    std::string padded = text;
    while (static_cast<int>(padded.size()) < minSize) {
        padded.insert(padded.begin(), ' ');
    }
    return padded;
}

std::string formatUptime(std::uint64_t seconds) {
    // datetime.timedelta(seconds=...) equivalent: "4 days, 16:47:16"
    const std::uint64_t days = seconds / 86400;
    const std::uint64_t hours = (seconds % 86400) / 3600;
    const std::uint64_t minutes = (seconds % 3600) / 60;
    const std::uint64_t secs = seconds % 60;
    std::ostringstream stream;
    if (days > 0) {
        stream << days << (days == 1 ? " day, " : " days, ");
    }
    stream << hours << ':' << (minutes < 10 ? "0" : "") << minutes << ':'
           << (secs < 10 ? "0" : "") << secs;
    return stream.str();
}

MetricReading makeReading(double value, const std::string& text, const std::string& unit,
                          bool available = true) {
    MetricReading reading;
    reading.available = available;
    reading.value = value;
    reading.text = text;
    reading.unit = unit;
    return reading;
}

// Integer percentage, like display_themed_percent_value().
MetricReading percentReading(double value, bool available) {
    const long long rounded = static_cast<long long>(std::llround(value));
    return makeReading(value, padNumber(rounded, 0), "%", available);
}

MetricReading temperatureReading(double value, bool available) {
    const long long rounded = static_cast<long long>(std::llround(value));
    return makeReading(value, padNumber(rounded, 0), "°C", available);
}

}  // namespace

// ---------------------------------------------------------------------------
// OffscreenCanvas
// ---------------------------------------------------------------------------

OffscreenCanvas::OffscreenCanvas(Size size, Color fill)
    : canvas_(std::max(1, size.width), std::max(1, size.height), 3) {
    for (int y = 0; y < canvas_.height(); ++y) {
        std::uint8_t* row = canvas_.data() + static_cast<std::size_t>(y) * canvas_.width() * 3;
        for (int x = 0; x < canvas_.width(); ++x) {
            row[x * 3 + 0] = fill.r;
            row[x * 3 + 1] = fill.g;
            row[x * 3 + 2] = fill.b;
        }
    }
}

const Image& OffscreenCanvas::cachedImage(const std::string& path) {
    auto it = images_.find(path);
    if (it == images_.end()) {
        it = images_.emplace(path, Image::load(path)).first;
    }
    return it->second;
}

std::shared_ptr<Font> OffscreenCanvas::cachedFont(const std::string& path, int size) {
    const std::string key = path + "#" + std::to_string(size);
    auto it = fonts_.find(key);
    if (it == fonts_.end()) {
        it = fonts_.emplace(key, std::make_shared<Font>(path, size)).first;
    }
    return it->second;
}

void OffscreenCanvas::displayPILImage(const Image& image, int x, int y) {
    if (image.channels() == 4) {
        canvas_.paste(image, x, y);
        return;
    }
    const int maxX = std::min(canvas_.width(), x + image.width());
    const int maxY = std::min(canvas_.height(), y + image.height());
    const int startX = std::max(0, x);
    const int startY = std::max(0, y);
    for (int py = startY; py < maxY; ++py) {
        const std::uint8_t* src =
            image.data() + (static_cast<std::size_t>(py - y) * image.width() + (startX - x)) *
                               image.channels();
        std::uint8_t* dst =
            canvas_.data() + (static_cast<std::size_t>(py) * canvas_.width() + startX) * 3;
        for (int px = startX; px < maxX; ++px) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            src += image.channels();
            dst += 3;
        }
    }
}

void OffscreenCanvas::clearCaches() {
    images_.clear();
    fonts_.clear();
}

// ---------------------------------------------------------------------------
// Metric formatting (mirrors library/stats.py)
// ---------------------------------------------------------------------------

MetricReading readMetric(const std::string& metricId, const Metrics& m) {
    // CPU
    if (metricId == "cpu.percentage") {
        return percentReading(m.cpu.percent, true);
    }
    if (metricId == "cpu.frequency") {
        std::ostringstream stream;
        stream.setf(std::ios::fixed);
        stream.precision(2);
        stream << m.cpu.frequencyMhz / 1000.0;
        return makeReading(m.cpu.frequencyMhz / 1000.0, stream.str(), " GHz");
    }
    if (metricId == "cpu.temperature") {
        return temperatureReading(m.cpu.temperatureC, m.cpu.hasTemperature);
    }
    if (metricId == "cpu.fan") {
        return percentReading(m.cpu.fanPercent, m.cpu.hasFan);
    }
    if (metricId == "cpu.load.one") {
        return percentReading(m.cpu.load1, true);
    }
    if (metricId == "cpu.load.five") {
        return percentReading(m.cpu.load5, true);
    }
    if (metricId == "cpu.load.fifteen") {
        return percentReading(m.cpu.load15, true);
    }

    // GPU
    if (metricId == "gpu.percentage") {
        return percentReading(m.gpu.load, m.gpu.available);
    }
    if (metricId == "gpu.memoryPercent" || metricId == "gpu.memory") {
        return percentReading(m.gpu.memoryPercent, m.gpu.available);
    }
    if (metricId == "gpu.memory.bytes") {
        return makeReading(m.gpu.memoryUsedMb, padNumber(std::to_string(
                                                   static_cast<long long>(m.gpu.memoryUsedMb)), 0),
                           " M", m.gpu.available);
    }
    if (metricId == "gpu.temperature") {
        return temperatureReading(m.gpu.temperatureC, m.gpu.available);
    }
    if (metricId == "gpu.fan") {
        return percentReading(m.gpu.fanPercent, m.gpu.available);
    }
    if (metricId == "gpu.frequency") {
        std::ostringstream stream;
        stream.setf(std::ios::fixed);
        stream.precision(2);
        stream << m.gpu.frequencyMhz / 1000.0;
        return makeReading(m.gpu.frequencyMhz / 1000.0, stream.str(), " GHz", m.gpu.available);
    }
    if (metricId == "gpu.fps") {
        return makeReading(m.gpu.fps, padNumber(std::to_string(m.gpu.fps), 0), " FPS",
                           m.gpu.available);
    }
    if (metricId == "gpu.formatted") {
        std::ostringstream stream;
        stream << "GPU " << static_cast<int>(std::lround(m.gpu.load)) << " %";
        return makeReading(m.gpu.load, stream.str(), "", m.gpu.available);
    }

    // Memory
    if (metricId == "mem.virtual.percent" || metricId == "mem.virtual") {
        return percentReading(m.memory.virtualPercent, true);
    }
    if (metricId == "mem.virtual.used") {
        const long long megabytes = static_cast<long long>(m.memory.virtualUsed / (1024 * 1024));
        return makeReading(static_cast<double>(megabytes), padNumber(megabytes, 0), " M");
    }
    if (metricId == "mem.virtual.free") {
        const long long megabytes = static_cast<long long>(m.memory.virtualFree / (1024 * 1024));
        return makeReading(static_cast<double>(megabytes), padNumber(megabytes, 0), " M");
    }
    if (metricId == "mem.virtual.total") {
        const long long megabytes = static_cast<long long>(m.memory.virtualTotal / (1024 * 1024));
        return makeReading(static_cast<double>(megabytes), padNumber(megabytes, 0), " M");
    }
    if (metricId == "mem.swap.percent" || metricId == "mem.swap") {
        return percentReading(m.memory.swapPercent, true);
    }
    if (metricId == "mem.swap.used") {
        const long long megabytes = static_cast<long long>(m.memory.swapUsed / (1024 * 1024));
        return makeReading(static_cast<double>(megabytes), padNumber(megabytes, 0), " M");
    }
    if (metricId == "mem.swap.free") {
        const long long megabytes = static_cast<long long>(m.memory.swapFree / (1024 * 1024));
        return makeReading(static_cast<double>(megabytes), padNumber(megabytes, 0), " M");
    }
    if (metricId == "mem.swap.total") {
        const long long megabytes = static_cast<long long>(m.memory.swapTotal / (1024 * 1024));
        return makeReading(static_cast<double>(megabytes), padNumber(megabytes, 0), " M");
    }

    // Disk
    if (metricId == "disk.percent" || metricId == "disk.used") {
        return percentReading(m.disk.usedPercent, true);
    }
    if (metricId == "disk.used.bytes") {
        const long long gigabytes = static_cast<long long>(m.disk.used / 1000000000ull);
        return makeReading(static_cast<double>(gigabytes), padNumber(gigabytes, 0), " G");
    }
    if (metricId == "disk.free.bytes") {
        const long long gigabytes = static_cast<long long>(m.disk.free / 1000000000ull);
        return makeReading(static_cast<double>(gigabytes), padNumber(gigabytes, 0), " G");
    }
    if (metricId == "disk.total.bytes") {
        const long long gigabytes = static_cast<long long>(m.disk.total / 1000000000ull);
        return makeReading(static_cast<double>(gigabytes), padNumber(gigabytes, 0), " G");
    }

    // Network
    if (metricId.rfind("net.", 0) == 0) {
        const bool wireless = metricId.rfind("net.wlo", 0) == 0;
        const NetMetrics& net = wireless ? m.wlo : m.eth;
        if (metricId.find(".upload") != std::string::npos &&
            metricId.find(".uploaded") == std::string::npos) {
            return makeReading(net.uploadBps, bytesToHuman(net.uploadBps, true), "", true);
        }
        if (metricId.find(".download") != std::string::npos &&
            metricId.find(".downloaded") == std::string::npos) {
            return makeReading(net.downloadBps, bytesToHuman(net.downloadBps, true), "", true);
        }
        if (metricId.find(".uploaded") != std::string::npos) {
            return makeReading(static_cast<double>(net.uploadedBytes),
                               bytesToHuman(static_cast<double>(net.uploadedBytes)), "", true);
        }
        if (metricId.find(".downloaded") != std::string::npos) {
            return makeReading(static_cast<double>(net.downloadedBytes),
                               bytesToHuman(static_cast<double>(net.downloadedBytes)), "", true);
        }
    }

    // Date / uptime
    if (metricId == "date.day") {
        return makeReading(0, m.dateDay, "");
    }
    if (metricId == "date.hour") {
        return makeReading(0, m.dateHour, "");
    }
    if (metricId == "uptime.seconds") {
        return makeReading(static_cast<double>(m.uptimeSeconds),
                           std::to_string(m.uptimeSeconds), "");
    }
    if (metricId == "uptime.formatted") {
        return makeReading(static_cast<double>(m.uptimeSeconds), formatUptime(m.uptimeSeconds),
                           "");
    }

    // Custom: "custom.<name>" (numeric when the command printed a number).
    if (metricId.rfind("custom.", 0) == 0) {
        const std::string name = metricId.substr(7);
        auto it = m.custom.find(name);
        if (it != m.custom.end()) {
            const CustomMetrics& custom = it->second;
            if (!std::isnan(custom.numeric)) {
                return makeReading(custom.numeric, custom.text, "");
            }
            return makeReading(kNaN, custom.text, "", false);
        }
    }

    return makeReading(kNaN, "--", "", false);
}

// ---------------------------------------------------------------------------
// ThemeRenderer
// ---------------------------------------------------------------------------

ThemeRenderer::ThemeRenderer(const Theme& theme, DrawSurface& surface)
    : theme_(theme), surface_(surface) {}

void ThemeRenderer::drawBackground() {
    for (const ThemeImage& image : theme_.images) {
        if (image.path.empty()) {
            continue;
        }
        try {
            const Image& source = surface_.cachedImage(image.path);
            Image picture = source;
            if (image.width > 0 && image.height > 0 &&
                (image.width != source.width() || image.height != source.height())) {
                picture = source.resize(image.width, image.height);
            }
            surface_.displayPILImage(picture, image.x, image.y);
        } catch (const std::exception& error) {
            LCDCTL_WARN << "theme image failed: " << image.path << ": " << error.what();
        }
    }
    for (const ThemeText& text : theme_.texts) {
        if (text.text.empty()) {
            continue;
        }
        TextStyle style = text.style;
        style.x = text.x;
        style.y = text.y;
        try {
            drawText(surface_, text.text, style);
        } catch (const std::exception& error) {
            LCDCTL_WARN << "static text failed (" << text.name << "): " << error.what();
        }
    }
}

void ThemeRenderer::reset() {
    lastValue_.clear();
    history_.clear();
}

bool ThemeRenderer::drawItem(const ThemeItem& item, const Metrics& metrics) {
    if (!item.show || item.interval <= 0) {
        return false;
    }

    MetricReading reading = readMetric(item.metric, metrics);
    if (!reading.available && item.kind == ThemeItem::Kind::Text) {
        // Show the placeholder so a missing GPU still looks intentional.
        reading.text = "--";
        reading.unit.clear();
    }

    // Line graphs always move: keep the history and redraw.
    if (item.kind == ThemeItem::Kind::LineGraph) {
        std::deque<float>& values = history_[item.key];
        if (static_cast<int>(values.size()) != item.historySize) {
            values.assign(static_cast<std::size_t>(item.historySize),
                          std::numeric_limits<float>::quiet_NaN());
        }
        values.push_back(static_cast<float>(reading.value));
        values.pop_front();

        std::vector<float> series(values.begin(), values.end());
        LineGraphStyle style = item.graph;
        try {
            drawLineGraph(surface_, series, style);
        } catch (const std::exception& error) {
            LCDCTL_WARN << "line graph failed (" << item.key << "): " << error.what();
            return false;
        }
        lastValue_[item.key] = std::to_string(reading.value);
        return true;
    }

    switch (item.kind) {
        case ThemeItem::Kind::Text: {
            const std::string text =
                padNumber(reading.text, item.minSize) + (item.showUnit ? reading.unit : "");
            if (lastValue_[item.key] == text) {
                return false;
            }
            TextStyle style = item.text;
            try {
                drawText(surface_, text, style);
            } catch (const std::exception& error) {
                LCDCTL_WARN << "text failed (" << item.key << "): " << error.what();
                return false;
            }
            lastValue_[item.key] = text;
            return true;
        }
        case ThemeItem::Kind::ProgressBar: {
            const int value = static_cast<int>(std::lround(reading.value));
            const std::string key = item.key + "#" + std::to_string(value);
            if (lastValue_[item.key] == key) {
                return false;
            }
            ProgressBarStyle style = item.bar;
            style.value = value;
            try {
                drawProgressBar(surface_, style);
            } catch (const std::exception& error) {
                LCDCTL_WARN << "progress bar failed (" << item.key << "): " << error.what();
                return false;
            }
            lastValue_[item.key] = key;
            return true;
        }
        case ThemeItem::Kind::Radial: {
            const std::string text =
                padNumber(reading.text, item.minSize) + (item.showUnit ? reading.unit : "");
            if (lastValue_[item.key] == text) {
                return false;
            }
            RadialStyle style = item.radial;
            style.value = static_cast<int>(std::lround(reading.value));
            style.text = style.withText ? text : std::string();
            try {
                drawRadialProgressBar(surface_, style);
            } catch (const std::exception& error) {
                LCDCTL_WARN << "radial bar failed (" << item.key << "): " << error.what();
                return false;
            }
            lastValue_[item.key] = text;
            return true;
        }
        case ThemeItem::Kind::LineGraph:
            return false;
    }
    return false;
}

int ThemeRenderer::drawAll(const Metrics& metrics) {
    int transmitted = 0;
    for (const ThemeItem& item : theme_.items) {
        if (drawItem(item, metrics)) {
            ++transmitted;
        }
    }
    lastTransmitted_ = transmitted;
    return transmitted;
}

}  // namespace lcdctl
