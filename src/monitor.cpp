// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/monitor.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

#include "lcdctl/image.hpp"
#include "lcdctl/logging.hpp"

namespace lcdctl {
namespace {
namespace fs = std::filesystem;
constexpr int kDefaultSensorRefreshMs = 1000;
}  // namespace

long MonitorEngine::framePixels(const Theme& theme) {
    long pixels = 0;
    for (const ThemeItem& item : theme.items) {
        if (!item.show) {
            continue;
        }
        const Rect rect = item.bounds();
        if (rect.width > 0 && rect.height > 0) {
            pixels += static_cast<long>(rect.width) * rect.height;
        }
    }
    return pixels;
}

MonitorEngine::MonitorEngine(Theme theme, DrawSurface& surface,
                             std::unique_ptr<SensorProvider> sensors)
    : theme_(std::move(theme)),
      surface_(surface),
      sensors_(std::move(sensors)),
      renderer_(std::make_unique<ThemeRenderer>(theme_, surface_)) {}

MonitorEngine::~MonitorEngine() = default;

int MonitorEngine::minIntervalSeconds() const {
    int interval = 0;
    for (const ThemeItem& item : theme_.items) {
        if (!item.show || item.interval <= 0) {
            continue;
        }
        interval = (interval == 0) ? item.interval : std::min(interval, item.interval);
    }
    return interval == 0 ? 1 : interval;
}

void MonitorEngine::start() {
    repaintAll();
    started_ = true;
}

void MonitorEngine::repaintAll() {
    // The background must be re-sent, not just the stats: after a rotation the
    // panel shows a rotated copy of the old frame (or nothing for 90°/270°,
    // where the canvas dimensions swap), and after a screen-off the content is
    // not guaranteed to survive on every model.
    renderer_->drawBackground();
    nextSensorRefresh_ = Clock::now();
    refreshSensors();
    redrawAll();
}

void MonitorEngine::refreshSensors() {
    if (sensors_) {
        sensors_->refresh(metrics_);
    }
}

int MonitorEngine::tick() {
    if (!started_) {
        start();
        return 0;
    }
    if (paused_) {
        return 0;
    }

    const auto now = Clock::now();

    // Refresh the sensors only when at least one item is due.
    if (now >= nextSensorRefresh_) {
        bool anyDue = false;
        for (const ThemeItem& item : theme_.items) {
            if (!item.show || item.interval <= 0) {
                continue;
            }
            auto it = nextDue_.find(item.key);
            if (it == nextDue_.end() || now >= it->second) {
                anyDue = true;
                break;
            }
        }
        if (anyDue) {
            refreshSensors();
        }
        nextSensorRefresh_ = now + std::chrono::milliseconds(250);
    }

    int transmitted = 0;
    for (const ThemeItem& item : theme_.items) {
        if (!item.show || item.interval <= 0) {
            continue;
        }
        auto it = nextDue_.find(item.key);
        if (it != nextDue_.end() && now < it->second) {
            continue;
        }
        if (renderer_->drawItem(item, metrics_)) {
            ++transmitted;
        }
        nextDue_[item.key] = now + std::chrono::seconds(item.interval);
    }
    return transmitted;
}

int MonitorEngine::redrawAll() {
    renderer_->reset();
    nextDue_.clear();
    const int transmitted = renderer_->drawAll(metrics_);
    const auto now = Clock::now();
    for (const ThemeItem& item : theme_.items) {
        if (!item.show || item.interval <= 0) {
            continue;
        }
        nextDue_[item.key] = now + std::chrono::seconds(item.interval);
    }
    return transmitted;
}

void MonitorEngine::setTheme(Theme theme) {
    theme_ = std::move(theme);
    renderer_ = std::make_unique<ThemeRenderer>(theme_, surface_);
    nextDue_.clear();
    renderer_->drawBackground();
    redrawAll();
}

void MonitorEngine::invalidate() {
    renderer_->reset();
    nextDue_.clear();
}

int renderSimulation(const Theme& theme, const SimulationOptions& options) {
    Size size = options.size;
    if (size.width <= 0 || size.height <= 0) {
        size = theme.declaredSize();
        if (size.width <= 0) size.width = 320;
        if (size.height <= 0) size.height = 480;
    }
    if (theme.orientation == Orientation::Landscape ||
        theme.orientation == Orientation::ReverseLandscape) {
        std::swap(size.width, size.height);
    }

    SensorSettings settings;
    auto sensors = makeStubSensors(options.sensorSpec, settings);

    OffscreenCanvas canvas(size, Color{0, 0, 0});
    ThemeRenderer renderer(theme, canvas);
    renderer.drawBackground();

    std::error_code ec;
    if (options.frames > 1) {
        fs::create_directories(options.outputDirectory, ec);
    }

    Metrics metrics;
    for (int frame = 0; frame < std::max(1, options.frames); ++frame) {
        sensors->refresh(metrics);
        renderer.drawAll(metrics);

        std::string path;
        if (options.frames <= 1) {
            path = options.outputDirectory;
        } else {
            char name[256];
            std::snprintf(name, sizeof(name), "%s-%04d.png", options.prefix.c_str(), frame);
            path = (fs::path(options.outputDirectory) / name).string();
        }
        savePng(path, canvas.framebuffer());
    }
    return 0;
}

}  // namespace lcdctl
