// SPDX-License-Identifier: GPL-3.0-or-later
//
// Revision B: XuanFang 3.5" (incl. flagship) screens.
// Ported from library/lcd/lcd_comm_rev_b.py of
// https://github.com/mathoudebine/turing-smart-screen-python/
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include "lcdctl/drivers.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

#include "lcdctl/logging.hpp"
#include "lcdctl/serialize.hpp"

namespace lcdctl {
namespace {

// Command ids of the rev B protocol (framed 10-byte packets).
enum RevBCommand : int {
    CmdHello = 0xCA,           // Establish communication before driving the screen
    CmdSetOrientation = 0xCB,  // Sets the screen orientation
    CmdDisplayBitmap = 0xCC,   // Displays an image on the screen
    CmdSetLighting = 0xCD,     // Sets the screen backplate RGB LED color
    CmdSetBrightness = 0xCE,   // Sets the screen brightness
};

// Basic orientations are managed by the display (reverse ones are software).
enum RevBOrientation : std::uint8_t {
    OrientationPortrait = 0x0,
    OrientationLandscape = 0x1,
};

// HW sub-revisions returned by HELLO; they describe the screen capabilities.
enum RevBSubRevision : int {
    SubRevisionA01 = 0xA01,  // HW revision B - brightness 0/1
    SubRevisionA02 = 0xA02,  // HW revision "flagship" - brightness 0/1
    SubRevisionA11 = 0xA11,  // HW revision B - brightness 0-255
    SubRevisionA12 = 0xA12,  // HW revision "flagship" - brightness 0-255
};

// Default brightness percentage used by ScreenOn(), mirroring the Python default.
constexpr int kDefaultBrightness = 25;

// Cooldown between two bitmaps to reduce "corrupted bitmap" occurrences.
constexpr auto kBitmapCooldown = std::chrono::milliseconds(50);

// Crop the image to the requested size and, for the software-managed reverse
// orientations, rotate it 180 degrees before serialization.
Image serializeImage(const Image& image, int height, int width, Orientation orientation) {
    Image result = image;
    if (result.width() != width || result.height() != height) {
        result = result.crop(0, 0, width, height);
    }
    if (orientation == Orientation::ReversePortrait ||
        orientation == Orientation::ReverseLandscape) {
        result = result.rotate(180);
    }
    return result;
}

}  // namespace

LcdCommRevB::LcdCommRevB(std::unique_ptr<Transport> transport, int displayWidth,
                         int displayHeight)
    : LcdComm(std::move(transport), displayWidth, displayHeight) {}

// New protocol: 10-byte packets framed with the command, 8 data bytes inside.
void LcdCommRevB::sendCommand(int cmd, const std::vector<std::uint8_t>& payload,
                              bool /*bypassQueue*/) {
    std::vector<std::uint8_t> buffer(10, 0);
    buffer[0] = static_cast<std::uint8_t>(cmd);
    const std::size_t count = std::min<std::size_t>(payload.size(), 8);
    for (std::size_t i = 0; i < count; ++i) {
        buffer[1 + i] = payload[i];
    }
    buffer[9] = static_cast<std::uint8_t>(cmd);
    writeData(buffer);
}

void LcdCommRevB::hello() {
    const std::vector<std::uint8_t> hello = {'H', 'E', 'L', 'L', 'O'};

    // This command reads the LCD answer on the serial link, so it bypasses the queue.
    sendCommand(CmdHello, hello, true);
    const std::vector<std::uint8_t> response = readData(10);
    flushInput();

    if (response.size() != 10) {
        LCDCTL_WARN << "Device not recognised (short response to HELLO)";
    }
    if (response.empty()) {
        LCDCTL_WARN << "Device did not return anything";
        return;
    }
    if (response.front() != static_cast<std::uint8_t>(CmdHello) ||
        response.back() != static_cast<std::uint8_t>(CmdHello)) {
        LCDCTL_WARN << "Device not recognised (bad framing)";
    }
    if (response.size() >= 6 &&
        std::vector<std::uint8_t>(response.begin() + 1, response.begin() + 6) != hello) {
        LCDCTL_WARN << "Device not recognised (no HELLO answer)";
    }

    // The HELLO response carries 2 bytes describing the screen version (not the
    // revision, which is B/flagship). It tells which capabilities the screen offers.
    if (response.size() >= 8 && response[6] == 0x0A) {
        switch (response[7]) {
            case 0x01: subRevision_ = SubRevisionA01; break;
            case 0x02: subRevision_ = SubRevisionA02; break;
            case 0x11: subRevision_ = SubRevisionA11; break;
            case 0x12: subRevision_ = SubRevisionA12; break;
            default:
                LCDCTL_WARN << "Display returned unknown sub-revision on HELLO answer";
                break;
        }
    }

    LCDCTL_DEBUG << "HW sub-revision: 0x" << std::hex << subRevision_ << std::dec;
}

void LcdCommRevB::initializeComm() { hello(); }

void LcdCommRevB::reset() {
    // HW revision B does not implement a reset command: clear the display instead.
    clear();
}

void LcdCommRevB::clear() {
    // HW revision B does not implement a Clear command: display a blank image on
    // the whole screen. Force an orientation in case the screen currently uses one
    // different from the theme.
    const Orientation backupOrientation = orientation_;
    setOrientation(Orientation::Portrait);

    const Image blank(getWidth(), getHeight(), 3, 255);  // RGB white
    displayPILImage(blank);

    setOrientation(backupOrientation);
}

void LcdCommRevB::screenOff() {
    // No native "ScreenOff": use SetBrightness(0) instead.
    setBrightness(0);
}

void LcdCommRevB::screenOn() {
    // No native "ScreenOn": use the default brightness instead.
    setBrightness(kDefaultBrightness);
}

void LcdCommRevB::setBrightness(int level) {
    if (level < 0 || level > 100) {
        throw std::invalid_argument("brightness level must be within [0, 100]");
    }

    int convertedLevel = 0;
    if (isBrightnessRange()) {
        // Brightness scales from 0 to 255, 255 being the brightest and 0 the darkest.
        convertedLevel = static_cast<int>((level / 100.0) * 255);
    } else {
        // Brightness is only 0 (full brightness) or 1 (off).
        LCDCTL_INFO << "Your display does not support custom brightness level";
        convertedLevel = (level == 0) ? 1 : 0;
    }

    sendCommand(CmdSetBrightness, {static_cast<std::uint8_t>(convertedLevel)});
}

void LcdCommRevB::setBackplateLedColor(Color color) {
    if (isFlagship()) {
        sendCommand(CmdSetLighting, {color.r, color.g, color.b});
    } else {
        LCDCTL_INFO << "Only HW revision 'flagship' supports backplate LED color setting";
    }
}

void LcdCommRevB::setOrientation(Orientation orientation) {
    // Basic orientations (portrait / landscape) are managed by the display; the
    // reverse orientations (reverse portrait / reverse landscape) are software-managed.
    orientation_ = orientation;
    if (orientation_ == Orientation::Portrait || orientation_ == Orientation::ReversePortrait) {
        sendCommand(CmdSetOrientation, {OrientationPortrait});
    } else {
        sendCommand(CmdSetOrientation, {OrientationLandscape});
    }
}

void LcdCommRevB::displayPILImage(const Image& image, int x, int y, int imageWidth,
                                  int imageHeight) {
    // If the image height/width isn't provided, use the native image size.
    if (imageHeight == 0) {
        imageHeight = image.height();
    }
    if (imageWidth == 0) {
        imageWidth = image.width();
    }

    // If our image is bigger than our display, resize it to fit our screen.
    if (image.height() > getHeight()) {
        imageHeight = getHeight();
    }
    if (image.width() > getWidth()) {
        imageWidth = getWidth();
    }

    if (x > getWidth()) {
        throw std::invalid_argument("image X coordinate must be <= display width");
    }
    if (y > getHeight()) {
        throw std::invalid_argument("image Y coordinate must be <= display height");
    }
    if (imageHeight <= 0) {
        throw std::invalid_argument("image height must be > 0");
    }
    if (imageWidth <= 0) {
        throw std::invalid_argument("image width must be > 0");
    }

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    if (orientation_ == Orientation::Portrait || orientation_ == Orientation::Landscape) {
        x0 = x;
        y0 = y;
        x1 = x + imageWidth - 1;
        y1 = y + imageHeight - 1;
    } else {
        // Reverse landscape/portrait orientations are software-managed: get new coordinates.
        x0 = getWidth() - x - imageWidth;
        y0 = getHeight() - y - imageHeight;
        x1 = getWidth() - x - 1;
        y1 = getHeight() - y - 1;
    }

    sendCommand(CmdDisplayBitmap,
                {static_cast<std::uint8_t>((x0 >> 8) & 0xFF), static_cast<std::uint8_t>(x0 & 0xFF),
                 static_cast<std::uint8_t>((y0 >> 8) & 0xFF), static_cast<std::uint8_t>(y0 & 0xFF),
                 static_cast<std::uint8_t>((x1 >> 8) & 0xFF), static_cast<std::uint8_t>(x1 & 0xFF),
                 static_cast<std::uint8_t>((y1 >> 8) & 0xFF), static_cast<std::uint8_t>(y1 & 0xFF)});

    const Image serialized = serializeImage(image, imageHeight, imageWidth, orientation_);
    const std::vector<std::uint8_t> rgb565be = imageToRGB565(serialized, true);

    // Send image data by multiple of "display width" bytes.
    for (const std::vector<std::uint8_t>& chunk :
         chunked(rgb565be, static_cast<std::size_t>(getWidth()) * 8)) {
        sendLine(chunk);
    }

    // Cooldown between two bitmaps: we are not listening to events from the display.
    std::this_thread::sleep_for(kBitmapCooldown);
}

}  // namespace lcdctl
