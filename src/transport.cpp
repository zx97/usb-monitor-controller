// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/transport.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "lcdctl/logging.hpp"

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <ntddser.h>
#endif

#if defined(LCDCTL_WITH_LIBUSB)
#include <libusb.h>
#endif

namespace lcdctl {

// ===========================================================================
// USB CDC serial
// ===========================================================================

#if defined(_WIN32)

// ===========================================================================
// Windows: serial ports through the Win32 comm API
// ===========================================================================
namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    if (!wide.empty() && wide.back() == L'\0') {
        wide.pop_back();
    }
    return wide;
}

std::string narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    if (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    return result;
}

std::string registryString(HDEVINFO devices, SP_DEVINFO_DATA& info, DWORD property) {
    wchar_t buffer[512] = {0};
    DWORD type = 0;
    if (!::SetupDiGetDeviceRegistryPropertyW(devices, &info, property, &type,
                                             reinterpret_cast<PBYTE>(buffer), sizeof(buffer),
                                             nullptr)) {
        return {};
    }
    return narrow(buffer);
}

// "USB\VID_1A86&PID_5722&REV_0264" -> vid/pid of the first hardware id.
void parseHardwareId(const std::string& hardwareId, SerialPortInfo& info) {
    const std::size_t vidPosition = hardwareId.find("VID_");
    if (vidPosition != std::string::npos && hardwareId.size() >= vidPosition + 8) {
        info.vid = static_cast<std::uint16_t>(
            std::stoul(hardwareId.substr(vidPosition + 4, 4), nullptr, 16));
    }
    const std::size_t pidPosition = hardwareId.find("PID_");
    if (pidPosition != std::string::npos && hardwareId.size() >= pidPosition + 8) {
        info.pid = static_cast<std::uint16_t>(
            std::stoul(hardwareId.substr(pidPosition + 4, 4), nullptr, 16));
    }
}

// "USB-SERIAL CH340 (COM3)" / "USB Serial Device (COM4)" -> "COM3".
std::string portFromFriendlyName(const std::string& friendlyName) {
    const std::size_t open = friendlyName.rfind('(');
    const std::size_t close = friendlyName.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open + 1) {
        return {};
    }
    return friendlyName.substr(open + 1, close - open - 1);
}

}  // namespace

std::vector<SerialPortInfo> SerialTransport::enumerate() {
    std::vector<SerialPortInfo> ports;
    HDEVINFO devices = ::SetupDiGetClassDevsW(&GUID_DEVINTERFACE_COMPORT, nullptr, nullptr,
                                              DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        return ports;
    }

    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);
    for (DWORD index = 0;
         ::SetupDiEnumDeviceInterfaces(devices, nullptr, &GUID_DEVINTERFACE_COMPORT, index,
                                       &interfaceData);
         ++index) {
        DWORD required = 0;
        ::SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &required, nullptr);
        if (required == 0) {
            continue;
        }
        std::vector<unsigned char> storage(required);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA deviceInfo{};
        deviceInfo.cbSize = sizeof(deviceInfo);
        if (!::SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, required, nullptr,
                                                &deviceInfo)) {
            continue;
        }

        const std::string friendly = registryString(devices, deviceInfo, SPDRP_FRIENDLYNAME);
        const std::string portName = portFromFriendlyName(friendly);
        if (portName.empty()) {
            continue;
        }

        SerialPortInfo info;
        info.device = portName;
        info.product = friendly;
        const std::string hardwareId = registryString(devices, deviceInfo, SPDRP_HARDWAREID);
        parseHardwareId(hardwareId, info);

        // Device path: \\?\usb#vid_1a86&pid_5722#<serial>#{...}
        const std::string path = narrow(detail->DevicePath);
        const std::size_t firstHash = path.find('#');
        const std::size_t secondHash =
            firstHash == std::string::npos ? std::string::npos : path.find('#', firstHash + 1);
        if (secondHash != std::string::npos) {
            const std::size_t end = path.find('#', secondHash + 1);
            info.serial = path.substr(secondHash + 1, end - secondHash - 1);
        }
        ports.push_back(std::move(info));
    }
    ::SetupDiDestroyDeviceInfoList(devices);
    return ports;
}

SerialTransport::~SerialTransport() { close(); }

void SerialTransport::close() {
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

bool SerialTransport::open(const std::string& port, int baud, bool rtscts, int attempts,
                           int delaySeconds) {
    port_ = port;
    baud_ = baud;
    rtscts_ = rtscts;

    std::string path = port;
    if (path.rfind("\\\\.\\", 0) != 0 && path.rfind("\\\\?\\", 0) != 0) {
        path = "\\\\.\\" + path;   // \\.\COM3
    }

    for (int attempt = 1; attempt <= attempts; ++attempt) {
        HANDLE handle = ::CreateFileW(widen(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            DCB dcb{};
            dcb.DCBlength = sizeof(dcb);
            if (::GetCommState(handle, &dcb)) {
                dcb.BaudRate = static_cast<DWORD>(baud);
                dcb.ByteSize = 8;
                dcb.Parity = NOPARITY;
                dcb.StopBits = ONESTOPBIT;
                dcb.fBinary = TRUE;
                dcb.fParity = FALSE;
                dcb.fOutxCtsFlow = rtscts ? TRUE : FALSE;
                dcb.fRtsControl = rtscts ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
                dcb.fDtrControl = DTR_CONTROL_ENABLE;
                ::SetCommState(handle, &dcb);
                ::SetupComm(handle, 8192, 8192);
                ::PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
                handle_ = handle;
                LCDCTL_DEBUG << "Opened serial port " << port << " @" << baud;
                return true;
            }
            ::CloseHandle(handle);
        }
        LCDCTL_WARN << "Cannot open " << port << " (error " << ::GetLastError() << "), retry "
                    << attempt << "/" << attempts;
        if (attempt < attempts && delaySeconds > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(delaySeconds));
        }
    }
    return false;
}

bool SerialTransport::write(const std::uint8_t* data, std::size_t len) {
    if (handle_ == nullptr) {
        return false;
    }
    std::size_t written = 0;
    while (written < len) {
        DWORD sent = 0;
        if (!::WriteFile(static_cast<HANDLE>(handle_), data + written,
                         static_cast<DWORD>(len - written), &sent, nullptr)) {
            LCDCTL_ERROR << "serial write failed (error " << ::GetLastError() << ")";
            return false;
        }
        written += sent;
    }
    return true;
}

std::vector<std::uint8_t> SerialTransport::read(std::size_t len, int timeoutMs) {
    std::vector<std::uint8_t> out;
    if (handle_ == nullptr || len == 0) {
        return out;
    }
    COMMTIMEOUTS timeouts{};
    // ReadIntervalTimeout = MAXDWORD with a total constant returns as soon as
    // something is available, and waits at most timeoutMs otherwise.
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = static_cast<DWORD>(timeoutMs);
    ::SetCommTimeouts(static_cast<HANDLE>(handle_), &timeouts);

    out.resize(len);
    DWORD received = 0;
    if (!::ReadFile(static_cast<HANDLE>(handle_), out.data(), static_cast<DWORD>(len), &received,
                    nullptr)) {
        out.clear();
        return out;
    }
    out.resize(received);
    return out;
}

std::vector<std::uint8_t> SerialTransport::readAll() {
    std::vector<std::uint8_t> out;
    if (handle_ == nullptr) {
        return out;
    }
    DWORD errors = 0;
    COMSTAT status{};
    if (!::ClearCommError(static_cast<HANDLE>(handle_), &errors, &status)) {
        return out;
    }
    if (status.cbInQue == 0) {
        return out;
    }
    return read(status.cbInQue, 200);
}

void SerialTransport::flushInput() {
    if (handle_ != nullptr) {
        ::PurgeComm(static_cast<HANDLE>(handle_), PURGE_RXCLEAR);
    }
}

bool SerialTransport::reopen() {
    if (port_.empty()) {
        return false;
    }
    close();
    return open(port_, baud_, rtscts_, 10, 1);
}

#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)

namespace {

// Resolve the USB VID/PID/serial of a tty device through sysfs (Linux only).
bool sysfsUsbInfo(const std::string& name, SerialPortInfo& info) {
#if defined(__linux__)
    std::array<char, PATH_MAX> resolved{};
    const std::string link = "/sys/class/tty/" + name + "/device";
    if (realpath(link.c_str(), resolved.data()) == nullptr) {
        return false;
    }
    const std::string iface(resolved.data());  // .../usb1/1-2/1-2:1.0
    // The interface's own uevent carries PRODUCT=vid/pid/bcd; the USB device
    // (parent directory) carries the serial/product strings.
    const std::size_t slash = iface.find_last_of('/');
    const std::string deviceDir = (slash == std::string::npos) ? iface : iface.substr(0, slash);

    std::ifstream uevent(iface + "/uevent");
    std::string line;
    while (std::getline(uevent, line)) {
        if (line.rfind("PRODUCT=", 0) == 0) {
            const std::string product = line.substr(8);
            const std::size_t slash1 = product.find('/');
            const std::size_t slash2 = product.find('/', slash1 + 1);
            if (slash1 != std::string::npos) {
                info.vid = static_cast<std::uint16_t>(std::stoul(product.substr(0, slash1), nullptr, 16));
                const std::string pidStr = product.substr(slash1 + 1, slash2 - slash1 - 1);
                info.pid = static_cast<std::uint16_t>(std::stoul(pidStr, nullptr, 16));
            }
        }
    }

    // Fallbacks: some kernels/UDCs expose the ids only as attributes.
    if (info.vid == 0) {
        std::ifstream vid(deviceDir + "/idVendor");
        if (vid.good()) {
            std::string value;
            vid >> value;
            info.vid = static_cast<std::uint16_t>(std::stoul(value, nullptr, 16));
        }
    }
    if (info.pid == 0) {
        std::ifstream pid(deviceDir + "/idProduct");
        if (pid.good()) {
            std::string value;
            pid >> value;
            info.pid = static_cast<std::uint16_t>(std::stoul(value, nullptr, 16));
        }
    }

    std::ifstream serial(deviceDir + "/serial");
    if (serial.good()) {
        std::getline(serial, info.serial);
    }
    std::ifstream product(deviceDir + "/product");
    if (product.good()) {
        std::getline(product, info.product);
    }
    return true;
#else
    (void)name;
    (void)info;
    return false;
#endif
}

}  // namespace

std::vector<SerialPortInfo> SerialTransport::enumerate() {
    std::vector<SerialPortInfo> ports;
#if defined(__linux__)
    ::glob_t glob_result{};
    if (::glob("/sys/class/tty/ttyACM*", 0, nullptr, &glob_result) == 0) {
        for (std::size_t i = 0; i < glob_result.gl_pathc; ++i) {
            const std::string path(glob_result.gl_pathv[i]);
            const std::string name = path.substr(path.find_last_of('/') + 1);
            SerialPortInfo info;
            info.device = "/dev/" + name;
            sysfsUsbInfo(name, info);
            ports.push_back(info);
        }
    }
    ::globfree(&glob_result);
    glob_result = {};
    if (::glob("/sys/class/tty/ttyUSB*", 0, nullptr, &glob_result) == 0) {
        for (std::size_t i = 0; i < glob_result.gl_pathc; ++i) {
            const std::string path(glob_result.gl_pathv[i]);
            const std::string name = path.substr(path.find_last_of('/') + 1);
            SerialPortInfo info;
            info.device = "/dev/" + name;
            sysfsUsbInfo(name, info);
            ports.push_back(info);
        }
    }
    ::globfree(&glob_result);
#else
    // Best effort on non-Linux: assume the usual device nodes.
    for (const char* pattern : {"/dev/tty.usbmodem*", "/dev/tty.usbserial*"}) {
        ::glob_t glob_result{};
        if (::glob(pattern, 0, nullptr, &glob_result) == 0) {
            for (std::size_t i = 0; i < glob_result.gl_pathc; ++i) {
                SerialPortInfo info;
                info.device = glob_result.gl_pathv[i];
                ports.push_back(info);
            }
        }
        ::globfree(&glob_result);
    }
#endif
    return ports;
}

SerialTransport::~SerialTransport() { close(); }

void SerialTransport::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool SerialTransport::open(const std::string& port, int baud, bool rtscts, int attempts,
                           int delaySeconds) {
    port_ = port;
    baud_ = baud;
    rtscts_ = rtscts;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        const int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0) {
            struct termios tty {};
            if (::tcgetattr(fd, &tty) == 0) {
                ::cfmakeraw(&tty);
                const speed_t speed = (baud == 9600)     ? B9600
                                      : (baud == 57600)  ? B57600
                                      : (baud == 230400) ? B230400
                                                         : B115200;
                ::cfsetispeed(&tty, speed);
                ::cfsetospeed(&tty, speed);
                tty.c_cflag |= (CLOCAL | CREAD);
                tty.c_cflag &= ~CSTOPB;
                tty.c_cflag &= ~PARENB;
                tty.c_cflag &= ~CSIZE;
                tty.c_cflag |= CS8;
                if (rtscts) {
                    tty.c_cflag |= CRTSCTS;
                } else {
                    tty.c_cflag &= ~CRTSCTS;
                }
                // VMIN=0 / VTIME=1 => read() returns after ~100 ms even with no data;
                // the read() implementation uses select() for real timeouts.
                tty.c_cc[VMIN] = 0;
                tty.c_cc[VTIME] = 1;
                ::tcsetattr(fd, TCSANOW, &tty);
                ::tcflush(fd, TCIOFLUSH);
                // Back to blocking mode: select() drives the timeouts.
                const int flags = ::fcntl(fd, F_GETFL, 0);
                ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                fd_ = fd;
                LCDCTL_DEBUG << "Opened serial port " << port << " @" << baud;
                return true;
            }
            ::close(fd);
        }
        LCDCTL_WARN << "Cannot open " << port << " (" << std::strerror(errno) << "), retry "
                    << attempt << "/" << attempts;
        if (attempt < attempts && delaySeconds > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(delaySeconds));
        }
    }
    return false;
}

bool SerialTransport::write(const std::uint8_t* data, std::size_t len) {
    if (fd_ < 0) {
        return false;
    }
    std::size_t written = 0;
    while (written < len) {
        const ssize_t result = ::write(fd_, data + written, len - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            LCDCTL_ERROR << "serial write failed: " << std::strerror(errno);
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

std::vector<std::uint8_t> SerialTransport::read(std::size_t len, int timeoutMs) {
    std::vector<std::uint8_t> out;
    if (fd_ < 0 || len == 0) {
        return out;
    }
    out.reserve(len);

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd_, &readSet);
    struct timeval timeout {};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;

    const int ready = ::select(fd_ + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        return out;
    }

    std::vector<std::uint8_t> buffer(len);
    const ssize_t result = ::read(fd_, buffer.data(), len);
    if (result > 0) {
        out.assign(buffer.begin(), buffer.begin() + result);
    }
    return out;
}

std::vector<std::uint8_t> SerialTransport::readAll() {
    std::vector<std::uint8_t> out;
    if (fd_ < 0) {
        return out;
    }
    std::uint8_t buffer[4096];
    // Drain whatever is already buffered, without blocking.
    for (;;) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd_, &readSet);
        struct timeval timeout {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 20000;
        const int ready = ::select(fd_ + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            break;
        }
        const ssize_t result = ::read(fd_, buffer, sizeof(buffer));
        if (result <= 0) {
            break;
        }
        out.insert(out.end(), buffer, buffer + result);
    }
    return out;
}

void SerialTransport::flushInput() {
    if (fd_ >= 0) {
        ::tcflush(fd_, TCIFLUSH);
    }
}

bool SerialTransport::reopen() {
    if (port_.empty()) {
        return false;
    }
    close();
    return open(port_, baud_, rtscts_, 10, 1);
}

#else  // !POSIX

// Non-POSIX builds: serial support is not implemented in this port.
std::vector<SerialPortInfo> SerialTransport::enumerate() { return {}; }
SerialTransport::~SerialTransport() = default;
void SerialTransport::close() {}
bool SerialTransport::open(const std::string&, int, bool, int, int) { return false; }
bool SerialTransport::write(const std::uint8_t*, std::size_t) { return false; }
std::vector<std::uint8_t> SerialTransport::read(std::size_t, int) { return {}; }
std::vector<std::uint8_t> SerialTransport::readAll() { return {}; }
void SerialTransport::flushInput() {}
bool SerialTransport::reopen() { return false; }

#endif

// ===========================================================================
// Native USB (Turing TUR_USB models)
// ===========================================================================

const std::vector<UsbProductInfo>& turingUsbProducts() {
    static const std::vector<UsbProductInfo> products = {
        {0x0028, 480, 480, "Turing 2.8\" round"},
        {0x0046, 320, 960, "Turing 4.6\""},
        {0x0050, 720, 1280, "Turing 5.2\""},
        {0x0080, 800, 1280, "Turing 8.0\""},
        {0x0088, 480, 1920, "Turing 8.8\""},
        {0x0092, 462, 1920, "Turing 9.2\""},
        {0x0123, 720, 1920, "Turing 12.3\""},
    };
    return products;
}

#if defined(LCDCTL_WITH_LIBUSB)

namespace {

::libusb_context* usbContext() {
    static ::libusb_context* context = nullptr;
    static bool initialized = false;
    if (!initialized) {
        if (::libusb_init(&context) != 0) {
            context = nullptr;
        }
        initialized = true;
    }
    return context;
}

}  // namespace

bool UsbTransport::available() { return usbContext() != nullptr; }

std::vector<UsbDeviceInfo> UsbTransport::enumerate() {
    std::vector<UsbDeviceInfo> devices;
    ::libusb_context* context = usbContext();
    if (!context) {
        return devices;
    }
    ::libusb_device** list = nullptr;
    const ssize_t count = ::libusb_get_device_list(context, &list);
    if (count < 0) {
        return devices;
    }
    for (ssize_t i = 0; i < count; ++i) {
        ::libusb_device_descriptor descriptor{};
        if (::libusb_get_device_descriptor(list[i], &descriptor) != 0) {
            continue;
        }
        if (descriptor.idVendor != TUR_USB_VENDOR_ID) {
            continue;
        }
        for (const UsbProductInfo& product : turingUsbProducts()) {
            if (product.pid == descriptor.idProduct) {
                devices.push_back({descriptor.idVendor, descriptor.idProduct, product.width,
                                   product.height});
                break;
            }
        }
    }
    ::libusb_free_device_list(list, 1);
    return devices;
}

UsbTransport::~UsbTransport() { close(); }

void UsbTransport::close() {
    auto* handle = static_cast<::libusb_device_handle*>(handle_);
    if (handle) {
        ::libusb_release_interface(handle, 0);
        ::libusb_attach_kernel_driver(handle, 0);
        ::libusb_close(handle);
        handle_ = nullptr;
    }
}

bool UsbTransport::open(std::uint16_t vid, std::uint16_t pid) {
    ::libusb_context* context = usbContext();
    if (!context) {
        LCDCTL_ERROR << "libusb could not be initialized";
        return false;
    }
    auto* handle = ::libusb_open_device_with_vid_pid(context, vid, pid);
    if (!handle) {
        return false;
    }
    if (::libusb_kernel_driver_active(handle, 0) == 1) {
        ::libusb_detach_kernel_driver(handle, 0);
    }
    if (::libusb_set_configuration(handle, 1) != 0) {
        LCDCTL_WARN << "libusb set_configuration failed (continuing)";
    }
    if (::libusb_claim_interface(handle, 0) != 0) {
        LCDCTL_ERROR << "libusb claim_interface failed";
        ::libusb_close(handle);
        return false;
    }

    // Locate the first bulk OUT / bulk IN endpoints of interface 0.
    ::libusb_config_descriptor* config = nullptr;
    if (::libusb_get_active_config_descriptor(::libusb_get_device(handle), &config) != 0 ||
        config == nullptr) {
        LCDCTL_ERROR << "cannot read USB configuration";
        ::libusb_release_interface(handle, 0);
        ::libusb_close(handle);
        return false;
    }
    endpointOut_ = 0;
    endpointIn_ = 0;
    for (int i = 0; i < config->bNumInterfaces; ++i) {
        const ::libusb_interface& interface = config->interface[i];
        for (int j = 0; j < interface.num_altsetting; ++j) {
            const ::libusb_interface_descriptor& altsetting = interface.altsetting[j];
            for (int k = 0; k < altsetting.bNumEndpoints; ++k) {
                const ::libusb_endpoint_descriptor& endpoint = altsetting.endpoint[k];
                const std::uint8_t type = endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                if (type != LIBUSB_TRANSFER_TYPE_BULK) {
                    continue;
                }
                if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT) {
                    if (!endpointOut_) endpointOut_ = endpoint.bEndpointAddress;
                } else {
                    if (!endpointIn_) endpointIn_ = endpoint.bEndpointAddress;
                }
            }
        }
    }
    ::libusb_free_config_descriptor(config);

    if (!endpointOut_ || !endpointIn_) {
        LCDCTL_ERROR << "USB bulk endpoints not found";
        ::libusb_release_interface(handle, 0);
        ::libusb_close(handle);
        return false;
    }

    handle_ = handle;
    pid_ = pid;
    LCDCTL_DEBUG << "Opened native USB device " << std::hex << vid << ":" << pid << std::dec;
    return true;
}

bool UsbTransport::openFirst(std::uint16_t& pid, int& width, int& height) {
    const std::vector<UsbDeviceInfo> devices = enumerate();
    if (devices.empty()) {
        return false;
    }
    if (!open(devices.front().vid, devices.front().pid)) {
        return false;
    }
    pid = devices.front().pid;
    width = devices.front().width;
    height = devices.front().height;
    return true;
}

bool UsbTransport::write(const std::uint8_t* data, std::size_t len) {
    auto* handle = static_cast<::libusb_device_handle*>(handle_);
    if (!handle || !endpointOut_) {
        return false;
    }
    int transferred = 0;
    const int result =
        ::libusb_bulk_transfer(handle, endpointOut_, const_cast<std::uint8_t*>(data),
                               static_cast<int>(len), &transferred, 2000);
    if (result != 0) {
        LCDCTL_ERROR << "USB write error: " << ::libusb_error_name(result);
        return false;
    }
    return transferred == static_cast<int>(len);
}

std::vector<std::uint8_t> UsbTransport::read(std::size_t len, int timeoutMs) {
    auto* handle = static_cast<::libusb_device_handle*>(handle_);
    std::vector<std::uint8_t> out;
    if (!handle || !endpointIn_) {
        return out;
    }
    out.resize(len);
    int transferred = 0;
    const int result = ::libusb_bulk_transfer(handle, endpointIn_, out.data(),
                                              static_cast<int>(len), &transferred, timeoutMs);
    if (result != 0) {
        out.clear();
        return out;
    }
    out.resize(static_cast<std::size_t>(transferred));
    return out;
}

std::vector<std::uint8_t> UsbTransport::readAll() { return read(512, 100); }

#else  // !LCDCTL_WITH_LIBUSB

bool UsbTransport::available() { return false; }
std::vector<UsbDeviceInfo> UsbTransport::enumerate() { return {}; }
UsbTransport::~UsbTransport() = default;
void UsbTransport::close() {}
bool UsbTransport::open(std::uint16_t, std::uint16_t) { return false; }
bool UsbTransport::openFirst(std::uint16_t&, int&, int&) { return false; }
bool UsbTransport::write(const std::uint8_t*, std::size_t) { return false; }
std::vector<std::uint8_t> UsbTransport::read(std::size_t, int) { return {}; }
std::vector<std::uint8_t> UsbTransport::readAll() { return {}; }

#endif

}  // namespace lcdctl
