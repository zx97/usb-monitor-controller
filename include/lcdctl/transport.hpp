// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - byte transports (USB CDC serial and native USB).
//
// Ported from library/lcd/lcd_comm.py (pyserial) and
// library/lcd/lcd_comm_turing_usb.py (pyusb) of
// https://github.com/mathoudebine/turing-smart-screen-python/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lcdctl {

// Transport interface used by every driver. All screens ultimately exchange
// opaque byte buffers, whether over a virtual serial port or a USB endpoint.
class Transport {
public:
    virtual ~Transport() = default;
    virtual bool isOpen() const = 0;
    virtual void close() = 0;
    virtual bool write(const std::uint8_t* data, std::size_t len) = 0;
    virtual std::vector<std::uint8_t> read(std::size_t len, int timeoutMs = 1000) = 0;
    virtual std::vector<std::uint8_t> readAll() = 0;
    virtual void flushInput() {}

    bool write(const std::vector<std::uint8_t>& data) {
        return write(data.data(), data.size());
    }

    // Device identity (e.g. serial port path); empty when not meaningful.
    virtual std::string deviceName() const { return ""; }

    // Re-open the underlying device, e.g. after a hardware reset made it
    // disconnect and re-enumerate.
    virtual bool reopen() { return isOpen(); }
};

// ---------------------------------------------------------------------------
// USB CDC serial (revisions A, B, C, D, WeAct)
// ---------------------------------------------------------------------------

struct SerialPortInfo {
    std::string device;   // e.g. /dev/ttyACM0
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    std::string serial;   // USB iSerialNumber
    std::string product;
};

class SerialTransport : public Transport {
public:
    SerialTransport() = default;
    ~SerialTransport() override;

    // Opens `port` at 115200/8N1 with RTS/CTS by default. Retries `attempts`
    // times (`delaySeconds` apart) because the screen often resets itself and
    // re-enumerates right after the program starts. Returns false on failure.
    bool open(const std::string& port, int baud = 115200, bool rtscts = true,
              int attempts = 10, int delaySeconds = 1);

    bool isOpen() const override { return fd_ >= 0; }
    void close() override;
    bool write(const std::uint8_t* data, std::size_t len) override;
    std::vector<std::uint8_t> read(std::size_t len, int timeoutMs = 1000) override;
    std::vector<std::uint8_t> readAll() override;
    void flushInput() override;

    const std::string& portName() const { return port_; }

    std::string deviceName() const override { return port_; }
    bool reopen() override;

    // Enumerate serial ports with their USB identifiers (Linux: sysfs; other
    // platforms: best-effort device node globbing).
    static std::vector<SerialPortInfo> enumerate();

private:
    int fd_ = -1;
    std::string port_;
    int baud_ = 115200;
    bool rtscts_ = true;
};

// ---------------------------------------------------------------------------
// Native USB (Turing "TUR_USB" models)
// ---------------------------------------------------------------------------

// USB IDs of the native-USB Turing models (portrait resolution in brackets).
constexpr std::uint16_t TUR_USB_VENDOR_ID = 0x1cbe;

struct UsbProductInfo {
    std::uint16_t pid;
    int width;
    int height;
    const char* model;
};

// Product ids known to use the native-USB vendor protocol.
const std::vector<UsbProductInfo>& turingUsbProducts();

struct UsbDeviceInfo {
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    int width = 0;
    int height = 0;
};

class UsbTransport : public Transport {
public:
    UsbTransport() = default;
    ~UsbTransport() override;

    // Returns false when built without libusb.
    static bool available();

    // Find the first connected Turing native-USB screen and open it.
    // `pid`/`width`/`height` are filled with what was detected.
    bool openFirst(std::uint16_t& pid, int& width, int& height);
    bool open(std::uint16_t vid, std::uint16_t pid);

    bool isOpen() const override { return handle_ != nullptr; }
    void close() override;
    bool write(const std::uint8_t* data, std::size_t len) override;
    std::vector<std::uint8_t> read(std::size_t len, int timeoutMs = 2000) override;
    std::vector<std::uint8_t> readAll() override;

    static std::vector<UsbDeviceInfo> enumerate();

private:
    void* handle_ = nullptr;  // libusb_device_handle*, opaque to avoid the header here
    std::uint16_t pid_ = 0;
    std::uint8_t endpointOut_ = 0;
    std::uint8_t endpointIn_ = 0;
};

}  // namespace lcdctl
