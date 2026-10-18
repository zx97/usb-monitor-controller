// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/factory.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>

#include "lcdctl/logging.hpp"

namespace lcdctl {
namespace {

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

// Best-effort revision guess from a serial port's USB identifiers, mirroring
// the `auto_detect_com_port()` of each Python driver.
std::optional<Revision> guessRevision(const SerialPortInfo& port) {
    const std::string& serial = port.serial;

    if (port.vid == 0x1a86 && port.pid == 0x5722) {  // QinHeng, used by several models
        if (serial == "2017-2-25") {
            return Revision::B;  // XuanFang rev. B / flagship
        }
        if (serial == "USB7INCH" || serial == "CT21INCH" || serial == "CT88INCH") {
            return Revision::C;  // Turing 5" / 2.x" / 8.8" (sleeping)
        }
        return Revision::A;      // Turing 3.5" / UsbPCMonitor
    }
    if (port.vid == 0x1a86 && port.pid == 0xca21) {
        return Revision::C;  // Turing rev. C, sleeping
    }
    if (port.vid == 0x0525 && port.pid == 0xa4a7) {
        return Revision::C;  // Turing rev. C, awake (Linux USB gadget)
    }
    if (port.vid == 0x1d6b && (port.pid == 0x0121 || port.pid == 0x0106)) {
        return Revision::C;  // Turing rev. C, awake (gadget)
    }
    if (port.vid == 0x454d && port.pid == 0x4e41) {
        return Revision::D;  // Kipye Qiye Smart Display
    }
    if (port.vid == 0x1a86 && port.pid == 0xfe0c) {  // WeAct: same PID for both sizes
        if (serial.rfind("AB", 0) == 0) {
            return Revision::WeActA;
        }
        if (serial.rfind("AD", 0) == 0) {
            return Revision::WeActB;
        }
        return Revision::WeActA;
    }
    return std::nullopt;
}

std::string findPortForRevision(Revision revision) {
    for (const SerialPortInfo& port : SerialTransport::enumerate()) {
        const std::optional<Revision> guess = guessRevision(port);
        if (guess && *guess == revision) {
            return port.device;
        }
    }
    return "";
}

}  // namespace

const char* revisionName(Revision revision) {
    switch (revision) {
        case Revision::A:       return "A";
        case Revision::B:       return "B";
        case Revision::C:       return "C";
        case Revision::D:       return "D";
        case Revision::TuringUsb:return "TUR_USB";
        case Revision::WeActA:   return "WEACT_A";
        case Revision::WeActB:   return "WEACT_B";
    }
    return "UNKNOWN";
}

bool parseRevision(const std::string& text, Revision& out) {
    const std::string value = upper(text);
    if (value == "A") {
        out = Revision::A;
    } else if (value == "B") {
        out = Revision::B;
    } else if (value == "C") {
        out = Revision::C;
    } else if (value == "D") {
        out = Revision::D;
    } else if (value == "TUR_USB" || value == "TURING_USB" || value == "TURINGUSB" || value == "USB") {
        out = Revision::TuringUsb;
    } else if (value == "WEACT_A" || value == "WEACT-A") {
        out = Revision::WeActA;
    } else if (value == "WEACT_B" || value == "WEACT-B") {
        out = Revision::WeActB;
    } else {
        return false;
    }
    return true;
}

std::vector<std::string> revisionNames() {
    return {"A", "B", "C", "D", "TUR_USB", "WEACT_A", "WEACT_B"};
}

Size defaultSize(Revision revision) {
    switch (revision) {
        case Revision::A:        return {320, 480};
        case Revision::B:        return {320, 480};
        case Revision::C:        return {480, 800};  // theme dependent
        case Revision::D:        return {320, 480};
        case Revision::WeActA:   return {320, 480};
        case Revision::WeActB:   return {80, 160};
        case Revision::TuringUsb:return {480, 480};  // resolved from the USB PID
    }
    return {320, 480};
}

std::vector<DetectedDevice> detectDevices() {
    std::vector<DetectedDevice> devices;

    // Native USB screens first.
    for (const UsbDeviceInfo& usb : UsbTransport::enumerate()) {
        DetectedDevice device;
        device.revision = Revision::TuringUsb;
        device.width = usb.width;
        device.height = usb.height;
        device.nativeUsb = true;
        devices.push_back(device);
    }

    // Serial screens.
    for (const SerialPortInfo& port : SerialTransport::enumerate()) {
        const std::optional<Revision> guess = guessRevision(port);
        if (!guess) {
            continue;
        }
        DetectedDevice device;
        device.revision = *guess;
        device.port = port.device;
        device.serial = port.serial;
        const Size size = defaultSize(*guess);
        device.width = size.width;
        device.height = size.height;
        devices.push_back(device);
    }

    return devices;
}

std::unique_ptr<LcdComm> createLcd(Revision revision, const std::string& port, int width,
                                   int height, int baud) {
    if (revision == Revision::TuringUsb) {
        if (!LcdCommTuringUSB::isAvailable()) {
            throw std::runtime_error(
                "native USB (TUR_USB) support is not available: rebuild with libusb");
        }
        return std::make_unique<LcdCommTuringUSB>();
    }

    std::string device = port;
    if (device.empty() || upper(device) == "AUTO") {
        device = findPortForRevision(revision);
        if (device.empty()) {
            throw std::runtime_error(
                std::string("no connected screen matching revision ") + revisionName(revision));
        }
        LCDCTL_INFO << "Auto-detected port " << device << " for revision "
                    << revisionName(revision);
    }

    Size size{width, height};
    if (size.width == 0 || size.height == 0) {
        size = defaultSize(revision);
    }

    auto transport = std::make_unique<SerialTransport>();
    if (!transport->open(device, baud)) {
        throw std::runtime_error("cannot open serial port " + device);
    }

    switch (revision) {
        case Revision::A:
            return std::make_unique<LcdCommRevA>(std::move(transport), size.width, size.height);
        case Revision::B:
            return std::make_unique<LcdCommRevB>(std::move(transport), size.width, size.height);
        case Revision::C:
            return std::make_unique<LcdCommRevC>(std::move(transport), size.width, size.height);
        case Revision::D:
            return std::make_unique<LcdCommRevD>(std::move(transport), size.width, size.height);
        case Revision::WeActA:
            return std::make_unique<LcdCommWeActA>(std::move(transport));
        case Revision::WeActB:
            return std::make_unique<LcdCommWeActB>(std::move(transport));
        case Revision::TuringUsb:
            break;  // handled above
    }
    throw std::runtime_error("unsupported revision");
}

}  // namespace lcdctl
