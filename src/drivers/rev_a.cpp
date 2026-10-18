// SPDX-License-Identifier: GPL-3.0-or-later
//
// Revision A: Turing Smart Screen 3.5" and UsbPCMonitor screens.
// Ported from library/lcd/lcd_comm_rev_a.py of
// https://github.com/mathoudebine/turing-smart-screen-python/

#include "lcdctl/drivers.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "lcdctl/logging.hpp"
#include "lcdctl/serialize.hpp"

namespace lcdctl {
namespace {

enum RevACommand : int {
    CmdHello = 69,
    CmdReset = 101,
    CmdClear = 102,
    CmdToBlack = 103,
    CmdScreenOff = 108,
    CmdScreenOn = 109,
    CmdSetBrightness = 110,
    CmdSetOrientation = 121,
    CmdDisplayBitmap = 197,
};

// HELLO answers identifying the UsbPCMonitor sub-revisions.
const std::vector<std::uint8_t> kUsbMonitor35 = {0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
const std::vector<std::uint8_t> kUsbMonitor5 = {0x02, 0x02, 0x02, 0x02, 0x02, 0x02};
const std::vector<std::uint8_t> kUsbMonitor7 = {0x03, 0x03, 0x03, 0x03, 0x03, 0x03};

}  // namespace

LcdCommRevA::LcdCommRevA(std::unique_ptr<Transport> transport, int displayWidth,
                         int displayHeight)
    : LcdComm(std::move(transport), displayWidth, displayHeight) {}

void LcdCommRevA::sendCommand(int cmd, int x, int y, int ex, int ey, bool /*bypassQueue*/) {
    std::vector<std::uint8_t> buffer(6);
    buffer[0] = static_cast<std::uint8_t>((x >> 2) & 0xFF);
    buffer[1] = static_cast<std::uint8_t>((((x & 3) << 6) + (y >> 4)) & 0xFF);
    buffer[2] = static_cast<std::uint8_t>((((y & 15) << 4) + (ex >> 6)) & 0xFF);
    buffer[3] = static_cast<std::uint8_t>((((ex & 63) << 2) + (ey >> 8)) & 0xFF);
    buffer[4] = static_cast<std::uint8_t>(ey & 0xFF);
    buffer[5] = static_cast<std::uint8_t>(cmd & 0xFF);
    writeData(buffer);
}

void LcdCommRevA::hello() {
    const std::vector<std::uint8_t> hello(6, static_cast<std::uint8_t>(CmdHello));
    writeData(hello);
    const std::vector<std::uint8_t> response = readData(6);
    flushInput();

    if (response == kUsbMonitor35) {
        subRevision_ = 1;
        displayWidth_ = 320;
        displayHeight_ = 480;
    } else if (response == kUsbMonitor5) {
        subRevision_ = 2;
        displayWidth_ = 480;
        displayHeight_ = 800;
    } else if (response == kUsbMonitor7) {
        subRevision_ = 3;
        displayWidth_ = 600;
        displayHeight_ = 1024;
    } else {
        subRevision_ = 0;  // official Turing 3.5" (does not answer HELLO)
        displayWidth_ = 320;
        displayHeight_ = 480;
    }
    LCDCTL_DEBUG << "Rev A sub-revision: " << subRevision_ << " (" << displayWidth_ << "x"
                 << displayHeight_ << ")";
}

void LcdCommRevA::initializeComm() { hello(); }

void LcdCommRevA::reset() {
    LCDCTL_INFO << "Display reset (port may disconnect and change)...";
    sendCommand(CmdReset, 0, 0, 0, 0);
    if (transport_) {
        transport_->close();
    }
    std::this_thread::sleep_for(std::chrono::seconds(5));
    if (!transport_ || !transport_->reopen()) {
        throw std::runtime_error("could not reopen the screen after reset");
    }
}

void LcdCommRevA::clear() {
    const Orientation previous = orientation_;
    setOrientation(Orientation::Portrait);  // orientation must be portrait to clear
    sendCommand(CmdClear, 0, 0, 0, 0);
    setOrientation(previous);
}

void LcdCommRevA::screenOff() { sendCommand(CmdScreenOff, 0, 0, 0, 0); }

void LcdCommRevA::screenOn() { sendCommand(CmdScreenOn, 0, 0, 0, 0); }

void LcdCommRevA::setBrightness(int level) {
    if (level < 0 || level > 100) {
        throw std::invalid_argument("brightness must be within [0, 100]");
    }
    // The display scales 0 (brightest) .. 255 (darkest).
    const int absolute = static_cast<int>(255 - (level / 100.0) * 255);
    sendCommand(CmdSetBrightness, absolute, 0, 0, 0);
}

void LcdCommRevA::setOrientation(Orientation orientation) {
    orientation_ = orientation;
    const int width = getWidth();
    const int height = getHeight();

    std::vector<std::uint8_t> buffer(16, 0);
    buffer[5] = static_cast<std::uint8_t>(CmdSetOrientation);
    buffer[6] = static_cast<std::uint8_t>(static_cast<int>(orientation) + 100);
    buffer[7] = static_cast<std::uint8_t>(width >> 8);
    buffer[8] = static_cast<std::uint8_t>(width & 255);
    buffer[9] = static_cast<std::uint8_t>(height >> 8);
    buffer[10] = static_cast<std::uint8_t>(height & 255);
    writeData(buffer);
}

void LcdCommRevA::displayPILImage(const Image& image, int x, int y, int imageWidth,
                                  int imageHeight) {
    const int width = getWidth();
    const int height = getHeight();

    int targetWidth = imageWidth > 0 ? imageWidth : image.width();
    int targetHeight = imageHeight > 0 ? imageHeight : image.height();

    if (x + targetWidth > width) {
        targetWidth = width - x;
    }
    if (y + targetHeight > height) {
        targetHeight = height - y;
    }
    if (targetWidth <= 0 || targetHeight <= 0) {
        throw std::invalid_argument("image does not fit on the screen");
    }

    Image cropped = image;
    if (targetWidth != image.width() || targetHeight != image.height()) {
        cropped = image.crop(0, 0, targetWidth, targetHeight);
    }

    const int x1 = x + targetWidth - 1;
    const int y1 = y + targetHeight - 1;

    const std::vector<std::uint8_t> rgb565le = imageToRGB565(cropped, false);
    sendCommand(CmdDisplayBitmap, x, y, x1, y1);

    for (const std::vector<std::uint8_t>& chunk : chunked(rgb565le, static_cast<std::size_t>(width) * 8)) {
        sendLine(chunk);
    }
}

}  // namespace lcdctl
