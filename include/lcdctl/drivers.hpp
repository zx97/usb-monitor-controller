// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - concrete display drivers.
//
// One class per hardware revision, ported from library/lcd/lcd_comm_*.py of
// https://github.com/mathoudebine/turing-smart-screen-python/

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lcdctl/lcd_comm.hpp"
#include "lcdctl/transport.hpp"

namespace lcdctl {

// ---------------------------------------------------------------------------
// Revision A: Turing Smart Screen 3.5" / UsbPCMonitor 3.5", 5", 7"
// 6-byte commands, RGB565 little-endian, HELLO returns a sub-revision.
// ---------------------------------------------------------------------------
class LcdCommRevA : public LcdComm {
public:
    explicit LcdCommRevA(std::unique_ptr<Transport> transport,
                         int displayWidth = 320, int displayHeight = 480);

    void initializeComm() override;
    void reset() override;
    void clear() override;
    void screenOff() override;
    void screenOn() override;
    void setBrightness(int level) override;
    void setOrientation(Orientation orientation) override;
    void displayPILImage(const Image& image, int x = 0, int y = 0,
                         int imageWidth = 0, int imageHeight = 0) override;

    // 0 = Turing 3.5", 1/2/3 = UsbMonitor 3.5"/5"/7" (from HELLO)
    int subRevision() const { return subRevision_; }

private:
    void sendCommand(int cmd, int x, int y, int ex, int ey, bool bypassQueue = false);
    void hello();

    int subRevision_ = 0;
};

// ---------------------------------------------------------------------------
// Revision B: XuanFang 3.5" (incl. flagship)
// 10-byte framed commands, RGB565 big-endian, HELLO returns a sub-revision.
// ---------------------------------------------------------------------------
class LcdCommRevB : public LcdComm {
public:
    explicit LcdCommRevB(std::unique_ptr<Transport> transport,
                         int displayWidth = 320, int displayHeight = 480);

    void initializeComm() override;
    void reset() override;
    void clear() override;
    void screenOff() override;
    void screenOn() override;
    void setBrightness(int level) override;
    void setOrientation(Orientation orientation) override;
    void displayPILImage(const Image& image, int x = 0, int y = 0,
                         int imageWidth = 0, int imageHeight = 0) override;
    void setBackplateLedColor(Color color) override;

    int subRevision() const { return subRevision_; }  // 0xA01/0xA02/0xA11/0xA12
    bool isFlagship() const { return subRevision_ == 0xA02 || subRevision_ == 0xA12; }
    bool isBrightnessRange() const { return subRevision_ == 0xA11 || subRevision_ == 0xA12; }

private:
    void sendCommand(int cmd, const std::vector<std::uint8_t>& payload = {}, bool bypassQueue = false);
    void hello();

    int subRevision_ = 0xA01;
};

// ---------------------------------------------------------------------------
// Revision C: Turing 2.1" / 2.8" / 5" / 8" / 8.8"
// 0xef69 framed protocol, full-screen and partial bitmap updates.
// ---------------------------------------------------------------------------
class LcdCommRevC : public LcdComm {
public:
    explicit LcdCommRevC(std::unique_ptr<Transport> transport,
                         int displayWidth = 480, int displayHeight = 800);

    void initializeComm() override;
    void reset() override;
    void clear() override;
    void screenOff() override;
    void screenOn() override;
    void setBrightness(int level) override;
    void setOrientation(Orientation orientation) override;
    void displayPILImage(const Image& image, int x = 0, int y = 0,
                         int imageWidth = 0, int imageHeight = 0) override;

    int subRevision() const { return subRevision_; }   // 1=2", 2=5", 3=8"
    int romVersion() const { return romVersion_; }

private:
    // Command ids of the vendor protocol.
    enum : int {
        CmdHello = 0,
        CmdOptions,
        CmdRestart,
        CmdTurnOff,
        CmdTurnOn,
        CmdSetBrightness,
        CmdStopVideo,
        CmdStopMedia,
        CmdQueryStatus,
        CmdStartDisplayBitmap,
        CmdPreUpdateBitmap,
        CmdUpdateBitmap,
        CmdDisplayBitmap2Inch,
        CmdDisplayBitmap5Inch,
        CmdDisplayBitmap8Inch,
        CmdSendPayload,
        CmdStartModeDefault,
        CmdNoFlip,
        CmdSleepOff,
    };

    void sendCommand(int cmd, const std::vector<std::uint8_t>& payload = {},
                     bool bypassQueue = false, int readSize = 0);
    void hello();
    std::vector<std::uint8_t> generateFullImage(const Image& image) const;
    std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>
    generateUpdateImage(const Image& image, int x, int y, int count) const;

    int subRevision_ = 0;   // 0 unknown
    int romVersion_ = 87;
    int updateCount_ = 0;
};

// ---------------------------------------------------------------------------
// Revision D: Kipye Qiye Smart Display 3.5"
// 4-byte ASCII-ish commands, RGB565 big-endian, 63-byte chunks prefixed 0x50.
// ---------------------------------------------------------------------------
class LcdCommRevD : public LcdComm {
public:
    explicit LcdCommRevD(std::unique_ptr<Transport> transport,
                         int displayWidth = 320, int displayHeight = 480);

    void initializeComm() override;
    void reset() override;
    void clear() override;
    void screenOff() override;
    void screenOn() override;
    void setBrightness(int level) override;
    void setOrientation(Orientation orientation) override;
    void displayPILImage(const Image& image, int x = 0, int y = 0,
                         int imageWidth = 0, int imageHeight = 0) override;

private:
    void sendCommand(const std::vector<std::uint8_t>& cmd,
                     const std::vector<std::uint8_t>& payload = {});
};

// ---------------------------------------------------------------------------
// WeAct Studio Display FS V1: 3.5" (A) and 0.96" (B)
// CMD + payload + CMD_END framing, RGB565 little-endian, width*4 chunks.
// ---------------------------------------------------------------------------
class LcdCommWeAct : public LcdComm {
public:
    LcdCommWeAct(std::unique_ptr<Transport> transport, int displayWidth, int displayHeight);

    void initializeComm() override;
    void reset() override;
    void clear() override;
    void screenOff() override;
    void screenOn() override;
    void setBrightness(int level) override;
    void setOrientation(Orientation orientation) override;
    void displayPILImage(const Image& image, int x = 0, int y = 0,
                         int imageWidth = 0, int imageHeight = 0) override;

    const std::string& deviceVersion() const { return deviceVersion_; }

private:
    void sendCommand(const std::vector<std::uint8_t>& data);
    void full(Color color);

    int brightness_ = 0;
    std::string deviceVersion_;
};

class LcdCommWeActA : public LcdCommWeAct {
public:
    explicit LcdCommWeActA(std::unique_ptr<Transport> transport)
        : LcdCommWeAct(std::move(transport), 320, 480) {}
};

class LcdCommWeActB : public LcdCommWeAct {
public:
    explicit LcdCommWeActB(std::unique_ptr<Transport> transport)
        : LcdCommWeAct(std::move(transport), 80, 160) {}
};

// ---------------------------------------------------------------------------
// Turing native-USB models (4.6" / 5.2" / 8.0" / 8.8" / 9.2" / 12.3")
// DES-encrypted 512-byte packets over USB bulk endpoints.
// Only built when libusb is available (LCDCTL_WITH_LIBUSB).
// ---------------------------------------------------------------------------
class LcdCommTuringUSB : public LcdComm {
public:
    LcdCommTuringUSB();
    ~LcdCommTuringUSB() override;

    static bool isAvailable();

    void initializeComm() override;
    void reset() override;
    void clear() override;
    void screenOff() override;
    void screenOn() override;
    void setBrightness(int level) override;
    void setOrientation(Orientation orientation) override;
    void displayPILImage(const Image& image, int x = 0, int y = 0,
                         int imageWidth = 0, int imageHeight = 0) override;

    std::uint16_t productId() const { return productId_; }

private:
    // Vendor protocol helpers (see docs/ARCHITECTURE.md).
    std::vector<std::uint8_t> buildCommandPacketHeader(int commandId) const;
    std::vector<std::uint8_t> encryptCommandPacket(const std::vector<std::uint8_t>& data) const;
    std::vector<std::uint8_t> writeToDevice(const std::vector<std::uint8_t>& packet);
    void sendSyncCommand();
    void sendBrightnessCommand(int brightness);
    void sendImage(const std::vector<std::uint8_t>& pngData);
    void sendJpeg(const std::vector<std::uint8_t>& jpegData);
    void clearImage();
    std::vector<std::uint8_t> encodePng(const Image& image) const;
    std::vector<std::uint8_t> encodeJpegUnderLimit(const Image& image, std::size_t maxBytes,
                                                  int quality) const;

    std::uint16_t productId_ = 0;
    Image currentState_;
};

}  // namespace lcdctl
