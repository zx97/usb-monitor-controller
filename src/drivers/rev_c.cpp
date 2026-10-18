// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - Revision C driver.
//
// Turing Smart Screen 2.1" / 2.8" / 5" / 8" (0xef69 framed protocol).
//
// Ported from library/lcd/lcd_comm_rev_c.py of
// https://github.com/mathoudebine/turing-smart-screen-python/
// Original Python code Copyright (C) 2021 Matthieu Houdebine (mathoudebine)
//                    Copyright (C) 2023 Alex W. Baulé (alexwbaule)
//                    Copyright (C) 2023 Arthur Ferrai (arthurferrai)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include "lcdctl/drivers.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "lcdctl/logging.hpp"
#include "lcdctl/serialize.hpp"

namespace lcdctl {
namespace {

// Append the low `bytes` bytes of `value` in big-endian order. This mirrors the
// Python int.to_bytes(size, "big") calls used by the protocol.
void appendBigEndian(std::vector<std::uint8_t>& out, long long value, int bytes) {
    for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

// Concatenate the `chunkSize`-byte chunks of `data`, inserting a single 0x00
// byte between consecutive chunks: equivalent to b'\x00'.join(chunked(...)).
std::vector<std::uint8_t> joinChunksWithNull(const std::vector<std::uint8_t>& data,
                                             std::size_t chunkSize) {
    std::vector<std::uint8_t> out;
    const std::vector<std::vector<std::uint8_t>> chunks = chunked(data, chunkSize);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        if (i > 0) {
            out.push_back(0x00);
        }
        out.insert(out.end(), chunks[i].begin(), chunks[i].end());
    }
    return out;
}

// Mimic Python's ''.join(filter(lambda x: x in string.printable, ...)): keep
// only the printable ASCII range plus the whitespace characters.
std::string toPrintableAscii(const std::vector<std::uint8_t>& data) {
    std::string out;
    out.reserve(data.size());
    for (std::uint8_t c : data) {
        const bool printable = (c >= 0x20 && c <= 0x7E) || c == '\t' || c == '\n' || c == '\r' ||
                               c == '\v' || c == '\f';
        if (printable) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

// Pad `message` so its length becomes a multiple of the 250-byte protocol
// frame. `padByte` is 0x00 for every command except START_DISPLAY_BITMAP
// (Padding.START_DISPLAY_BITMAP = 0x2c).
void padToFrame(std::vector<std::uint8_t>& message, std::uint8_t padByte) {
    const std::size_t remainder = message.size() % 250;
    if (remainder != 0) {
        message.insert(message.end(), 250 - remainder, padByte);
    }
}

std::string trim(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\n\r\v\f");
    if (begin == std::string::npos) {
        return "";
    }
    const std::size_t end = text.find_last_not_of(" \t\n\r\v\f");
    return text.substr(begin, end - begin + 1);
}

// Split on '.' and parse index 2 as the ROM version (mirrors
// int(response.split(".")[2]) + the [80, 100] sanity clamp). Returns 87 when
// the field is missing or not a plain integer.
int parseRomVersion(const std::string& response) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t dot = response.find('.', start);
        if (dot == std::string::npos) {
            parts.push_back(response.substr(start));
            break;
        }
        parts.push_back(response.substr(start, dot - start));
        start = dot + 1;
    }

    const std::string field = (parts.size() > 2) ? trim(parts[2]) : "";

    int value = 0;
    bool parsed = false;
    if (!field.empty()) {
        try {
            std::size_t consumed = 0;
            const int candidate = std::stoi(field, &consumed);
            if (consumed == field.size()) {
                value = candidate;
                parsed = true;
            }
        } catch (const std::exception&) {
            parsed = false;
        }
    }

    if (!parsed) {
        LCDCTL_WARN << "Display returned invalid or unsupported ID, use default ROM version 87";
        return 87;
    }
    if (value < 80 || value > 100) {
        LCDCTL_WARN << "ROM version " << value << " may be invalid, use default ROM version 87";
        return 87;
    }
    return value;
}

}  // namespace

LcdCommRevC::LcdCommRevC(std::unique_ptr<Transport> transport, int displayWidth,
                         int displayHeight)
    : LcdComm(std::move(transport), displayWidth, displayHeight) {}

// ---------------------------------------------------------------------------
// Frame builder. `bypassQueue` is kept for API parity with the Python
// implementation but has no effect here: the base LcdComm has no update queue,
// so every frame is written synchronously anyway.
// ---------------------------------------------------------------------------
void LcdCommRevC::sendCommand(int cmd, const std::vector<std::uint8_t>& payload,
                              bool /*bypassQueue*/, int readSize) {
    std::vector<std::uint8_t> message;

    // Exact command bytes of the vendor protocol (Python Command enum).
    switch (cmd) {
        case CmdHello:
            message = {0x01, 0xef, 0x69, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc5, 0xd3};
            break;
        case CmdOptions:
            message = {0x7d, 0xef, 0x69, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x2d};
            break;
        case CmdRestart:
            message = {0x84, 0xef, 0x69, 0x00, 0x00, 0x00, 0x01};
            break;
        case CmdTurnOff:
            message = {0x83, 0xef, 0x69, 0x00, 0x00, 0x00, 0x01};
            break;
        case CmdTurnOn:
            message = {0x83, 0xef, 0x69, 0x00, 0x00, 0x00, 0x00};
            break;
        case CmdSetBrightness:
            message = {0x7b, 0xef, 0x69, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
            break;
        case CmdStopVideo:
            message = {0x79, 0xef, 0x69, 0x00, 0x00, 0x00, 0x01};
            break;
        case CmdStopMedia:
            message = {0x96, 0xef, 0x69, 0x00, 0x00, 0x00, 0x01};
            break;
        case CmdQueryStatus:
            message = {0xcf, 0xef, 0x69, 0x00, 0x00, 0x00, 0x01};
            break;
        case CmdStartDisplayBitmap:
            message = {0x2c};
            break;
        case CmdPreUpdateBitmap:
            message = {0x86, 0xef, 0x69, 0x00, 0x00, 0x00, 0x01};
            break;
        case CmdUpdateBitmap:
            message = {0xcc, 0xef, 0x69, 0x00};
            break;
        case CmdDisplayBitmap2Inch:
            message = {0xc8, 0xef, 0x69, 0x00, 0x0e, 0x10};
            break;
        case CmdDisplayBitmap5Inch:
            message = {0xc8, 0xef, 0x69, 0x00, 0x17, 0x70};
            break;
        case CmdDisplayBitmap8Inch:
            message = {0xc8, 0xef, 0x69, 0x00, 0x38, 0x40};
            break;
        case CmdStartModeDefault:
        case CmdNoFlip:
        case CmdSleepOff:
            message = {0x00};
            break;
        case CmdSendPayload:
            // Python never prefixes a raw payload with its own command id
            // (Command.SEND_PAYLOAD is skipped): the payload *is* the message.
            break;
    }

    if (!payload.empty()) {
        message.insert(message.end(), payload.begin(), payload.end());
    }

    const std::uint8_t padByte =
        (cmd == CmdStartDisplayBitmap) ? 0x2c : static_cast<std::uint8_t>(0x00);
    padToFrame(message, padByte);

    writeData(message);
    if (readSize > 0) {
        (void)readData(static_cast<std::size_t>(readSize));
    }
}

// ---------------------------------------------------------------------------
// HELLO / identification
// ---------------------------------------------------------------------------
void LcdCommRevC::hello() {
    subRevision_ = 0;

    flushInput();
    sendCommand(CmdHello, {}, true);
    std::string response = toPrintableAscii(readData(23));
    flushInput();
    LCDCTL_DEBUG << "Display ID returned: " << response;

    while (response.compare(0, 4, "chs_") != 0) {
        LCDCTL_WARN << "Display returned invalid or unsupported ID, try again in 1 second";
        std::this_thread::sleep_for(std::chrono::seconds(1));
        sendCommand(CmdHello, {}, true);
        response = toPrintableAscii(readData(23));
        flushInput();
        LCDCTL_DEBUG << "Display ID returned: " << response;
    }

    // The ID string is unreliable on some models (e.g. a 2.1" display answers
    // "chs_5inch"), so the sub-revision comes from the resolution instead.
    if (displayWidth_ == 480 && displayHeight_ == 480) {
        subRevision_ = 1;  // 2.1" / 2.8"
    } else if (displayWidth_ == 480 && displayHeight_ == 800) {
        subRevision_ = 2;  // 5"
    } else if (displayWidth_ == 480 && displayHeight_ == 1920) {
        subRevision_ = 3;  // 8"
    } else {
        LCDCTL_ERROR << "Unsupported resolution " << displayWidth_ << "x" << displayHeight_
                     << " for revision C";
    }

    romVersion_ = parseRomVersion(response);

    LCDCTL_DEBUG << "HW sub-revision detected: " << subRevision_
                 << ", ROM version: " << romVersion_;
}

void LcdCommRevC::initializeComm() { hello(); }

// ---------------------------------------------------------------------------
// Reset
//
// Deviation from the Python implementation: the original sends RESTART, then
// polls the OS serial-port list (comports()) to wait for the device to
// disconnect and re-enumerate before reopening it. Our Transport abstraction
// exposes no port-list logic, so instead we sleep a fixed delay that covers
// the typical re-enumeration and then ask the transport to reopen itself. This
// is the same simplification used by rev A.
// ---------------------------------------------------------------------------
void LcdCommRevC::reset() {
    LCDCTL_INFO << "Display reset (port may disconnect and change)...";
    sendCommand(CmdRestart, {}, true);
    if (transport_) {
        transport_->close();
    }
    std::this_thread::sleep_for(std::chrono::seconds(5));
    if (!transport_ || !transport_->reopen()) {
        throw std::runtime_error("could not reopen the screen after reset");
    }
}

// ---------------------------------------------------------------------------
// Clear / power / brightness / orientation
// ---------------------------------------------------------------------------
void LcdCommRevC::clear() {
    // This revision has no Clear command: draw a blank white image instead.
    // Force portrait first in case the current orientation differs.
    const Orientation backup = orientation_;
    setOrientation(Orientation::Portrait);

    const Image blank(getWidth(), getHeight(), 3, 255);
    displayPILImage(blank);

    setOrientation(backup);
}

void LcdCommRevC::screenOff() {
    sendCommand(CmdStopVideo);
    sendCommand(CmdStopMedia, {}, false, 1024);
    sendCommand(CmdTurnOff);
}

void LcdCommRevC::screenOn() {
    sendCommand(CmdStopVideo);
    sendCommand(CmdStopMedia, {}, false, 1024);
}

void LcdCommRevC::setBrightness(int level) {
    if (level < 0 || level > 100) {
        throw std::invalid_argument("brightness must be within [0, 100]");
    }
    // Brightness scales from 0 (darkest) to 255 (brightest).
    const std::uint8_t converted = static_cast<std::uint8_t>((level / 100.0) * 255);
    sendCommand(CmdSetBrightness, {converted}, true);
}

void LcdCommRevC::setOrientation(Orientation orientation) {
    orientation_ = orientation;
    // STARTMODE_DEFAULT + Padding.NULL + NO_FLIP + SleepInterval.OFF.
    const std::vector<std::uint8_t> payload = {0x00, 0x00, 0x00, 0x00};
    sendCommand(CmdOptions, payload);
}

// ---------------------------------------------------------------------------
// Image generation
//
// Pillow's Image.rotate(angle, expand=True) turns counter-clockwise, whereas
// our Image::rotate(deg) turns clockwise. So PIL 90 => C++ 270,
// PIL 270 => C++ 90 and PIL 180 => C++ 180.
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> LcdCommRevC::generateFullImage(const Image& image) const {
    Image rotated = image;

    if (subRevision_ == 3) {
        // 8" panels use the opposite landscape/portrait mapping.
        switch (orientation_) {
            case Orientation::Landscape:
                rotated = image.rotate(90);  // PIL rotate(270)
                break;
            case Orientation::ReverseLandscape:
                rotated = image.rotate(270);  // PIL rotate(90)
                break;
            case Orientation::Portrait:
                rotated = image.rotate(180);  // PIL rotate(180)
                break;
            case Orientation::ReversePortrait:
                break;  // no rotation
        }
    } else {
        switch (orientation_) {
            case Orientation::Portrait:
                rotated = image.rotate(270);  // PIL rotate(90)
                break;
            case Orientation::ReversePortrait:
                rotated = image.rotate(90);  // PIL rotate(270)
                break;
            case Orientation::ReverseLandscape:
                rotated = image.rotate(180);  // PIL rotate(180)
                break;
            case Orientation::Landscape:
                break;  // no rotation
        }
    }

    const std::vector<std::uint8_t> bgra = imageToBGRA(rotated);
    return joinChunksWithNull(bgra, 249);
}

std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>
LcdCommRevC::generateUpdateImage(const Image& image, int x, int y, int count) const {
    int x0 = x;
    int y0 = y;
    Image rotated = image;

    const int width = getWidth();
    const int height = getHeight();

    if (subRevision_ == 3) {
        // 8" panels use the opposite landscape/portrait mapping.
        switch (orientation_) {
            case Orientation::Landscape:
                rotated = image.rotate(90);  // PIL rotate(270)
                y0 = height - y - rotated.width();
                break;
            case Orientation::ReverseLandscape:
                rotated = image.rotate(270);  // PIL rotate(90)
                x0 = width - x - rotated.height();
                break;
            case Orientation::Portrait:
                rotated = image.rotate(180);  // PIL rotate(180)
                x0 = height - y - rotated.height();
                y0 = height - x - rotated.width();
                break;
            case Orientation::ReversePortrait:
                x0 = y;
                y0 = x;
                break;
        }
    } else {
        switch (orientation_) {
            case Orientation::Portrait:
                rotated = image.rotate(270);  // PIL rotate(90)
                x0 = width - x - rotated.height();
                break;
            case Orientation::ReversePortrait:
                rotated = image.rotate(90);  // PIL rotate(270)
                y0 = height - y - rotated.width();
                break;
            case Orientation::ReverseLandscape:
                rotated = image.rotate(180);  // PIL rotate(180)
                y0 = width - x - rotated.width();
                x0 = height - y - rotated.height();
                break;
            case Orientation::Landscape:
                x0 = y;
                y0 = x;
                break;
        }
    }

    // Some panels require a different RGBA encoding: BGRA (4 bytes) on the
    // newer ROMs (except the 2"), plain BGR (3 bytes) otherwise.
    const bool useBgra = (subRevision_ != 1 && romVersion_ > 88);
    const int pixelSize = useBgra ? 4 : 3;
    const std::vector<std::uint8_t> imgData =
        useBgra ? imageToBGRA(rotated) : imageToBGR(rotated);

    const std::size_t rowBytes = static_cast<std::size_t>(rotated.width()) * pixelSize;
    const std::vector<std::vector<std::uint8_t>> rows = chunked(imgData, rowBytes);

    const int indexStride = (subRevision_ == 3) ? displayWidth_ : displayHeight_;

    std::vector<std::uint8_t> imgRaw;
    for (std::size_t h = 0; h < rows.size(); ++h) {
        const long long index =
            static_cast<long long>((x0 + static_cast<int>(h)) * indexStride + y0);
        appendBigEndian(imgRaw, index, 3);
        appendBigEndian(imgRaw, rotated.width(), 2);
        imgRaw.insert(imgRaw.end(), rows[h].begin(), rows[h].end());
    }

    const long long imageSize = static_cast<long long>(imgRaw.size()) + 2;  // +2 for "ef69"

    std::vector<std::uint8_t> payload;
    // CmdUpdateBitmap prefix (Command.UPDATE_BITMAP).
    payload.insert(payload.end(), {0xcc, 0xef, 0x69, 0x00});
    appendBigEndian(payload, imageSize, 3);
    payload.insert(payload.end(), 3, static_cast<std::uint8_t>(0x00));
    appendBigEndian(payload, count, 4);

    if (imgRaw.size() > 250) {
        imgRaw = joinChunksWithNull(imgRaw, 249);
    }
    imgRaw.push_back(0xef);
    imgRaw.push_back(0x69);

    return {std::move(imgRaw), std::move(payload)};
}

// ---------------------------------------------------------------------------
// Bitmap display
// ---------------------------------------------------------------------------
void LcdCommRevC::displayPILImage(const Image& image, int x, int y, int imageWidth,
                                  int imageHeight) {
    int targetWidth = imageWidth != 0 ? imageWidth : image.width();
    int targetHeight = imageHeight != 0 ? imageHeight : image.height();

    // Clamp to the screen if the image is larger than the display.
    if (image.height() > getHeight()) {
        targetHeight = getHeight();
    }
    if (image.width() > getWidth()) {
        targetWidth = getWidth();
    }

    Image cropped = image;
    if (targetWidth != image.width() || targetHeight != image.height()) {
        cropped = image.crop(0, 0, targetWidth, targetHeight);
    }

    if (x > getWidth()) {
        throw std::invalid_argument("image X coordinate must be <= display width");
    }
    if (y > getHeight()) {
        throw std::invalid_argument("image Y coordinate must be <= display height");
    }
    if (targetHeight <= 0) {
        throw std::invalid_argument("image height must be > 0");
    }
    if (targetWidth <= 0) {
        throw std::invalid_argument("image width must be > 0");
    }

    const bool fullScreen =
        (x == 0 && y == 0 && targetWidth == getWidth() && targetHeight == getHeight());

    if (fullScreen) {
        sendCommand(CmdPreUpdateBitmap);
        sendCommand(CmdStartDisplayBitmap);

        int displayBmpCmd = CmdSendPayload;  // placeholder, always overwritten below
        if (subRevision_ == 2) {
            displayBmpCmd = CmdDisplayBitmap5Inch;
        } else if (subRevision_ == 1) {
            displayBmpCmd = CmdDisplayBitmap2Inch;
        } else if (subRevision_ == 3) {
            displayBmpCmd = CmdDisplayBitmap8Inch;
        } else {
            throw std::runtime_error("revision C sub-revision unknown: cannot display image");
        }

        const int bmpPayload = static_cast<int>(displayWidth_ * displayWidth_ / 64.0);
        std::vector<std::uint8_t> sizeHeader;
        appendBigEndian(sizeHeader, bmpPayload, 2);
        sendCommand(displayBmpCmd, sizeHeader);

        sendCommand(CmdSendPayload, generateFullImage(cropped), false, 1024);
        sendCommand(CmdQueryStatus, {}, false, 1024);
    } else {
        const std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> generated =
            generateUpdateImage(cropped, x, y, updateCount_);
        sendCommand(CmdSendPayload, generated.second);  // header/payload
        sendCommand(CmdSendPayload, generated.first);   // raw image data
        sendCommand(CmdQueryStatus, {}, false, 1024);
        ++updateCount_;
    }
}

}  // namespace lcdctl
