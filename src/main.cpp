// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - lcd-cli command line tool.
//
// A small controller for the USB LCD screens supported by
// https://github.com/mathoudebine/turing-smart-screen-python/

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "lcdctl/drivers.hpp"
#include "lcdctl/factory.hpp"
#include "lcdctl/logging.hpp"
#include "lcdctl/transport.hpp"
#include "lcdctl/types.hpp"

namespace {

using lcdctl::Color;
using lcdctl::LcdComm;
using lcdctl::Revision;

const char* kUsage =
    "Usage: lcd-cli <command> [options]\n"
    "\n"
    "Commands:\n"
    "  list                          List detected screens (serial + native USB)\n"
    "  probe                         Open a screen and print what was detected\n"
    "  clear                         Clear the screen\n"
    "  on                            Turn the screen on\n"
    "  off                           Turn the screen off\n"
    "  brightness <0-100>            Set the brightness\n"
    "  led <r> <g> <b>               Set the backplate RGB LED (supported HW)\n"
    "  orientation <value>           portrait|landscape|reverse-portrait|reverse-landscape\n"
    "  text <text>                   Display text (see --x --y --size --color --background)\n"
    "  image <path>                  Display a PNG/JPEG image\n"
    "\n"
    "Options:\n"
    "  --revision <A|B|C|D|TUR_USB|WEACT_A|WEACT_B|AUTO>   default: AUTO\n"
    "  --port <device>               e.g. /dev/ttyACM0, COM5 (default: auto)\n"
    "  --width <px> --height <px>    portrait size (default: per revision)\n"
    "  --baud <rate>                 serial baud rate (default: 115200)\n"
    "  --reset                       send a hardware reset before initializing\n"
    "  --x <px> --y <px>             text/image position\n"
    "  --size <pt>                   font size (text)\n"
    "  --color <c> --background <c>  color name, #rrggbb or \"r,g,b\"\n"
    "  --font <path.ttf>             font file (default: bundled Roboto Mono)\n"
    "  --verbose, -v                 debug logging\n";

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
    static const std::vector<std::string> booleanFlags = {"--reset", "--verbose", "-v", "-h", "--help"};
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string token = argv[i];
        if (token.rfind("--", 0) == 0 || token == "-v" || token == "-h") {
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
    std::cout << "Opened " << lcdctl::revisionName(lcdctl::Revision::A) << std::endl;
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

int run(const Options& options) {
    if (options.flag("--help") || options.positional.empty()) {
        std::cout << kUsage;
        return options.positional.empty() ? 1 : 0;
    }

    const std::string command = options.positional[0];

    if (command == "list") {
        printDevices();
        return 0;
    }

    if (command == "probe") {
        auto lcd = openLcd(options);
        printProbe(*lcd);
        return 0;
    }

    auto lcd = openLcd(options);

    if (command == "clear") {
        lcd->clear();
    } else if (command == "on") {
        lcd->screenOn();
    } else if (command == "off") {
        lcd->screenOff();
    } else if (command == "brightness") {
        if (options.positional.size() < 2) {
            throw std::runtime_error("brightness requires a value");
        }
        lcd->setBrightness(std::stoi(options.positional[1]));
    } else if (command == "led") {
        if (options.positional.size() < 4) {
            throw std::runtime_error("led requires r g b");
        }
        const Color color{static_cast<std::uint8_t>(std::stoi(options.positional[1])),
                          static_cast<std::uint8_t>(std::stoi(options.positional[2])),
                          static_cast<std::uint8_t>(std::stoi(options.positional[3]))};
        lcd->setBackplateLedColor(color);
    } else if (command == "orientation") {
        if (options.positional.size() < 2) {
            throw std::runtime_error("orientation requires a value");
        }
        lcdctl::Orientation orientation = lcdctl::Orientation::Portrait;
        if (!lcdctl::parseOrientation(options.positional[1], orientation)) {
            throw std::runtime_error("invalid orientation: " + options.positional[1]);
        }
        lcd->setOrientation(orientation);
    } else if (command == "image") {
        if (options.positional.size() < 2) {
            throw std::runtime_error("image requires a path");
        }
        lcd->displayBitmap(options.positional[1], options.getInt("--x", 0), options.getInt("--y", 0));
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
        lcd->displayText(text, options.getInt("--x", 0), options.getInt("--y", 0), style);
    } else {
        std::cerr << "Unknown command: " << command << "\n\n" << kUsage;
        return 1;
    }

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
