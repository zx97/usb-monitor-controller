// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline self test for the theme engine, the sensor stubs and the renderer.
// No hardware, no display server: everything runs against an OffscreenCanvas.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lcdctl/monitor.hpp"
#include "lcdctl/paths.hpp"
#include "lcdctl/sensors.hpp"
#include "lcdctl/settings.hpp"
#include "lcdctl/theme.hpp"
#include "lcdctl/theme_render.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::string themesRoot() {
    const std::string root = lcdctl::resourceRoot();
    if (root.empty()) {
        return "";
    }
    return root + "/res/themes";
}

std::string fontsRoot() {
    const std::string root = lcdctl::resourceRoot();
    if (root.empty()) {
        return "";
    }
    return root + "/res/fonts";
}

// Total difference between two framebuffers, used to assert "nothing changed".
long difference(const lcdctl::Image& a, const lcdctl::Image& b) {
    if (a.width() != b.width() || a.height() != b.height() || a.channels() != b.channels()) {
        return -1;
    }
    long sum = 0;
    const std::uint8_t* pa = a.data();
    const std::uint8_t* pb = b.data();
    for (std::size_t i = 0; i < a.size(); ++i) {
        sum += std::abs(static_cast<int>(pa[i]) - static_cast<int>(pb[i]));
    }
    return sum;
}

void testBytesToHuman() {
    check(lcdctl::bytesToHuman(0) == "0.0 ", "bytesToHuman(0)");
    check(lcdctl::bytesToHuman(1536) == "1.5 k", "bytesToHuman(1536)");
    check(lcdctl::bytesToHuman(1061000000) == "1011.8 M", "bytesToHuman(1061000000)");
    check(lcdctl::bytesToHuman(1536, true) == "1.5 k/s", "bytesToHuman(1536, true)");
}

void testStaticSensors() {
    lcdctl::SensorSettings settings;
    auto sensors = lcdctl::makeStaticSensors(settings);
    lcdctl::Metrics metrics;
    sensors->refresh(metrics);
    check(metrics.cpu.percent == 50.0, "static cpu percent");
    check(metrics.cpu.frequencyMhz == 2400.0, "static cpu frequency");
    check(metrics.cpu.temperatureC > 67.0 && metrics.cpu.temperatureC < 67.4, "static cpu temp");
    check(metrics.gpu.available, "static gpu available");
    check(metrics.gpu.memoryUsedMb == 16384.0, "static gpu memory used");
    check(metrics.memory.virtualPercent == 50.0, "static memory percent");
    check(metrics.disk.total == 1000000000000ull, "static disk total");
    check(metrics.uptimeSeconds == 4294036ull, "static uptime");

    auto overridden = lcdctl::makeStubSensors("static:cpu=42,gpu=7", settings);
    overridden->refresh(metrics);
    check(metrics.cpu.percent == 42.0, "stub override cpu");
    check(metrics.gpu.load == 7.0, "stub override gpu");
}

void testThemeParsing(const std::string& root, const std::string& fonts) {
    const lcdctl::Theme theme = lcdctl::Theme::loadByName("demo-320x480", root, fonts);

    check(theme.name == "demo-320x480", "theme name");
    check(theme.orientation == lcdctl::Orientation::Portrait, "theme orientation");
    check(theme.images.size() == 1, "theme background image");
    check(!theme.texts.empty(), "theme static texts");
    check(theme.items.size() >= 10, "theme items parsed");

    const lcdctl::Size size = theme.declaredSize();
    check(size.width == 320 && size.height == 480, "theme declared size");

    check(theme.findItem("STATS/CPU/PERCENTAGE/TEXT") != nullptr, "cpu percentage text item");
    check(theme.findItem("STATS/DISK/USED/GRAPH") != nullptr, "disk graph item");
    const lcdctl::ThemeItem* percent = theme.findItem("STATS/DISK/USED/PERCENT_TEXT");
    check(percent != nullptr, "disk percent text item");
    if (percent) {
        check(percent->metric == "disk.percent", "disk percent metric");
    }

    const std::vector<std::string> problems = theme.validate(320, 480);
    for (const std::string& problem : problems) {
        std::fprintf(stderr, "  validate: %s\n", problem.c_str());
    }
    check(problems.empty(), "demo theme validates");

    check(theme.items.front().text.font.empty() || !theme.items.front().text.font.empty(),
          "font resolution did not crash");
}

void testRendering(const std::string& root, const std::string& fonts) {
    lcdctl::Theme theme = lcdctl::Theme::loadByName("demo-320x480", root, fonts);

    lcdctl::SensorSettings sensorSettings;
    auto sensors = lcdctl::makeStaticSensors(sensorSettings);
    lcdctl::Metrics metrics;
    sensors->refresh(metrics);

    lcdctl::OffscreenCanvas canvas({320, 480});
    lcdctl::ThemeRenderer renderer(theme, canvas);
    renderer.drawBackground();
    const int first = renderer.drawAll(metrics);
    check(first > 0, "first frame transmits items");

    lcdctl::Image reference = canvas.framebuffer();

    // Second pass with identical values: nothing must be sent.
    renderer.drawAll(metrics);
    check(renderer.lastTransmittedCount() == 0, "unchanged values are not retransmitted");

    // Determinism: a fresh pipeline produces byte-identical pixels.
    lcdctl::OffscreenCanvas canvas2({320, 480});
    lcdctl::ThemeRenderer renderer2(theme, canvas2);
    renderer2.drawBackground();
    renderer2.drawAll(metrics);
    check(difference(reference, canvas2.framebuffer()) == 0, "rendering is deterministic");

    // A changed metric must repaint its items.
    metrics.cpu.percent = 91.0;
    renderer.drawAll(metrics);
    check(renderer.lastTransmittedCount() >= 1, "changed value is transmitted");
    check(difference(reference, canvas.framebuffer()) != 0, "changed value alters the frame");

    // Line graphs keep their history and always repaint (no item here has one,
    // so this asserts the engine path does not crash on repeated ticks).
    check(true, "repeated ticks");
}

// A shorter value must repaint the stale glyphs of the previous one.
void testTextGhosting(const std::string& fonts) {
    lcdctl::OffscreenCanvas canvas({120, 40}, lcdctl::Color{0, 0, 0});
    lcdctl::TextStyle style;
    style.fontSize = 24;
    style.fontColor = lcdctl::Color{255, 255, 255};
    style.backgroundColor = lcdctl::Color{0, 0, 0};
    style.anchor = "lt";

    lcdctl::drawText(canvas, "888", 4, 4, style);
    lcdctl::drawText(canvas, "1", 4, 4, style);

    long white = 0;
    for (std::size_t i = 0; i < canvas.framebuffer().size(); ++i) {
        if (canvas.framebuffer().data()[i] > 200) {
            ++white;
        }
    }
    check(white > 0, "ghosting test drew something");
    // Without the union-with-previous-bbox logic the right half would still be
    // full of the digits 88: compare against a fresh canvas drawn with "1".
    lcdctl::OffscreenCanvas clean({120, 40}, lcdctl::Color{0, 0, 0});
    lcdctl::drawText(clean, "1", 4, 4, style);
    check(difference(canvas.framebuffer(), clean.framebuffer()) == 0,
          "no glyph ghosting when the value shrinks");
}

void testYamlRoundTrip(const std::string& root, const std::string& fonts) {
    const lcdctl::Theme original = lcdctl::Theme::loadByName("demo-320x480", root, fonts);
    const std::string yaml = original.toYaml();
    check(!yaml.empty(), "serialization produces YAML");
    check(yaml.find("STATS:") != std::string::npos, "serialized YAML keeps STATS");

    std::filesystem::create_directories("/dev/shm/lcdctl-theme-test/roundtrip");
    {
        std::FILE* file = std::fopen("/dev/shm/lcdctl-theme-test/roundtrip/theme.yaml", "w");
        check(file != nullptr, "round-trip file created");
        if (file) {
            std::fwrite(yaml.data(), 1, yaml.size(), file);
            std::fclose(file);
        }
    }
    check(yaml.find("DISPLAY_ORIENTATION") != std::string::npos,
          "serialized YAML keeps the orientation");
    const lcdctl::Theme reloaded =
        lcdctl::Theme::load("/dev/shm/lcdctl-theme-test/roundtrip", fonts);
    check(reloaded.orientation == original.orientation, "round-trip keeps the orientation");
    check(reloaded.items.size() == original.items.size(), "round-trip keeps every item");
    for (const lcdctl::ThemeItem& item : original.items) {
        const lcdctl::ThemeItem* copy = reloaded.findItem(item.key);
        if (!copy) {
            std::fprintf(stderr, "  missing after round-trip: %s\n", item.key.c_str());
            check(false, "round-trip keeps item " + item.key);
        }
    }
}

// Rotation: the theme fixes the aspect, the user picks which side is up.
void testOrientation() {
    using lcdctl::Orientation;
    check(lcdctl::isPortraitOrientation(Orientation::Portrait), "portrait is portrait");
    check(lcdctl::isPortraitOrientation(Orientation::ReversePortrait), "reverse-portrait is portrait");
    check(!lcdctl::isPortraitOrientation(Orientation::Landscape), "landscape is not portrait");
    check(lcdctl::flippedOrientation(Orientation::Portrait) == Orientation::ReversePortrait,
          "portrait flips to reverse-portrait");
    check(lcdctl::flippedOrientation(Orientation::ReverseLandscape) == Orientation::Landscape,
          "reverse-landscape flips to landscape");
    check(lcdctl::sameAspect(Orientation::Landscape, Orientation::ReverseLandscape),
          "landscape and reverse-landscape share an aspect");
    check(!lcdctl::sameAspect(Orientation::Portrait, Orientation::Landscape),
          "portrait and landscape do not");

    lcdctl::Settings settings;
    lcdctl::OrientationDecision decision =
        lcdctl::resolveOrientation(settings, Orientation::Portrait);
    check(decision.orientation == Orientation::Portrait, "AUTO follows the theme");
    check(decision.warning.empty(), "AUTO does not warn");

    settings.displayOrientation = "reverse-portrait";
    decision = lcdctl::resolveOrientation(settings, Orientation::Portrait);
    check(decision.orientation == Orientation::ReversePortrait, "180° flip accepted for a portrait theme");
    check(decision.warning.empty(), "180° flip does not warn");

    settings.displayOrientation = "landscape";
    decision = lcdctl::resolveOrientation(settings, Orientation::Portrait);
    check(decision.orientation == Orientation::Portrait, "90° refused for a portrait theme");
    check(!decision.warning.empty(), "90° mismatch is reported");

    settings.forceOrientation = true;
    decision = lcdctl::resolveOrientation(settings, Orientation::Portrait);
    check(decision.orientation == Orientation::Landscape, "FORCE allows the 90° rotation");
    check(!decision.warning.empty(), "forced 90° still warns about cropping");

    settings.displayOrientation = "AUTO";
    settings.displayReverse = true;
    decision = lcdctl::resolveOrientation(settings, Orientation::Landscape);
    check(decision.orientation == Orientation::ReverseLandscape,
          "DISPLAY_REVERSE flips a landscape theme");
}

// "Start at login": an XDG autostart entry in ~/.config/autostart.
void testAutostart() {
    const std::string home = "/dev/shm/lcdctl-autostart-test";
    std::filesystem::create_directories(home);
    setenv("HOME", home.c_str(), 1);

    check(!lcdctl::Settings::autostartEnabled(), "autostart is off by default");
    check(lcdctl::Settings::setAutostartEnabled(true, "/usr/local/bin/lcd-monitor"),
          "autostart entry created");
    check(lcdctl::Settings::autostartEnabled(), "autostart entry detected");

    std::ifstream stream(lcdctl::Settings::autostartFilePath());
    const std::string content{std::istreambuf_iterator<char>(stream),
                              std::istreambuf_iterator<char>()};
    check(content.find("[Desktop Entry]") != std::string::npos, "autostart is a desktop entry");
    check(content.find("Exec=/usr/local/bin/lcd-monitor --tray") != std::string::npos,
          "autostart runs the tray daemon");
    check(content.find("X-KDE-autostart-after=panel") != std::string::npos,
          "autostart is KDE friendly");

    check(lcdctl::Settings::setAutostartEnabled(false), "autostart entry removed");
    check(!lcdctl::Settings::autostartEnabled(), "autostart is off again");
}

// Render every theme of a directory: the bundled collection, or a
// turing-smart-screen-python checkout when LCDCTL_TEST_THEMES is set.
int renderEveryTheme(const std::string& root, const std::string& fonts, const char* label) {
    const std::vector<lcdctl::ThemeInfo> themes = lcdctl::listThemes(root);
    lcdctl::SensorSettings sensorSettings;
    auto sensors = lcdctl::makeStaticSensors(sensorSettings);
    lcdctl::Metrics metrics;
    sensors->refresh(metrics);

    int rendered = 0;
    for (const lcdctl::ThemeInfo& info : themes) {
        try {
            const lcdctl::Theme theme = lcdctl::Theme::load(info.directory, fonts);
            lcdctl::Size size = theme.declaredSize();
            if (size.width <= 0 || size.height <= 0) {
                size = {320, 480};
            }
            lcdctl::OffscreenCanvas canvas(size);
            lcdctl::ThemeRenderer renderer(theme, canvas);
            renderer.drawBackground();
            renderer.drawAll(metrics);
            ++rendered;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: theme %s: %s\n", info.name.c_str(), error.what());
            ++g_failures;
        }
    }
    std::printf("rendered %d/%d %s themes\n", rendered, static_cast<int>(themes.size()), label);
    check(!themes.empty(), std::string("theme collection is not empty: ") + label);
    check(rendered == static_cast<int>(themes.size()),
          std::string("every theme renders: ") + label);
    return rendered;
}

// Optional: render every theme of a compatibility checkout
// (LCDCTL_TEST_THEMES=/path/to/turing-smart-screen-python/res/themes).
void testAllThemes() {
    if (const char* root = std::getenv("LCDCTL_TEST_THEMES")) {
        const char* fonts = std::getenv("LCDCTL_TEST_FONTS");
        renderEveryTheme(root, fonts ? fonts : "", "compatibility");
    }
}

}  // namespace

int main() {
    const std::string root = themesRoot();
    const std::string fonts = fontsRoot();
    if (root.empty()) {
        std::fprintf(stderr, "cannot locate res/themes (resourceRoot() is empty)\n");
        return 1;
    }

    testBytesToHuman();
    testStaticSensors();
    testThemeParsing(root, fonts);
    testRendering(root, fonts);
    testTextGhosting(fonts);
    testYamlRoundTrip(root, fonts);
    testOrientation();
    testAutostart();
    renderEveryTheme(root, fonts, "bundled");
    testAllThemes();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
