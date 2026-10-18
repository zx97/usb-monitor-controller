// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - revision detection and driver factory.
//
// Replaces the `Display` class + config-driven branch of
// library/display.py of https://github.com/mathoudebine/turing-smart-screen-python/

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lcdctl/drivers.hpp"
#include "lcdctl/transport.hpp"

namespace lcdctl {

enum class Revision {
    A,
    B,
    C,
    D,
    TuringUsb,
    WeActA,
    WeActB,
};

const char* revisionName(Revision revision);
bool parseRevision(const std::string& text, Revision& out);
std::vector<std::string> revisionNames();

// Default portrait resolution per revision (0 = resolved by the device).
Size defaultSize(Revision revision);

struct DetectedDevice {
    Revision revision;
    std::string port;        // serial device, or "" for native USB
    int width = 0;
    int height = 0;
    std::string serial;      // USB serial number, for display
    bool nativeUsb = false;
};

// Enumerate every supported screen currently connected: native-USB models
// first, then serial screens, with a best-effort revision guess.
std::vector<DetectedDevice> detectDevices();

// Instantiate the driver for `revision`. For serial revisions, `port` is a
// device path ("" or "AUTO" => auto-detect). For TuringUsb, `port` is ignored.
// `width`/`height` of 0 fall back to defaultSize() (or are auto-detected for
// native USB).
// Throws std::runtime_error when the device cannot be opened.
std::unique_ptr<LcdComm> createLcd(Revision revision, const std::string& port,
                                   int width = 0, int height = 0, int baud = 115200);

}  // namespace lcdctl
