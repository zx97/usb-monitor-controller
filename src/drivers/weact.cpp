// SPDX-License-Identifier: GPL-3.0-or-later
//
// WeAct Studio Display FS V1 driver, shared by the 3.5" ("A") and 0.96" ("B")
// variants (the subclasses only fix the portrait resolution).
//
// Ported from library/lcd/lcd_comm_weact_a.py and
// library/lcd/lcd_comm_weact_b.py of
// https://github.com/mathoudebine/turing-smart-screen-python/
// (original code Copyright (C) 2024 WeAct Studio).
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include "lcdctl/drivers.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "lcdctl/logging.hpp"
#include "lcdctl/serialize.hpp"

namespace lcdctl {
namespace {

// Vendor command ids. They are identical for both WeAct revisions; only the
// resolution differs (320x480 vs 80x160).
enum WeActCommand : int {
    CmdWhoAmI = 0x81,                // establish communication before driving the screen
    CmdSetOrientation = 0x02,        // screen orientation
    CmdSetBrightness = 0x03,         // screen brightness
    CmdFull = 0x04,                  // fill the whole screen with a solid color
    CmdSetBitmap = 0x05,             // display a bitmap region
    CmdSetBitmapWithFastlz = 0x15,   // bitmap compressed with fastlz (unused by this port)
    CmdEnableHumitureReport = 0x06,  // periodic temperature/humidity report (unused by this port)
    CmdFree = 0x07,                  // release the panel
    CmdSystemVersion = 0x42,         // read the firmware version
    CmdEnd = 0x0A,                   // every command is terminated by this byte
    CmdRead = 0x80,                  // OR-ed into a command to request a reply
};

// Python applies `bytes.decode("ascii").strip()`; additionally drop the NUL
// padding the firmware commonly appends.
std::string trimVersion(const std::string& value) {
    const auto isStripped = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
               c == '\f' || c == '\0';
    };
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && isStripped(value[begin])) {
        ++begin;
    }
    while (end > begin && isStripped(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

}  // namespace

LcdCommWeAct::LcdCommWeAct(std::unique_ptr<Transport> transport, int displayWidth,
                           int displayHeight)
    : LcdComm(std::move(transport), displayWidth, displayHeight) {}

// The Python driver wraps every write either directly or through its async
// queue; this port writes synchronously.
void LcdCommWeAct::sendCommand(const std::vector<std::uint8_t>& data) { writeData(data); }

void LcdCommWeAct::initializeComm() {
    // Drop anything left over from a previous run before querying the firmware.
    if (transport_) {
        transport_->readAll();
    }

    const std::vector<std::uint8_t> request = {
        static_cast<std::uint8_t>(CmdSystemVersion | CmdRead),  // 0x42 | 0x80 == 0xC2
        static_cast<std::uint8_t>(CmdEnd),
    };
    writeData(request);

    const std::vector<std::uint8_t> response = readData(19);
    flushInput();

    if (response.size() == 19) {
        // Bytes [1, 9) hold the ASCII version, e.g. "V1.0.0.0".
        const std::string raw(response.begin() + 1, response.begin() + 9);
        deviceVersion_ = trimVersion(raw);
        LCDCTL_INFO << "Device version: " << deviceVersion_;
    } else {
        LCDCTL_INFO << "Get version failed";
    }
}

// These screens do not implement a reset command.
void LcdCommWeAct::reset() {}

void LcdCommWeAct::clear() { full(Color{0, 0, 0}); }

// CMD_FULL paints the whole panel with a single RGB565 little-endian color.
void LcdCommWeAct::full(Color color) {
    const std::uint16_t rgb = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(color.r >> 3) << 11) |
        (static_cast<std::uint16_t>(color.g >> 2) << 5) |
        static_cast<std::uint16_t>(color.b >> 3));

    const int xe = getWidth();
    const int ye = getHeight();

    std::vector<std::uint8_t> buffer(12, 0);
    buffer[0] = static_cast<std::uint8_t>(CmdFull);
    // bytes [1..4] stay 0 (origin x0/y0 = 0, 0)
    buffer[5] = static_cast<std::uint8_t>((xe - 1) & 0xFF);
    buffer[6] = static_cast<std::uint8_t>((xe >> 8) & 0xFF);
    buffer[7] = static_cast<std::uint8_t>((ye - 1) & 0xFF);
    buffer[8] = static_cast<std::uint8_t>((ye >> 8) & 0xFF);
    // RGB565, little-endian
    buffer[9] = static_cast<std::uint8_t>(rgb & 0xFF);
    buffer[10] = static_cast<std::uint8_t>((rgb >> 8) & 0xFF);
    buffer[11] = static_cast<std::uint8_t>(CmdEnd);
    sendCommand(buffer);
}

void LcdCommWeAct::screenOff() {
    setBrightness(0);
    const std::vector<std::uint8_t> freeCommand = {
        static_cast<std::uint8_t>(CmdFree),
        static_cast<std::uint8_t>(CmdEnd),
    };
    sendCommand(freeCommand);
}

void LcdCommWeAct::screenOn() { setBrightness(brightness_); }

void LcdCommWeAct::setBrightness(int level) {
    if (level < 0 || level > 100) {
        throw std::invalid_argument("brightness level must be within [0, 100]");
    }
    const int convertedLevel = static_cast<int>((level / 100.0) * 255);
    const int brightnessMs = 1000;

    std::vector<std::uint8_t> buffer(5, 0);
    buffer[0] = static_cast<std::uint8_t>(CmdSetBrightness);
    buffer[1] = static_cast<std::uint8_t>(convertedLevel & 0xFF);
    buffer[2] = static_cast<std::uint8_t>(brightnessMs & 0xFF);
    buffer[3] = static_cast<std::uint8_t>((brightnessMs >> 8) & 0xFF);
    buffer[4] = static_cast<std::uint8_t>(CmdEnd);
    sendCommand(buffer);

    brightness_ = level;
}

void LcdCommWeAct::setOrientation(Orientation orientation) {
    orientation_ = orientation;
    const std::vector<std::uint8_t> buffer = {
        static_cast<std::uint8_t>(CmdSetOrientation),
        static_cast<std::uint8_t>(orientation),
        static_cast<std::uint8_t>(CmdEnd),
    };
    sendCommand(buffer);
}

void LcdCommWeAct::displayPILImage(const Image& image, int x, int y, int imageWidth,
                                   int imageHeight) {
    const int width = getWidth();
    const int height = getHeight();

    // If the size is not provided, use the image's native size.
    if (imageHeight == 0) {
        imageHeight = image.height();
    }
    if (imageWidth == 0) {
        imageWidth = image.width();
    }

    // Never declare a region larger than the display.
    if (image.height() > height) {
        imageHeight = height;
    }
    if (image.width() > width) {
        imageWidth = width;
    }

    if (x > width) {
        throw std::invalid_argument("display image x must be <= display width");
    }
    if (y > height) {
        throw std::invalid_argument("display image y must be <= display height");
    }
    if (imageHeight <= 0) {
        throw std::invalid_argument("image height must be > 0");
    }
    if (imageWidth <= 0) {
        throw std::invalid_argument("image width must be > 0");
    }
    if (x + imageWidth > width) {
        throw std::invalid_argument("display bitmap width + x exceeds display width");
    }
    if (y + imageHeight > height) {
        throw std::invalid_argument("display bitmap height + y exceeds display height");
    }

    const int x0 = x;
    const int y0 = y;
    const int x1 = x + imageWidth - 1;
    const int y1 = y + imageHeight - 1;

    // CMD_SET_BITMAP header: origin and inclusive end coordinates.
    std::vector<std::uint8_t> header(10, 0);
    header[0] = static_cast<std::uint8_t>(CmdSetBitmap);
    header[1] = static_cast<std::uint8_t>(x0 & 0xFF);
    header[2] = static_cast<std::uint8_t>((x0 >> 8) & 0xFF);
    header[3] = static_cast<std::uint8_t>(y0 & 0xFF);
    header[4] = static_cast<std::uint8_t>((y0 >> 8) & 0xFF);
    header[5] = static_cast<std::uint8_t>(x1 & 0xFF);
    header[6] = static_cast<std::uint8_t>((x1 >> 8) & 0xFF);
    header[7] = static_cast<std::uint8_t>(y1 & 0xFF);
    header[8] = static_cast<std::uint8_t>((y1 >> 8) & 0xFF);
    header[9] = static_cast<std::uint8_t>(CmdEnd);

    const std::size_t chunkSize = static_cast<std::size_t>(width) * 4;
    // The Python driver sends the whole image buffer, matching the header it
    // just emitted (the rectangle therefore has to match the image itself).
    const std::vector<std::uint8_t> rgb565le = imageToRGB565(image, /*bigEndian=*/false);

    sendLine(header);
    for (const std::vector<std::uint8_t>& chunk : chunked(rgb565le, chunkSize)) {
        sendLine(chunk);
    }
}

}  // namespace lcdctl
