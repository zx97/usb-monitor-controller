// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - theme scheduler.
//
// Replaces the thread + sched-based scheduler of library/scheduler.py with a
// single-threaded, deadline-driven loop. The engine never talks to the hardware
// directly: it draws on a DrawSurface, which is either the live LcdComm or an
// OffscreenCanvas (simulation, editor preview).

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <string>

#include "lcdctl/draw.hpp"
#include "lcdctl/sensors.hpp"
#include "lcdctl/theme.hpp"
#include "lcdctl/theme_render.hpp"

namespace lcdctl {

class MonitorEngine {
public:
    // Total cost estimate of one full frame, in pixels (used by the CLI and the
    // tray tooltip to warn about slow revisions).
    static long framePixels(const Theme& theme);

    MonitorEngine(Theme theme, DrawSurface& surface, std::unique_ptr<SensorProvider> sensors);
    ~MonitorEngine();

    // Draw the static background, then one full frame of stats.
    void start();

    // Render the items whose interval elapsed. Call this often (20-50 ms).
    // Returns the number of primitives actually transmitted.
    int tick();

    // Redraw everything, ignoring the "value unchanged" shortcut.
    int redrawAll();

    void setTheme(Theme theme);
    const Theme& theme() const { return theme_; }

    void refreshSensors();                 // force a sampler run now
    const Metrics& metrics() const { return metrics_; }

    // Forget change detection and graph histories (after a reconnect).
    void invalidate();

    // Pause drawing (screen off in the tray) without stopping the sensors.
    void setPaused(bool paused) { paused_ = paused; }
    bool paused() const { return paused_; }

    const SensorProvider* sensors() const { return sensors_.get(); }

private:
    using Clock = std::chrono::steady_clock;
    int minIntervalSeconds() const;

    Theme theme_;
    DrawSurface& surface_;
    std::unique_ptr<SensorProvider> sensors_;
    std::unique_ptr<ThemeRenderer> renderer_;
    Metrics metrics_;
    std::map<std::string, Clock::time_point> nextDue_;
    Clock::time_point nextSensorRefresh_{};
    bool paused_ = false;
    bool started_ = false;
};

// Render `frames` offscreen frames with the stub sensors and write them as
// PNGs into `outputDirectory` (or a single file when frames == 1). Used by
// `lcd-monitor --simulate`, the tests and the CI.
struct SimulationOptions {
    int frames = 1;
    double fps = 1.0;
    Size size;
    std::string sensorSpec = "random";
    std::string outputDirectory = ".";
    std::string prefix = "frame";
};

int renderSimulation(const Theme& theme, const SimulationOptions& options);

}  // namespace lcdctl
