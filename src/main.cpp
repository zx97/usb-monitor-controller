// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - lcd-cli command line tool.
//
// A small controller for the USB LCD screens supported by
// https://github.com/mathoudebine/turing-smart-screen-python/

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "lcdctl/console.hpp"
#include "lcdctl/drivers.hpp"
#include "lcdctl/factory.hpp"
#include "lcdctl/image.hpp"
#include "lcdctl/logging.hpp"
#include "lcdctl/simulate.hpp"
#include "lcdctl/transport.hpp"
#include "lcdctl/types.hpp"

namespace {

using lcdctl::Color;
using lcdctl::Image;
using lcdctl::LcdComm;
using lcdctl::Revision;

const char* kUsage =
    "Usage: lcd-cli <command> [options]\n"
    "\n"
    "Commands:\n"
    "  list                          List detected screens (serial + native USB)\n"
    "  probe                         Open a screen and print what was detected\n"
    "  clear                         Clear the screen (blank white; --color to pick)\n"
    "  on                            Turn the screen on\n"
    "  off                           Turn the screen off\n"
    "  brightness <0-100>            Set the brightness\n"
    "  led <r> <g> <b>               Set the backplate RGB LED (supported HW)\n"
    "  orientation <value>           portrait|landscape|reverse-portrait|reverse-landscape\n"
    "  text <text>                   Display text (see --x --y --size --color --background)\n"
    "  image <path>                  Display a PNG/JPEG image\n"
    "  console                       Scrolling text console / teleprompter (white on black,\n"
    "                                anti-aliased); reads stdin, or --file, see below\n"
    "\n"
    "Console options:\n"
    "  --file <path>                 read lines from a file instead of stdin\n"
    "  --interval <s>                prompter mode: advance automatically every <s> seconds\n"
    "  --lines-per-tick <n>          lines added per tick (default 1)\n"
    "  --loop                        restart the file when it ends\n"
    "  --wrap                        wrap long lines instead of clipping them\n"
    "  --gap <px>                    extra spacing between lines (default 2)\n"
    "  The area defaults to the whole screen from (--x,--y); use --width/--height to\n"
    "  shrink the redrawn area (a smaller area scrolls much faster).\n"
    "\n"
    "Options:\n"
    "  --revision <A|B|C|D|TUR_USB|WEACT_A|WEACT_B|AUTO>   default: AUTO\n"
    "  --port <device>               e.g. /dev/ttyACM0, COM5 (default: auto)\n"
    "  --width <px> --height <px>    portrait size (default: per revision)\n"
    "  --baud <rate>                 serial baud rate (default: 115200)\n"
    "  --reset                       send a hardware reset before initializing\n"
    "  --x <px> --y <px>             text/image position\n"
    "  --orientation <value>         portrait|landscape|reverse-portrait|reverse-landscape\n"
    "  --size <pt>                   font size (text)\n"
    "  --color <c> --background <c>  color name, #rrggbb or \"r,g,b\"\n"
    "  --font <path.ttf>             font file (default: bundled Roboto Mono)\n"
    "  --verbose, -v                 debug logging\n"
    "\n"
    "Simulation (no hardware):\n"
    "  --simulate                    run the driver offline and render the result\n"
    "  --output <file.png>           PNG to write in simulation mode (default: screencap.png)\n"
    "\n"
    "In simulation mode an explicit --revision is required, e.g.:\n"
    "  lcd-cli image logo.png --simulate --revision C --width 480 --height 800 -o out.png\n";

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
        if (it == values.end() || it->second.empty()) {
            return fallback;
        }
        return std::stoi(it->second);
    }
};

Options parseArgs(int argc, char** argv) {
    static const std::vector<std::string> booleanFlags = {"--reset", "--simulate", "--verbose",
                                                          "--wrap", "--loop", "-v", "-h", "--help"};
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string token = argv[i];
        // Short aliases.
        if (token == "-o") {
            token = "--output";
        } else if (token == "-v") {
            token = "--verbose";
        } else if (token == "-h") {
            token = "--help";
        }
        if (token.rfind("--", 0) == 0) {
            const std::size_t equal = token.find('=');
            if (equal != std::string::npos) {
                options.values[token.substr(0, equal)] = token.substr(equal + 1);
                continue;
            }
            const bool isBoolean =
                std::find(booleanFlags.begin(), booleanFlags.end(), token) != booleanFlags.end();
            if (isBoolean || i + 1 >= argc) {
                options.values[token] = "true";
            } else {
                options.values[token] = argv[++i];
            }
        } else {
            options.positional.push_back(token);
        }
    }
    return options;
}

std::unique_ptr<LcdComm> openLcd(const Options& options) {
    Revision revision = Revision::A;
    const std::string requested = options.get("--revision", "AUTO");
    if (requested != "AUTO" && !lcdctl::parseRevision(requested, revision)) {
        throw std::runtime_error("unknown revision: " + requested);
    }

    if (requested == "AUTO") {
        const std::vector<lcdctl::DetectedDevice> devices = lcdctl::detectDevices();
        if (devices.empty()) {
            throw std::runtime_error("no supported screen detected");
        }
        revision = devices.front().revision;
        LCDCTL_INFO << "Auto-detected revision " << lcdctl::revisionName(revision)
                    << (devices.front().port.empty() ? "" : " on " + devices.front().port);
        if (devices.size() > 1) {
            LCDCTL_WARN << devices.size() << " screens detected, using the first one";
        }
    }

    auto lcd = lcdctl::createLcd(revision, options.get("--port", ""), options.getInt("--width", 0),
                                 options.getInt("--height", 0), options.getInt("--baud", 115200));
    if (options.flag("--reset")) {
        lcd->reset();
    }
    lcd->initializeComm();
    return lcd;
}

// Instantiate a driver backed by a simulated transport (no hardware).
lcdctl::SimulatedLcd openSimulated(const Options& options) {
    Revision revision = Revision::A;
    const std::string requested = options.get("--revision", "");
    if (requested.empty() || requested == "AUTO") {
        throw std::runtime_error(
            "--simulate requires an explicit --revision (A/B/C/D/WEACT_A/WEACT_B)");
    }
    if (!lcdctl::parseRevision(requested, revision)) {
        throw std::runtime_error("unknown revision: " + requested);
    }
    if (!lcdctl::simulationSupported(revision)) {
        throw std::runtime_error("revision " + requested + " cannot be simulated");
    }
    auto simulated = lcdctl::createSimulatedLcd(revision, options.getInt("--width", 0),
                                                options.getInt("--height", 0));
    simulated.lcd->initializeComm();
    return simulated;
}

void printDevices() {
    std::cout << "Serial (USB CDC) ports:" << std::endl;
    bool anySerial = false;
    for (const lcdctl::SerialPortInfo& port : lcdctl::SerialTransport::enumerate()) {
        anySerial = true;
        std::printf("  %-22s VID:PID=%04x:%04x serial=%s\n", port.device.c_str(), port.vid,
                    port.pid, port.serial.c_str());
    }
    if (!anySerial) {
        std::cout << "  (none)" << std::endl;
    }

    std::cout << "\nNative USB (TUR_USB) devices:" << std::endl;
    if (!lcdctl::UsbTransport::available()) {
        std::cout << "  (libusb support not built)" << std::endl;
    } else {
        const std::vector<lcdctl::UsbDeviceInfo> usb = lcdctl::UsbTransport::enumerate();
        if (usb.empty()) {
            std::cout << "  (none)" << std::endl;
        }
        for (const lcdctl::UsbDeviceInfo& device : usb) {
            std::printf("  VID:PID=%04x:%04x portrait=%dx%d\n", lcdctl::TUR_USB_VENDOR_ID,
                        device.pid, device.width, device.height);
        }
    }
}

void printProbe(LcdComm& lcd) {
    const char* className = "LcdComm";
    if (dynamic_cast<lcdctl::LcdCommRevA*>(&lcd)) {
        className = "LcdCommRevA";
    } else if (dynamic_cast<lcdctl::LcdCommRevB*>(&lcd)) {
        className = "LcdCommRevB";
    } else if (dynamic_cast<lcdctl::LcdCommRevC*>(&lcd)) {
        className = "LcdCommRevC";
    } else if (dynamic_cast<lcdctl::LcdCommRevD*>(&lcd)) {
        className = "LcdCommRevD";
    } else if (dynamic_cast<lcdctl::LcdCommWeActA*>(&lcd)) {
        className = "LcdCommWeActA";
    } else if (dynamic_cast<lcdctl::LcdCommWeActB*>(&lcd)) {
        className = "LcdCommWeActB";
    } else if (dynamic_cast<lcdctl::LcdCommTuringUSB*>(&lcd)) {
        className = "LcdCommTuringUSB";
    }
    std::cout << "Opened " << className << std::endl;
    std::cout << "  width  = " << lcd.getWidth() << std::endl;
    std::cout << "  height = " << lcd.getHeight() << std::endl;
    if (auto* revA = dynamic_cast<lcdctl::LcdCommRevA*>(&lcd)) {
        std::cout << "  sub-revision = " << revA->subRevision() << std::endl;
    } else if (auto* revB = dynamic_cast<lcdctl::LcdCommRevB*>(&lcd)) {
        std::cout << "  sub-revision = 0x" << std::hex << revB->subRevision() << std::dec
                  << (revB->isFlagship() ? " (flagship)" : "") << std::endl;
    } else if (auto* revC = dynamic_cast<lcdctl::LcdCommRevC*>(&lcd)) {
        std::cout << "  sub-revision = " << revC->subRevision() << ", ROM = " << revC->romVersion()
                  << std::endl;
    } else if (auto* weact = dynamic_cast<lcdctl::LcdCommWeAct*>(&lcd)) {
        std::cout << "  firmware = " << weact->deviceVersion() << std::endl;
    } else if (auto* usb = dynamic_cast<lcdctl::LcdCommTuringUSB*>(&lcd)) {
        std::cout << "  USB product id = 0x" << std::hex << usb->productId() << std::dec
                  << std::endl;
    }
}

// Apply the optional --orientation before running a command.
void applyOrientation(LcdComm& lcd, const Options& options) {
    const std::string value = options.get("--orientation", "");
    if (value.empty()) {
        return;
    }
    lcdctl::Orientation orientation = lcdctl::Orientation::Portrait;
    if (!lcdctl::parseOrientation(value, orientation)) {
        throw std::runtime_error("invalid --orientation: " + value);
    }
    lcd.setOrientation(orientation);
}

// Execute a command against an already-initialized display.
void performAction(LcdComm& lcd, const std::string& command, const Options& options) {
    if (command == "clear") {
        const std::string color = options.get("--color", "");
        if (color.empty()) {
            lcd.clear();
        } else {
            lcd.fillScreen(lcdctl::parseColor(color));
        }
    } else if (command == "on") {
        lcd.screenOn();
    } else if (command == "off") {
        lcd.screenOff();
    } else if (command == "brightness") {
        if (options.positional.size() < 2) {
            throw std::runtime_error("brightness requires a value");
        }
        lcd.setBrightness(std::stoi(options.positional[1]));
    } else if (command == "led") {
        if (options.positional.size() < 4) {
            throw std::runtime_error("led requires r g b");
        }
        const Color color{static_cast<std::uint8_t>(std::stoi(options.positional[1])),
                          static_cast<std::uint8_t>(std::stoi(options.positional[2])),
                          static_cast<std::uint8_t>(std::stoi(options.positional[3]))};
        lcd.setBackplateLedColor(color);
    } else if (command == "orientation") {
        if (options.positional.size() < 2) {
            throw std::runtime_error("orientation requires a value");
        }
        lcdctl::Orientation orientation = lcdctl::Orientation::Portrait;
        if (!lcdctl::parseOrientation(options.positional[1], orientation)) {
            throw std::runtime_error("invalid orientation: " + options.positional[1]);
        }
        lcd.setOrientation(orientation);
    } else if (command == "image") {
        if (options.positional.size() < 2) {
            throw std::runtime_error("image requires a path");
        }
        lcd.displayBitmap(options.positional[1], options.getInt("--x", 0), options.getInt("--y", 0));
    } else if (command == "text") {
        std::string text;
        for (std::size_t i = 1; i < options.positional.size(); ++i) {
            if (i > 1) text += " ";
            text += options.positional[i];
        }
        lcdctl::TextStyle style;
        style.fontSize = options.getInt("--size", 20);
        style.font = options.get("--font", "");
        style.fontColor = lcdctl::parseColor(options.get("--color", "black"));
        style.backgroundColor = lcdctl::parseColor(options.get("--background", "white"));
        style.align = options.get("--align", "left");
        lcd.displayText(text, options.getInt("--x", 0), options.getInt("--y", 0), style);
    } else if (command == "console") {
        const bool simulate = options.flag("--simulate");

        lcdctl::TextConsole::Options consoleOptions;
        consoleOptions.x = options.getInt("--x", 0);
        consoleOptions.y = options.getInt("--y", 0);
        consoleOptions.width = options.getInt("--width", lcd.getWidth() - consoleOptions.x);
        consoleOptions.height = options.getInt("--height", lcd.getHeight() - consoleOptions.y);
        consoleOptions.font = options.get("--font", "");
        consoleOptions.fontSize = options.getInt("--size", 14);
        consoleOptions.lineGap = options.getInt("--gap", 2);
        // White on black, anti-aliased (FreeType coverage blending).
        consoleOptions.textColor = lcdctl::parseColor(options.get("--color", "white"));
        consoleOptions.background = lcdctl::parseColor(options.get("--background", "black"));
        consoleOptions.wrap = options.flag("--wrap");

        lcdctl::TextConsole console(lcd, consoleOptions);
        console.clear();

        if (!simulate) {
            // Rough link budget: ~122 kB/s (RGB565, 2 bytes/pixel) on revision A.
            const double areaBytes =
                static_cast<double>(consoleOptions.width) * console.capacity() *
                console.lineHeight() * 2;
            std::fprintf(stderr,
                         "console: %d lines of %d px, full redraw ~%.1f s per line when full\n",
                         console.capacity(), console.lineHeight(), areaBytes / 122000.0);
        }

        const std::string file = options.get("--file", "");
        std::ifstream fileStream;
        std::istream* input = &std::cin;
        if (!file.empty()) {
            fileStream.open(file);
            if (!fileStream) {
                throw std::runtime_error("cannot open " + file);
            }
            input = &fileStream;
        }

        const double interval = std::stod(options.get("--interval", "0"));
        const int perTick = std::max(1, options.getInt("--lines-per-tick", 1));
        const bool loop = options.flag("--loop");

        if (simulate || interval > 0) {
            // Read everything first (offline preview / paced prompter).
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(*input, line)) {
                lines.push_back(line);
            }
            if (interval <= 0) {
                for (const std::string& text : lines) {
                    console.addLine(text);
                }
            } else {
                do {
                    for (std::size_t i = 0; i < lines.size(); i += perTick) {
                        const std::size_t end = std::min(lines.size(), i + perTick);
                        for (std::size_t j = i; j < end; ++j) {
                            console.addLine(lines[j]);
                        }
                        if (!simulate) {
                            std::this_thread::sleep_for(std::chrono::duration<double>(interval));
                        }
                    }
                } while (loop && !simulate);
            }
        } else {
            // Live streaming: e.g. `tail -f app.log | lcd-cli console`.
            std::string line;
            while (std::getline(*input, line)) {
                console.addLine(line);
            }
        }
    } else {
        throw std::runtime_error("unknown command: " + command);
    }
}

int run(const Options& options) {
    if (options.flag("--help")) {
        std::cout << kUsage;
        return 0;
    }
    if (options.positional.empty()) {
        std::cerr << kUsage;
        return 1;
    }

    const std::string command = options.positional[0];

    if (command == "list") {
        printDevices();
        return 0;
    }

    if (command == "probe") {
        if (options.flag("--simulate")) {
            auto simulated = openSimulated(options);
            printProbe(*simulated.lcd);
            return 0;
        }
        auto lcd = openLcd(options);
        printProbe(*lcd);
        return 0;
    }

    if (options.flag("--simulate")) {
        // Offline: drive the real encoder, then decode the captured bytes back
        // into a framebuffer and write it to a PNG.
        auto simulated = openSimulated(options);
        applyOrientation(*simulated.lcd, options);
        performAction(*simulated.lcd, command, options);

        const int width = simulated.lcd->getWidth();
        const int height = simulated.lcd->getHeight();
        const Image framebuffer = lcdctl::decodeCapturedStream(
            simulated.revision, simulated.transport->captured(), width, height);
        const std::string output = options.get("--output", "screencap.png");
        lcdctl::savePng(output, framebuffer);

        std::cout << "OK: " << command << " (simulated " << lcdctl::revisionName(simulated.revision)
                  << " " << width << "x" << height << ", " << simulated.transport->captured().size()
                  << " bytes captured) -> " << output << std::endl;
        return 0;
    }

    auto lcd = openLcd(options);
    applyOrientation(*lcd, options);
    performAction(*lcd, command, options);
    std::cout << "OK: " << command << std::endl;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseArgs(argc, argv);
        if (options.flag("--verbose") || options.flag("-v")) {
            lcdctl::setLogLevel(lcdctl::LogLevel::Debug);
        }
        return run(options);
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << std::endl;
        return 1;
    }
}
