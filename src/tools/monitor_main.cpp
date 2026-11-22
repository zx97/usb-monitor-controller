// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - lcd-monitor: system-monitor daemon for the USB LCD
// screens. Runs the theme engine continuously, exposes a KDE/XDG tray icon and
// can start automatically at login.
//
// Modes:
//   lcd-monitor                 tray icon (falls back to headless)
//   lcd-monitor --headless      no GUI (systemd, ssh)
//   lcd-monitor --once          one tick, print the metrics as JSON
//   lcd-monitor --simulate ...  render frames to PNG (no screen, no GUI)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "lcdctl/factory.hpp"
#include "lcdctl/logging.hpp"
#include "lcdctl/monitor.hpp"
#include "lcdctl/sensors.hpp"
#include "lcdctl/settings.hpp"
#include "lcdctl/theme.hpp"
#include "lcdctl/theme_render.hpp"

#ifdef LCDCTL_WITH_QT
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSharedMemory>
#include <QSlider>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#endif

#if defined(_WIN32)
#include <windows.h>
#include <cstdio>
#endif

namespace {

using namespace lcdctl;

const char* kUsage =
    "Usage: lcd-monitor [options]\n"
    "\n"
    "Runs the configured theme continuously on the first detected screen.\n"
    "\n"
    "Options:\n"
    "  --config <file>           Configuration (default: ./config.yaml or\n"
    "                            ~/.config/usb-monitor-controller/config.yaml)\n"
    "  --theme <name>            Theme to display (default: config THEME)\n"
    "  --revision <value>        A|B|C|D|TUR_USB|WEACT_A|WEACT_B|AUTO\n"
    "  --port <device>           Serial port (default: AUTO)\n"
    "  --brightness <0-100>      Override the configured brightness\n"
    "  --orientation <value>     portrait|landscape|reverse-portrait|reverse-landscape\n"
    "                            (or AUTO: follow the theme layout)\n"
    "  --reverse                 Shorthand for a 180° rotation of the theme\n"
    "  --force-orientation       Allow a 90°/270° rotation that changes the aspect\n"
    "  --tray                    Tray icon + control window (default on a desktop)\n"
    "  --window                  Open the control window at startup\n"
    "  --headless                No GUI at all (systemd / ssh)\n"
    "  --once, --dump-json       Refresh once, print metrics as JSON, exit\n"
    "  --simulate                Render to PNG instead of driving a screen\n"
    "  --frames <n>              Frames to render in --simulate (default 1)\n"
    "  --fps <n>                 Simulated frame rate (default 1)\n"
    "  --stub <spec>             static | random | static:cpu=42,gpu=12\n"
    "  --output <dir|file>       Simulation output (default frame.png / frames/)\n"
    "  --width <px> --height <px> Offscreen size for --simulate\n"
    "  --root <dir> --fonts <dir> Theme and font roots\n"
    "  --verbose, -v             Debug logging\n";

struct Options {
    std::map<std::string, std::string> values;
    std::vector<std::string> positional;

    bool flag(const std::string& name) const { return values.count(name) > 0; }
    std::string get(const std::string& name, const std::string& fallback = "") const {
        auto it = values.find(name);
        return it == values.end() ? fallback : it->second;
    }
    int getInt(const std::string& name, int fallback) const {
        auto it = values.find(name);
        if (it == values.end() || it->second.empty()) return fallback;
        return std::stoi(it->second);
    }
    double getDouble(const std::string& name, double fallback) const {
        auto it = values.find(name);
        if (it == values.end() || it->second.empty()) return fallback;
        return std::stod(it->second);
    }
};

Options parseArgs(int argc, char** argv) {
    static const std::vector<std::string> flags = {
        "--tray", "--window", "--headless", "--once", "--dump-json", "--simulate",
        "--reverse", "--force-orientation", "--verbose", "-v", "--help", "-h"};
    static const std::vector<std::string> valued = {
        "--config", "--theme", "--revision", "--port", "--brightness", "--frames",
        "--fps",    "--stub",  "--output",   "--width", "--height",   "--orientation",
        "--root",   "--fonts"};
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string token = argv[i];
        if (token == "-v") token = "--verbose";
        if (token == "-h") token = "--help";
        if (std::find(valued.begin(), valued.end(), token) != valued.end()) {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + token);
            }
            options.values[token] = argv[++i];
            continue;
        }
        if (std::find(flags.begin(), flags.end(), token) != flags.end()) {
            options.values[token] = "1";
            continue;
        }
        options.positional.push_back(token);
    }
    return options;
}

Settings loadSettings(const Options& options) {
    Settings settings = Settings::load(options.get("--config", defaultConfigPath()));
    if (options.values.count("--revision")) settings.revision = options.get("--revision");
    if (options.values.count("--port")) settings.comPort = options.get("--port");
    if (options.values.count("--brightness")) {
        settings.brightness = options.getInt("--brightness", settings.brightness);
    }
    if (options.values.count("--root")) settings.themeRoot = options.get("--root");
    if (options.values.count("--fonts")) settings.fontsRoot = options.get("--fonts");
    if (options.values.count("--orientation")) {
        settings.displayOrientation = options.get("--orientation");
    }
    if (options.flag("--reverse")) {
        settings.displayReverse = true;
        settings.displayOrientation = "AUTO";
    }
    if (options.flag("--force-orientation")) {
        settings.forceOrientation = true;
    }
    return settings;
}

std::string themeNameFor(const Options& options, const Settings& settings) {
    if (options.values.count("--theme")) {
        return options.get("--theme");
    }
    if (!settings.themeName.empty()) {
        return settings.themeName;
    }
    const std::vector<ThemeInfo> themes = listThemes(settings.themeRoot);
    if (themes.empty()) {
        throw std::runtime_error("no theme available in " + settings.themeRoot);
    }
    return themes.front().name;
}

Theme loadThemeForDisplay(const Settings& settings, const std::string& name) {
    Theme theme = Theme::loadByName(name, settings.themeRoot, settings.fontsRoot);
    const OrientationDecision decision = resolveOrientation(settings, theme.orientation);
    if (!decision.warning.empty()) {
        LCDCTL_WARN << decision.warning;
    }
    theme.orientation = decision.orientation;
    return theme;
}

std::unique_ptr<SensorProvider> makeSensors(const Settings& settings,
                                            const std::string& stubSpec) {
    if (!stubSpec.empty()) {
        return makeStubSensors(stubSpec, settings.sensorSettings());
    }
    const std::string kind = settings.hwSensors;
    if (kind == "STUB" || kind == "STATIC") {
        return makeStaticSensors(settings.sensorSettings());
    }
    if (kind == "RANDOM") {
        return makeRandomSensors(settings.sensorSettings());
    }
    return makeSystemSensors(settings.sensorSettings());
}

std::unique_ptr<LcdComm> openDevice(const Settings& settings, const Theme& theme) {
    Revision revision = Revision::A;
    const std::string requested = settings.revision.empty() ? "AUTO" : settings.revision;
    if (requested != "AUTO" && !parseRevision(requested, revision)) {
        throw std::runtime_error("unknown revision: " + requested);
    }
    std::string port = settings.comPort;
    if (port == "AUTO") {
        port = "";
    }

    auto lcd = createLcd(revision, port, 0, 0, 115200);
    if (settings.resetOnStartup) {
        try {
            lcd->reset();
        } catch (const std::exception& error) {
            LCDCTL_WARN << "reset failed: " << error.what();
        }
    }
    lcd->initializeComm();
    lcd->setOrientation(theme.orientation);
    lcd->setBrightness(settings.brightness);
    if (theme.hasBackplateLed) {
        lcd->setBackplateLedColor(theme.backplateLed);
    }
    return lcd;
}

// Minimal JSON string escaping: Windows paths ("C:\") and device names must not
// produce invalid JSON.
std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

void printMetricsJson(const Metrics& m) {
    std::printf("{\n");
    std::printf("  \"cpu\": {\"percent\": %.1f, \"frequency_mhz\": %.0f, \"load\": [%.1f, %.1f, %.1f],"
                " \"temperature_c\": %.1f, \"fan_percent\": %.1f},\n",
                m.cpu.percent, m.cpu.frequencyMhz, m.cpu.load1, m.cpu.load5, m.cpu.load15,
                m.cpu.temperatureC, m.cpu.fanPercent);
    std::printf("  \"gpu\": {\"available\": %s, \"load\": %.1f, \"memory_percent\": %.1f,"
                " \"memory_used_mb\": %.0f, \"memory_total_mb\": %.0f, \"temperature_c\": %.1f,"
                " \"frequency_mhz\": %.0f, \"model\": \"%s\"},\n",
                m.gpu.available ? "true" : "false", m.gpu.load, m.gpu.memoryPercent,
                m.gpu.memoryUsedMb, m.gpu.memoryTotalMb, m.gpu.temperatureC, m.gpu.frequencyMhz,
                jsonEscape(m.gpu.model).c_str());
    std::printf("  \"memory\": {\"percent\": %.1f, \"used\": %llu, \"free\": %llu, \"total\": %llu},\n",
                m.memory.virtualPercent, static_cast<unsigned long long>(m.memory.virtualUsed),
                static_cast<unsigned long long>(m.memory.virtualFree),
                static_cast<unsigned long long>(m.memory.virtualTotal));
    std::printf("  \"disk\": {\"percent\": %.1f, \"used\": %llu, \"free\": %llu, \"total\": %llu,"
                " \"mount\": \"%s\"},\n",
                m.disk.usedPercent, static_cast<unsigned long long>(m.disk.used),
                static_cast<unsigned long long>(m.disk.free),
                static_cast<unsigned long long>(m.disk.total),
                jsonEscape(m.disk.mountPoint).c_str());
    std::printf("  \"network\": {\"eth\": {\"up_bps\": %.0f, \"down_bps\": %.0f, \"name\": \"%s\"},"
                " \"wlo\": {\"up_bps\": %.0f, \"down_bps\": %.0f, \"name\": \"%s\"}},\n",
                m.eth.uploadBps, m.eth.downloadBps, jsonEscape(m.eth.interfaceName).c_str(),
                m.wlo.uploadBps, m.wlo.downloadBps, jsonEscape(m.wlo.interfaceName).c_str());
    std::printf("  \"date\": {\"day\": \"%s\", \"hour\": \"%s\"},\n",
                jsonEscape(m.dateDay).c_str(), jsonEscape(m.dateHour).c_str());
    std::printf("  \"uptime_seconds\": %llu\n", static_cast<unsigned long long>(m.uptimeSeconds));
    std::printf("}\n");
}

int runSimulation(const Options& options, const Settings& settings) {
    const std::string name = themeNameFor(options, settings);
    const Theme theme = loadThemeForDisplay(settings, name);

    SimulationOptions simulation;
    simulation.frames = std::max(1, options.getInt("--frames", 1));
    simulation.fps = options.getDouble("--fps", 1.0);
    simulation.size = {options.getInt("--width", 0), options.getInt("--height", 0)};
    simulation.sensorSpec = options.get("--stub", "random");
    const std::string output = options.get("--output", "");
    simulation.outputDirectory =
        output.empty() ? (simulation.frames > 1 ? "frames" : "frame.png") : output;

    renderSimulation(theme, simulation);
    std::printf("Rendered %d frame(s) of %s -> %s\n", simulation.frames, theme.name.c_str(),
                simulation.outputDirectory.c_str());
    return 0;
}

int runOnce(const Options& options, const Settings& settings) {
    const std::string name = themeNameFor(options, settings);
    const Theme theme = loadThemeForDisplay(settings, name);
    Size size = theme.declaredSize();
    if (size.width <= 0 || size.height <= 0) {
        size = {320, 480};
    }

    OffscreenCanvas canvas(size, Color{0, 0, 0});
    auto sensors = makeSensors(settings, options.get("--stub", ""));
    Metrics metrics;
    // Prime the delta-based metrics (CPU %, network rates) before sampling.
    sensors->refresh(metrics);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    sensors->refresh(metrics);

    ThemeRenderer renderer(theme, canvas);
    renderer.drawBackground();
    const int transmitted = renderer.drawAll(metrics);

    printMetricsJson(metrics);
    std::fprintf(stderr, "// theme %s: %d item(s) rendered, %d warning(s)\n", theme.name.c_str(),
                 transmitted, static_cast<int>(theme.warnings.size()));
    return 0;
}

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop = true; }

int runHeadless(const Options& options, const Settings& settings) {
    const std::string name = themeNameFor(options, settings);
    Theme theme = loadThemeForDisplay(settings, name);

    auto lcd = openDevice(settings, theme);
    std::printf("lcd-monitor: theme %s on a %dx%d screen\n", theme.name.c_str(), lcd->getWidth(),
                lcd->getHeight());

    auto sensors = makeSensors(settings, options.get("--stub", ""));
    MonitorEngine engine(theme, *lcd, std::move(sensors));
    engine.start();

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    while (!g_stop) {
        try {
            engine.tick();
        } catch (const std::exception& error) {
            LCDCTL_INFO << "display error: " << error.what() << " (retrying in 3 s)";
            std::this_thread::sleep_for(std::chrono::seconds(3));
            try {
                lcd = openDevice(settings, theme);
                engine.invalidate();
                engine.start();
            } catch (const std::exception& reconnectError) {
                LCDCTL_INFO << "reconnect failed: " << reconnectError.what();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    try {
        lcd->screenOff();
    } catch (...) {
    }
    return 0;
}

#ifdef LCDCTL_WITH_QT

// ---------------------------------------------------------------------------
// Display worker: owns the device and the render loop.
//
// Every USB transfer is synchronous and a revision A screen needs ~2.4 s for a
// full frame, so the engine must never run on the GUI thread: the tray menu and
// the control window would only answer at the panel's own speed. The worker
// lives in its own QThread; the GUI talks to it with queued lambda invocations
// and receives status updates the same way.
// ---------------------------------------------------------------------------

struct DisplayStatus {
    int width = 0;
    int height = 0;
    Orientation orientation = Orientation::Portrait;
    bool paused = false;
    bool hasScreen = false;
    bool busy = false;          // a frame is being transmitted right now
    std::string theme;
    std::string error;
};

class DisplayWorker : public QObject {
public:
    DisplayWorker(Settings settings, Options options,
                  std::function<void(const DisplayStatus&)> notify)
        : settings_(std::move(settings)), options_(std::move(options)),
          notify_(std::move(notify)) {}

    // --- called from the worker thread only --------------------------------
    void start(const std::string& themeName) {
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, [this] { tick(); });
        timer_->start(50);
        applyTheme(themeName);
    }

    void shutdown() {
        if (lcd_) {
            try {
                lcd_->screenOff();
            } catch (...) {
            }
        }
    }

    void selectTheme(const std::string& name) { applyTheme(name); }
    void setBrightness(int level) {
        settings_.brightness = level;
        if (lcd_) {
            try {
                lcd_->setBrightness(level);
            } catch (const std::exception& error) {
                LCDCTL_WARN << "brightness failed: " << error.what();
            }
        }
        publish();
    }
    void selectOrientation(Orientation orientation) {
        settings_.displayOrientation = orientationName(orientation);
        settings_.displayReverse = false;
        if (lcd_) {
            try {
                lcd_->setOrientation(orientation);
            } catch (const std::exception& error) {
                LCDCTL_WARN << "rotation failed: " << error.what();
            }
        }
        // The engine caches the rendered values and the graph history; a
        // rotation only changes the panel, so a full redraw is enough.
        if (engine_) {
            engine_->invalidate();
            busy_ = true;
            publish();
            engine_->redrawAll();
            busy_ = false;
        }
        publish();
    }
    void setScreenEnabled(bool on) {
        if (!lcd_ || !engine_) {
            return;
        }
        try {
            if (on) {
                lcd_->screenOn();
                lcd_->setBrightness(settings_.brightness);
                engine_->setPaused(false);
                engine_->invalidate();
                busy_ = true;
                publish();
                engine_->redrawAll();
                busy_ = false;
            } else {
                engine_->setPaused(true);
                lcd_->screenOff();
            }
        } catch (const std::exception& error) {
            LCDCTL_WARN << "screen toggle failed: " << error.what();
        }
        publish();
    }

private:
    void applyTheme(const std::string& name) {
        try {
            Theme theme = loadThemeForDisplay(settings_, name);
            themeOrientation_ = theme.orientation;
            if (!lcd_) {
                lcd_ = openDevice(settings_, theme);
            } else {
                lcd_->setOrientation(theme.orientation);
                if (theme.hasBackplateLed) {
                    lcd_->setBackplateLedColor(theme.backplateLed);
                }
            }
            auto sensors = makeSensors(settings_, options_.get("--stub", ""));
            busy_ = true;
            publish();
            engine_ = std::make_unique<MonitorEngine>(theme, *lcd_, std::move(sensors));
            engine_->start();
            busy_ = false;
            currentTheme_ = name;
            error_.clear();
            publish();
        } catch (const std::exception& exception) {
            error_ = exception.what();
            engine_.reset();
            // Keep the device if it was already open; retry in the background.
            if (!retry_) {
                retry_ = new QTimer(this);
                connect(retry_, &QTimer::timeout, this, [this, name] {
                    QString retryError;
                    applyTheme(name);
                    if (error_.empty()) {
                        retry_->stop();
                    }
                });
            }
            if (!retry_->isActive()) {
                retry_->start(5000);
            }
            publish();
        }
    }

    void tick() {
        if (!engine_ || engine_->paused() || busy_) {
            return;
        }
        try {
            engine_->tick();
        } catch (const std::exception& error) {
            LCDCTL_WARN << "display error: " << error.what();
            error_ = error.what();
            engine_.reset();
            publish();
            if (retry_ && !retry_->isActive()) {
                retry_->start(5000);
                retryCurrent_ = currentTheme_;
            }
        }
    }

    void publish() {
        DisplayStatus status;
        status.hasScreen = lcd_ != nullptr;
        if (lcd_) {
            status.width = lcd_->getWidth();
            status.height = lcd_->getHeight();
            status.orientation = lcd_->orientation();
        } else {
            status.orientation = themeOrientation_;
        }
        status.paused = engine_ && engine_->paused();
        status.busy = busy_;
        status.theme = currentTheme_;
        status.error = error_;
        if (std::getenv("LCDCTL_DEBUG_STATUS") != nullptr) {
            std::fprintf(stderr, "[worker] screen=%d %dx%d theme='%s' error='%s'\n",
                         status.hasScreen ? 1 : 0, status.width, status.height,
                         status.theme.c_str(), status.error.c_str());
        }
        if (notify_) {
            notify_(status);
        }
    }

    Settings settings_;
    Options options_;
    std::function<void(const DisplayStatus&)> notify_;
    std::unique_ptr<LcdComm> lcd_;
    std::unique_ptr<MonitorEngine> engine_;
    QTimer* timer_ = nullptr;
    QTimer* retry_ = nullptr;
    std::string retryCurrent_;
    std::string currentTheme_;
    std::string error_;
    Orientation themeOrientation_ = Orientation::Portrait;
    bool busy_ = false;
};

QIcon makeTrayIcon(int size = 64) {
    // Prefer the icon shipped with the project (res/icons/, imported from
    // turing-smart-screen-python); fall back to a drawn one when it is absent.
    const std::string root = resourceRoot();
    if (!root.empty()) {
        const QString path = QString::fromStdString(root + "/res/icons/monitor-icon-17865/64.png");
        if (QFileInfo::exists(path)) {
            return QIcon(path);
        }
    }
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF screen(6, 8, size - 12, size - 24);
    painter.setBrush(QColor(24, 26, 32));
    painter.setPen(QPen(QColor(97, 190, 255), 3));
    painter.drawRoundedRect(screen, 6, 6);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(97, 190, 255));
    painter.drawRect(QRectF(screen.left() + 4, screen.top() + 4, (screen.width() - 8) * 0.55, 4));
    painter.drawRect(QRectF(screen.left() + 4, screen.top() + 12, (screen.width() - 8) * 0.35, 4));
    painter.drawRect(QRectF(screen.left() + 4, screen.top() + 20, (screen.width() - 8) * 0.7, 4));
    painter.setBrush(QColor(160, 170, 185));
    painter.drawRect(QRectF(size / 2.0 - 6, screen.bottom() + 2, 12, 5));
    painter.drawRect(QRectF(size / 2.0 - 14, screen.bottom() + 7, 28, 3));
    return QIcon(pixmap);
}

class MonitorApp : public QObject {
public:
    MonitorApp(const Settings& settings, const Options& options, std::string settingsPath,
               bool showWindow, QSharedMemory* instance)
        : settings_(settings), options_(options), settingsPath_(std::move(settingsPath)),
          showWindow_(showWindow), instance_(instance) {}

    ~MonitorApp() override {
        if (worker_ == nullptr) {
            return;
        }
        // Ask the worker to turn the screen off, then stop its event loop. A
        // device reset can block the worker for ~10 s (open retries), so give it
        // time; destroying a QThread that still runs aborts the process.
        invokeWorker([](DisplayWorker& worker) { worker.shutdown(); });
        workerThread_.quit();
        if (!workerThread_.wait(20000)) {
            LCDCTL_WARN << "display worker did not stop in time, terminating it";
            workerThread_.terminate();
            workerThread_.wait(2000);
        }
    }

    bool start(QString* error) {
        try {
            themeName_ = themeNameFor(options_, settings_);
        } catch (const std::exception& exception) {
            *error = QString::fromUtf8(exception.what());
            return false;
        }

        worker_ = new DisplayWorker(settings_, options_, [this](const DisplayStatus& status) {
            // Called from the worker thread: hop back to the GUI thread.
            QMetaObject::invokeMethod(this, [this, status] { onStatus(status); },
                                      Qt::QueuedConnection);
        });
        worker_->moveToThread(&workerThread_);
        connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
        workerThread_.start();

        invokeWorker([name = themeName_](DisplayWorker& worker) { worker.start(name); });

        setupUi();

        if (instance_ != nullptr) {
            auto* poll = new QTimer(this);
            connect(poll, &QTimer::timeout, this, [this] {
                if (instance_ == nullptr || !instance_->isAttached()) {
                    return;
                }
                auto* bytes = static_cast<char*>(instance_->data());
                if (bytes != nullptr && bytes[0] != 0) {
                    bytes[0] = 0;
                    showControlWindow();
                }
            });
            poll->start(500);
        }
        return true;
    }

    // Tray icon when the shell offers one, control window as a guaranteed way
    // in (Windows hides new notification icons in the overflow area).
    void setupUi() {
        if (QSystemTrayIcon::isSystemTrayAvailable()) {
            buildTray();
            if (showWindow_) {
                showControlWindow();
            }
            return;
        }
        if (showWindow_) {
            showControlWindow();
        }

        // Debug/CI: render the control window to a PNG and exit.
        if (const char* screenshot = std::getenv("LCDCTL_WINDOW_SCREENSHOT")) {
            const QString path = QString::fromUtf8(screenshot);
            const char* delayText = std::getenv("LCDCTL_WINDOW_SCREENSHOT_DELAY_MS");
            const int delay = delayText != nullptr ? std::atoi(delayText) : 800;
            QTimer::singleShot(std::max(100, delay), this, [this, path] {
                if (window_ != nullptr) {
                    window_->grab().save(path);
                }
                QApplication::quit();
            });
        }

        trayRetry_ = new QTimer(this);
        trayRetryCount_ = 0;
        connect(trayRetry_, &QTimer::timeout, this, [this] {
            if (QSystemTrayIcon::isSystemTrayAvailable()) {
                trayRetry_->stop();
                buildTray();
                return;
            }
            if (++trayRetryCount_ >= 30) {
                trayRetry_->stop();
                showControlWindow();
            }
        });
        trayRetry_->start(1000);
    }

    void showControlWindow() {
        if (window_ == nullptr) {
            buildControlWindow();
        }
        window_->show();
        window_->raise();
        window_->activateWindow();
        syncControlWindow();
    }

private:
    void invokeWorker(std::function<void(DisplayWorker&)> task) {
        if (worker_ == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            worker_, [worker = worker_, task = std::move(task)] { task(*worker); },
            Qt::QueuedConnection);
    }

    // GUI thread: everything the worker reports lands here.
    void onStatus(const DisplayStatus& status) {
        status_ = status;
        if (status.hasScreen) {
            themeOrientation_ = status.orientation;
        }
        syncControlWindow();
        syncTrayChecks();
        if (tray_ != nullptr) {
            tray_->setToolTip(QString("USB Monitor Controller \u2014 %1 (%2x%3)")
                                  .arg(QString::fromStdString(status.theme))
                                  .arg(status.width)
                                  .arg(status.height));
        }

    }

    void buildTray() {
        tray_ = new QSystemTrayIcon(makeTrayIcon(), this);
        tray_->setToolTip("USB Monitor Controller");

        auto* menu = new QMenu();
        auto* statusAction = menu->addAction("USB Monitor Controller");
        statusAction->setEnabled(false);
        menu->addSeparator();

        auto* screenAction = menu->addAction("Screen on");
        screenAction->setCheckable(true);
        screenAction->setChecked(true);
        windowScreenAction_ = screenAction;
        connect(screenAction, &QAction::toggled, this, [this](bool on) {
            invokeWorker([on](DisplayWorker& worker) { worker.setScreenEnabled(on); });
        });

        auto* brightnessMenu = menu->addMenu("Brightness");
        for (int level : {20, 40, 60, 80, 100}) {
            auto* action = brightnessMenu->addAction(QString::number(level) + " %");
            connect(action, &QAction::triggered, this, [this, level] { setBrightness(level); });
        }

        auto* orientationMenu = menu->addMenu("Orientation");
        orientationGroup_ = new QActionGroup(orientationMenu);
        orientationGroup_->setExclusive(true);
        struct RotationEntry {
            Orientation orientation;
            const char* label;
            bool needsLandscape;
        };
        static const RotationEntry rotations[] = {
            {Orientation::Portrait, "Portrait", false},
            {Orientation::ReversePortrait, "Portrait (upside down)", false},
            {Orientation::Landscape, "Landscape", true},
            {Orientation::ReverseLandscape, "Landscape (upside down)", true},
        };
        for (const RotationEntry& entry : rotations) {
            auto* action = orientationMenu->addAction(entry.label);
            action->setCheckable(true);
            action->setActionGroup(orientationGroup_);
            orientationActions_.push_back({action, entry.orientation});
            connect(action, &QAction::triggered, this, [this, entry] {
                const bool compatible = sameAspect(entry.orientation, themeOrientation_);
                applyOrientation(entry.orientation, compatible);
            });
        }

        auto* themeMenu = menu->addMenu("Theme");
        themeGroup_ = new QActionGroup(themeMenu);
        themeGroup_->setExclusive(true);
        for (const ThemeInfo& info : listThemes(settings_.themeRoot)) {
            auto* action = themeMenu->addAction(QString::fromStdString(info.name));
            action->setCheckable(true);
            action->setChecked(info.name == themeName_);
            themeGroup_->addAction(action);
            const std::string name = info.name;
            connect(action, &QAction::triggered, this, [this, name] {
                selectTheme(QString::fromStdString(name));
            });
        }

        auto* windowAction = menu->addAction("Control panel\u2026");
        connect(windowAction, &QAction::triggered, this, [this] { showControlWindow(); });

        auto* editAction = menu->addAction("Edit theme\u2026");
        connect(editAction, &QAction::triggered, this, [this] { launchEditor(); });

        menu->addSeparator();
        auto* autostartAction = menu->addAction("Start at login");
        autostartAction->setCheckable(true);
        autostartAction->setChecked(Settings::autostartEnabled());
        autostartAction_ = autostartAction;
        connect(autostartAction, &QAction::toggled, this, [this](bool on) {
            Settings::setAutostartEnabled(on);
            settings_.startAtLogin = on;
            persistSettings();
        });

        menu->addSeparator();
        auto* quitAction = menu->addAction("Quit");
        connect(quitAction, &QAction::triggered, this, [] {
            g_stop = true;
            QApplication::quit();
        });

        tray_->setContextMenu(menu);
        connect(tray_, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason reason) {
                    if (reason == QSystemTrayIcon::Trigger ||
                        reason == QSystemTrayIcon::DoubleClick) {
                        showControlWindow();
                    }
                });
        tray_->show();
        syncTrayChecks();

        if (!settings_.trayTipShown) {
            tray_->showMessage(
                "USB Monitor Controller",
                "The icon is in the notification area: click the ^ chevron next to "
                "the clock, then drag it onto the taskbar to keep it visible. "
                "Double-click the icon to open the control panel.",
                QSystemTrayIcon::Information, 10000);
            settings_.trayTipShown = true;
            persistSettings();
        }
    }

    void syncTrayChecks() {
        if (tray_ == nullptr) {
            return;
        }
        if (windowScreenAction_ != nullptr) {
            const bool blocked = windowScreenAction_->blockSignals(true);
            windowScreenAction_->setChecked(!status_.paused);
            windowScreenAction_->blockSignals(blocked);
        }
        for (auto& entry : orientationActions_) {
            const bool blocked = entry.first->blockSignals(true);
            entry.first->setChecked(entry.second == status_.orientation);
            entry.first->blockSignals(blocked);
        }
        if (themeGroup_ != nullptr) {
            for (QAction* action : themeGroup_->actions()) {
                const bool blocked = action->blockSignals(true);
                action->setChecked(action->text().toStdString() == status_.theme);
                action->blockSignals(blocked);
            }
        }
    }

    void buildControlWindow() {
        window_ = new QWidget();
        window_->setWindowTitle("USB Monitor Controller");
        window_->setWindowIcon(makeTrayIcon());
        window_->setMinimumWidth(420);

        auto* layout = new QVBoxLayout(window_);
        windowStatus_ = new QLabel();
        windowStatus_->setWordWrap(true);
        layout->addWidget(windowStatus_);
        auto* form = new QFormLayout();

        windowScreen_ = new QCheckBox("Screen on");
        windowScreen_->setChecked(!status_.paused);
        form->addRow(windowScreen_);
        connect(windowScreen_, &QCheckBox::toggled, this, [this](bool on) {
            invokeWorker([on](DisplayWorker& worker) { worker.setScreenEnabled(on); });
        });

        windowBrightness_ = new QSlider(Qt::Horizontal);
        windowBrightness_->setRange(0, 100);
        windowBrightness_->setValue(settings_.brightness);
        form->addRow("Brightness", windowBrightness_);
        connect(windowBrightness_, &QSlider::valueChanged, this,
                [this](int value) { setBrightness(value); });

        windowOrientation_ = new QComboBox();
        windowOrientation_->addItem("Portrait", static_cast<int>(Orientation::Portrait));
        windowOrientation_->addItem("Portrait (upside down)",
                                    static_cast<int>(Orientation::ReversePortrait));
        windowOrientation_->addItem("Landscape", static_cast<int>(Orientation::Landscape));
        windowOrientation_->addItem("Landscape (upside down)",
                                    static_cast<int>(Orientation::ReverseLandscape));
        form->addRow("Orientation", windowOrientation_);
        connect(windowOrientation_, &QComboBox::currentIndexChanged, this, [this](int index) {
            if (index < 0 || updatingWindow_) {
                return;
            }
            const auto orientation =
                static_cast<Orientation>(windowOrientation_->itemData(index).toInt());
            applyOrientation(orientation, sameAspect(orientation, themeOrientation_));
        });

        windowTheme_ = new QComboBox();
        for (const ThemeInfo& info : listThemes(settings_.themeRoot)) {
            windowTheme_->addItem(QString::fromStdString(info.name));
        }
        form->addRow("Theme", windowTheme_);
        connect(windowTheme_, &QComboBox::currentTextChanged, this, [this](const QString& name) {
            if (updatingWindow_ || name.isEmpty()) {
                return;
            }
            selectTheme(name);
        });

        windowAutostart_ = new QCheckBox("Start at login");
        windowAutostart_->setChecked(Settings::autostartEnabled());
        form->addRow(windowAutostart_);
        connect(windowAutostart_, &QCheckBox::toggled, this, [this](bool on) {
            Settings::setAutostartEnabled(on);
            settings_.startAtLogin = on;
            persistSettings();
        });

        layout->addLayout(form);

        auto* buttons = new QHBoxLayout();
        auto* edit = new QPushButton("Edit theme\u2026");
        auto* quit = new QPushButton("Quit");
        buttons->addWidget(edit);
        buttons->addStretch();
        buttons->addWidget(quit);
        layout->addLayout(buttons);

        connect(edit, &QPushButton::clicked, this, [this] { launchEditor(); });
        connect(quit, &QPushButton::clicked, this, [] {
            g_stop = true;
            QApplication::quit();
        });

        syncControlWindow();
    }

    void syncControlWindow() {
        if (window_ == nullptr) {
            return;
        }
        updatingWindow_ = true;
        if (windowScreen_ != nullptr) {
            windowScreen_->setChecked(!status_.paused);
        }
        if (windowBrightness_ != nullptr) {
            windowBrightness_->setValue(settings_.brightness);
        }
        if (windowOrientation_ != nullptr) {
            const int index = windowOrientation_->findData(static_cast<int>(status_.orientation));
            if (index >= 0) {
                windowOrientation_->setCurrentIndex(index);
            }
        }
        if (windowTheme_ != nullptr && !status_.theme.empty()) {
            const int index = windowTheme_->findText(QString::fromStdString(status_.theme));
            if (index >= 0) {
                windowTheme_->setCurrentIndex(index);
            }
        }
        if (windowAutostart_ != nullptr) {
            windowAutostart_->setChecked(Settings::autostartEnabled());
        }
        if (windowStatus_ != nullptr) {
            if (!status_.error.empty()) {
                windowStatus_->setText(
                    QString("No screen: %1\n(plug the screen; retrying every 5 s)")
                        .arg(QString::fromStdString(status_.error)));
                windowStatus_->setStyleSheet("color: #c0392b;");
            } else if (status_.hasScreen) {
                windowStatus_->setText(QString("Screen %1x%2 \u2014 theme %3")
                                           .arg(status_.width)
                                           .arg(status_.height)
                                           .arg(QString::fromStdString(status_.theme)));
                windowStatus_->setStyleSheet("color: #2d7a3e;");
            } else {
                windowStatus_->setText(QString("Connecting to the screen\u2026"));
                windowStatus_->setStyleSheet("color: #7f8c8d;");
            }
        }
        updatingWindow_ = false;
    }

    void selectTheme(const QString& name) {
        themeName_ = name.toStdString();
        settings_.themeName = themeName_;
        persistSettings();
        invokeWorker([name = themeName_](DisplayWorker& worker) { worker.selectTheme(name); });
    }

    void applyOrientation(Orientation orientation, bool compatible) {
        settings_.displayOrientation = orientationName(orientation);
        settings_.displayReverse = false;
        persistSettings();
        invokeWorker([orientation](DisplayWorker& worker) {
            worker.selectOrientation(orientation);
        });
        if (!compatible && tray_ != nullptr) {
            tray_->showMessage("USB Monitor",
                               "This theme layout does not match the rotation: "
                               "the screen is cropped.",
                               QSystemTrayIcon::Warning);
        }
    }

    void setBrightness(int level) {
        settings_.brightness = level;
        persistSettings();
        invokeWorker([level](DisplayWorker& worker) { worker.setBrightness(level); });
    }

    void persistSettings() {
        try {
            settings_.save(settingsPath_);
        } catch (const std::exception& error) {
            LCDCTL_WARN << "cannot save settings: " << error.what();
        }
    }

    void launchEditor() {
        if (themeName_.empty()) {
            return;
        }
        const QString executable = QCoreApplication::applicationDirPath() + "/lcd-theme-editor";
        const QStringList arguments{QString::fromStdString(settings_.themeRoot),
                                    QString::fromStdString(settings_.fontsRoot),
                                    QString::fromStdString(themeName_)};
        if (!QProcess::startDetached(executable, arguments) && tray_) {
            tray_->showMessage("USB Monitor", "lcd-theme-editor not found",
                               QSystemTrayIcon::Warning);
        }
    }

    Settings settings_;
    Options options_;
    std::string settingsPath_;
    std::string themeName_;
    bool showWindow_ = false;
    QSharedMemory* instance_ = nullptr;

    QThread workerThread_;
    DisplayWorker* worker_ = nullptr;
    DisplayStatus status_;
    Orientation themeOrientation_ = Orientation::Portrait;

    QSystemTrayIcon* tray_ = nullptr;
    QAction* windowScreenAction_ = nullptr;
    QAction* autostartAction_ = nullptr;
    QActionGroup* orientationGroup_ = nullptr;
    QActionGroup* themeGroup_ = nullptr;
    std::vector<std::pair<QAction*, Orientation>> orientationActions_;
    QTimer* trayRetry_ = nullptr;
    int trayRetryCount_ = 0;

    QWidget* window_ = nullptr;
    QLabel* windowStatus_ = nullptr;
    QCheckBox* windowScreen_ = nullptr;
    QSlider* windowBrightness_ = nullptr;
    QComboBox* windowOrientation_ = nullptr;
    QComboBox* windowTheme_ = nullptr;
    QCheckBox* windowAutostart_ = nullptr;
    bool updatingWindow_ = false;
};

#endif  // LCDCTL_WITH_QT


}  // namespace

#if defined(_WIN32)
// The GUI build is a "windows" subsystem executable: no console window appears.
// When it is started from a terminal (--once, --simulate, --headless), lend it
// the parent console so the output is visible.
static void attachParentConsole() {
    // A redirected stdout/stderr (pipe, file) is inherited as-is: keep it.
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE &&
        ::GetFileType(out) != FILE_TYPE_UNKNOWN) {
        return;
    }
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
}
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    attachParentConsole();
#endif
    try {
        const Options options = parseArgs(argc, argv);
        if (options.flag("--help")) {
            std::cout << kUsage;
            return 0;
        }
        if (options.flag("--verbose") || options.flag("-v")) {
            setLogLevel(LogLevel::Debug);
        }
        const Settings settings = loadSettings(options);

        if (options.flag("--simulate")) {
            return runSimulation(options, settings);
        }
        if (options.flag("--once") || options.flag("--dump-json")) {
            return runOnce(options, settings);
        }

#ifdef LCDCTL_WITH_QT
        // A desktop session exists on Windows/macOS by definition; on Linux it
        // is announced through DISPLAY/WAYLAND_DISPLAY.
        bool displayAvailable = true;
#if !defined(_WIN32) && !defined(__APPLE__)
        displayAvailable = !qEnvironmentVariableIsEmpty("DISPLAY") ||
                           !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
#endif
        if (!options.flag("--headless") && displayAvailable) {
            QApplication application(argc, argv);
            application.setApplicationName("lcd-monitor");
            application.setWindowIcon(makeTrayIcon());
            application.setQuitOnLastWindowClosed(false);

            // One instance only: a second launch asks the running one to show
            // its control window, then exits. Also protects the serial port.
            QSharedMemory instance("usb-monitor-controller-instance");
            if (!instance.create(16)) {
                if (instance.attach()) {
                    auto* bytes = static_cast<char*>(instance.data());
                    if (bytes != nullptr) {
                        bytes[0] = 1;   // "show yourself"
                    }
                    instance.detach();
                }
                return 0;
            }

            MonitorApp monitor(settings, options,
                               options.get("--config", defaultConfigPath()),
                               options.flag("--window"), &instance);
            QString error;
            if (!monitor.start(&error)) {
                std::cerr << "Error: " << error.toStdString() << std::endl;
                return 1;
            }
            return application.exec();
        }
#endif
        return runHeadless(options, settings);
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << std::endl;
        return 1;
    }
}
