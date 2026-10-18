// SPDX-License-Identifier: GPL-3.0-or-later
//
// Revision D: Kipye Qiye Smart Display 3.5".
// Ported from library/lcd/lcd_comm_rev_d.py of
// https://github.com/mathoudebine/turing-smart-screen-python/
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include "lcdctl/drivers.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "lcdctl/logging.hpp"
#include "lcdctl/serialize.hpp"

namespace lcdctl {
namespace {

// Commands of the rev D protocol: each one is a full byte array.
const std::vector<std::uint8_t> kGetInfo = {71, 0, 0, 0};
const std::vector<std::uint8_t> kSetOrg = {67, 72, 0, 0};     // Set portrait orientation
const std::vector<std::uint8_t> kSet180 = {67, 71, 0, 0};     // Set reverse portrait orientation
const std::vector<std::uint8_t> kSetHf = {67, 68, 0, 0};      // Portrait with horizontal mirroring
const std::vector<std::uint8_t> kSetVf = {67, 70, 0, 0};      // Reverse portrait + horizontal mirror
const std::vector<std::uint8_t> kSetBl = {67, 67};            // Brightness setting
const std::vector<std::uint8_t> kDispColor = {67, 66};        // Display RGB565 color full screen
const std::vector<std::uint8_t> kBlockWrite = {67, 65};       // Send bitmap size
const std::vector<std::uint8_t> kIntoPicMode = {68, 0, 0, 0}; // Start bitmap transmission
const std::vector<std::uint8_t> kOutPicMode = {65, 0, 0, 0};  // End bitmap transmission

// Default brightness percentage used by ScreenOn(), mirroring the Python default.
constexpr int kDefaultBrightness = 25;

// Append a 16-bit big-endian value to `out`.
void appendUint16be(std::vector<std::uint8_t>& out, int value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

}  // namespace

LcdCommRevD::LcdCommRevD(std::unique_ptr<Transport> transport, int displayWidth,
                         int displayHeight)
    : LcdComm(std::move(transport), displayWidth, displayHeight) {}

void LcdCommRevD::sendCommand(const std::vector<std::uint8_t>& cmd,
                             const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> message = cmd;
    if (!payload.empty()) {
        message.insert(message.end(), payload.begin(), payload.end());
    }

    writeData(message);

    // Empty the input buffer after each write: we don't process the acknowledgements
    // the screen sends back.
    flushInput();
}

void LcdCommRevD::initializeComm() {
    // HW revision D does not need any initialization handshake.
}

void LcdCommRevD::reset() {
    // HW revision D does not implement a reset command: clear the display instead.
    clear();
}

void LcdCommRevD::clear() {
    // HW revision D does not implement a Clear command: display a blank image on the
    // whole screen. 0xFFFF is the RGB565 white color.
    sendCommand(kDispColor, {0xFF, 0xFF});
}

void LcdCommRevD::screenOff() {
    // No native "ScreenOff": use SetBrightness(0) instead.
    setBrightness(0);
}

void LcdCommRevD::screenOn() {
    // No native "ScreenOn": use the default brightness instead.
    setBrightness(kDefaultBrightness);
}

void LcdCommRevD::setBrightness(int level) {
    if (level < 0 || level > 100) {
        throw std::invalid_argument("brightness level must be within [0, 100]");
    }

    // Brightness scales from 0 to 500, 500 being the brightest and 0 the darkest.
    const int convertedLevel = level * 5;
    std::vector<std::uint8_t> levelBytes;
    appendUint16be(levelBytes, convertedLevel);

    // Send the command twice because sometimes it is not applied...
    sendCommand(kSetBl, levelBytes);
    sendCommand(kSetBl, levelBytes);
}

void LcdCommRevD::setOrientation(Orientation orientation) {
    // Reverse orientations (reverse portrait / reverse landscape) are managed by the
    // display; basic orientations (portrait / landscape) are software-managed because
    // the screen commands only support portrait.
    orientation_ = orientation;

    if (orientation_ == Orientation::ReverseLandscape ||
        orientation_ == Orientation::ReversePortrait) {
        sendCommand(kSet180);
    } else {
        sendCommand(kSetOrg);
    }
}

void LcdCommRevD::displayPILImage(const Image& image, int x, int y, int imageWidth,
                                 int imageHeight) {
    const int width = getWidth();
    const int height = getHeight();

    // If the image height/width isn't provided, use the native image size.
    if (imageHeight == 0) {
        imageHeight = image.height();
    }
    if (imageWidth == 0) {
        imageWidth = image.width();
    }

    if (x > width) {
        throw std::invalid_argument("image X coordinate must be <= display width");
    }
    if (y > height) {
        throw std::invalid_argument("image Y coordinate must be <= display height");
    }
    if (imageHeight <= 0) {
        throw std::invalid_argument("image height must be > 0");
    }
    if (imageWidth <= 0) {
        throw std::invalid_argument("image width must be > 0");
    }

    // If our image size + the (x, y) position offsets are bigger than our display,
    // reduce the image size to fit our screen.
    if (x + imageWidth > width) {
        imageWidth = width - x;
    }
    if (y + imageHeight > height) {
        imageHeight = height - y;
    }

    Image transformed = image;
    if (imageWidth != transformed.width() || imageHeight != transformed.height()) {
        transformed = transformed.crop(0, 0, imageWidth, imageHeight);
    }

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    if (orientation_ == Orientation::Portrait || orientation_ == Orientation::ReversePortrait) {
        x0 = x;
        y0 = y;
        x1 = x + imageWidth - 1;
        y1 = y + imageHeight - 1;
    } else {
        // Landscape / reverse landscape orientations are software-managed: rotate the
        // image 90 degrees clockwise and compute the new coordinates.
        // (Python uses PIL `image.rotate(270, expand=True)`, which is 270 deg
        // counter-clockwise == 90 deg clockwise; our Image::rotate() is clockwise.)
        transformed = transformed.rotate(90);
        x0 = displayWidth_ - y - imageHeight;
        y0 = x;
        x1 = displayWidth_ - y - 1;
        y1 = x + imageWidth - 1;
        std::swap(imageWidth, imageHeight);
    }

    // Send bitmap size: x0, x1, y0, y1 as 16-bit big-endian values.
    std::vector<std::uint8_t> imageData;
    appendUint16be(imageData, x0);
    appendUint16be(imageData, x1);
    appendUint16be(imageData, y0);
    appendUint16be(imageData, y1);
    sendCommand(kBlockWrite, imageData);

    // Prepare bitmap data transmission.
    sendCommand(kIntoPicMode);

    const std::vector<std::uint8_t> rgb565be = imageToRGB565(transformed, true);

    // Send image data as 63-byte chunks, each prefixed with 0x50.
    for (const std::vector<std::uint8_t>& chunk : chunked(rgb565be, 63)) {
        std::vector<std::uint8_t> line;
        line.reserve(chunk.size() + 1);
        line.push_back(0x50);
        line.insert(line.end(), chunk.begin(), chunk.end());
        sendLine(line);
    }

    // Indicate the complete bitmap has been transmitted.
    sendCommand(kOutPicMode);
}

}  // namespace lcdctl
