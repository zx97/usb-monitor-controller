// SPDX-License-Identifier: GPL-3.0-or-later
//
// Windows sensor provider.
//
// Sources: GetSystemTimes (CPU %), the registry (CPU MHz), GlobalMemoryStatusEx
// (RAM), GetDiskFreeSpaceExW (disk), GetIfTable2 (network), GetTickCount64
// (uptime), NVML (nvml.dll, GPU) and optionally LibreHardwareMonitor
// (http://127.0.0.1:8085/data.json) for temperatures and fans, which is what the
// Python project uses on Windows too.

// Windows 10 APIs (GetIfTable2, GetAdaptersAddresses, GetSystemTimes).
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "lcdctl/logging.hpp"
#include "lcdctl/sensors.hpp"

namespace lcdctl {
namespace {

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

std::string readRegistryString(HKEY root, const wchar_t* path, const wchar_t* value) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t buffer[512] = {0};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const LONG status = ::RegQueryValueExW(key, value, nullptr, &type,
                                           reinterpret_cast<LPBYTE>(buffer), &size);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    return narrow(buffer);
}

bool readRegistryDword(HKEY root, const wchar_t* path, const wchar_t* value, DWORD& out) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD size = sizeof(out);
    DWORD type = 0;
    const LONG status =
        ::RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<LPBYTE>(&out), &size);
    ::RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_DWORD;
}

// --- Minimal HTTP GET for LibreHardwareMonitor -----------------------------
bool httpGet(const std::string& host, unsigned short port, const std::string& path,
             std::string& response) {
    SOCKET socketHandle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET) {
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    address.sin_addr.s_addr = ::inet_addr(host.c_str());
    DWORD timeout = 800;
    ::setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    if (::connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::closesocket(socketHandle);
        return false;
    }
    const std::string request = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\n\r\n";
    if (::send(socketHandle, request.c_str(), static_cast<int>(request.size()), 0) <= 0) {
        ::closesocket(socketHandle);
        return false;
    }
    char buffer[8192];
    for (;;) {
        const int received = ::recv(socketHandle, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(received));
        if (response.size() > 4 * 1024 * 1024) {
            break;
        }
    }
    ::closesocket(socketHandle);
    const std::size_t body = response.find("\r\n\r\n");
    if (body != std::string::npos) {
        response.erase(0, body + 4);
    }
    return !response.empty();
}

// "Value":"42,0 °C" following "Text":"CPU Package" -> 42.0
bool sensorValue(const std::string& json, const std::string& sensor, double& out) {
    const std::string key = "\"Text\":\"" + sensor + "\"";
    const std::size_t position = json.find(key);
    if (position == std::string::npos) {
        return false;
    }
    const std::size_t value = json.find("\"Value\":\"", position);
    if (value == std::string::npos || value - position > 2000) {
        return false;
    }
    const std::size_t start = value + 9;
    const std::size_t end = json.find('"', start);
    if (end == std::string::npos) {
        return false;
    }
    std::string text = json.substr(start, end - start);
    std::replace(text.begin(), text.end(), ',', '.');
    // Drop the unit ("42.0 °C", "1250 RPM", "30 %").
    char* stop = nullptr;
    const double parsed = std::strtod(text.c_str(), &stop);
    if (stop == text.c_str()) {
        return false;
    }
    out = parsed;
    return true;
}

// --- NVML through nvml.dll -------------------------------------------------
using NvmlDevice = void*;

struct NvmlApi {
    bool loaded = false;
    int (*init)() = nullptr;
    int (*shutdown)() = nullptr;
    int (*handleByIndex)(unsigned, NvmlDevice*) = nullptr;
    int (*memoryInfo)(NvmlDevice, void*) = nullptr;
    int (*utilizationRates)(NvmlDevice, void*) = nullptr;
    int (*temperature)(NvmlDevice, int, unsigned*) = nullptr;
    int (*fanSpeed)(NvmlDevice, unsigned*) = nullptr;
    int (*clockInfo)(NvmlDevice, int, unsigned*) = nullptr;
    int (*deviceName)(NvmlDevice, char*, unsigned) = nullptr;
    bool initialized = false;
};

NvmlApi& nvml() {
    static NvmlApi api = [] {
        NvmlApi result;
        HMODULE module = ::LoadLibraryA("nvml.dll");
        if (module == nullptr) {
            return result;
        }
        auto symbol = [module](const char* name) {
            return reinterpret_cast<void*>(::GetProcAddress(module, name));
        };
        result.init = reinterpret_cast<int (*)()>(symbol("nvmlInit_v2"));
        result.shutdown = reinterpret_cast<int (*)()>(symbol("nvmlShutdown"));
        result.handleByIndex = reinterpret_cast<int (*)(unsigned, NvmlDevice*)>(
            symbol("nvmlDeviceGetHandleByIndex_v2"));
        result.memoryInfo =
            reinterpret_cast<int (*)(NvmlDevice, void*)>(symbol("nvmlDeviceGetMemoryInfo"));
        result.utilizationRates = reinterpret_cast<int (*)(NvmlDevice, void*)>(
            symbol("nvmlDeviceGetUtilizationRates"));
        result.temperature = reinterpret_cast<int (*)(NvmlDevice, int, unsigned*)>(
            symbol("nvmlDeviceGetTemperature"));
        result.fanSpeed =
            reinterpret_cast<int (*)(NvmlDevice, unsigned*)>(symbol("nvmlDeviceGetFanSpeed"));
        result.clockInfo = reinterpret_cast<int (*)(NvmlDevice, int, unsigned*)>(
            symbol("nvmlDeviceGetClockInfo"));
        result.deviceName =
            reinterpret_cast<int (*)(NvmlDevice, char*, unsigned)>(symbol("nvmlDeviceGetName"));
        result.loaded = result.init != nullptr && result.handleByIndex != nullptr &&
                        result.memoryInfo != nullptr && result.utilizationRates != nullptr;
        return result;
    }();
    return api;
}

struct NvmlMemory {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

struct NvmlUtilization {
    unsigned int gpu;
    unsigned int memory;
};

bool sampleNvml(GpuMetrics& gpu) {
    NvmlApi& api = nvml();
    if (!api.loaded) {
        return false;
    }
    if (!api.initialized) {
        if (api.init() != 0) {
            return false;
        }
        api.initialized = true;
    }
    NvmlDevice device = nullptr;
    if (api.handleByIndex(0, &device) != 0 || device == nullptr) {
        return false;
    }
    NvmlMemory memory{};
    if (api.memoryInfo(device, &memory) == 0 && memory.total > 0) {
        gpu.memoryTotalMb = static_cast<double>(memory.total) / (1024.0 * 1024.0);
        gpu.memoryUsedMb = static_cast<double>(memory.used) / (1024.0 * 1024.0);
        gpu.memoryPercent =
            gpu.memoryTotalMb > 0 ? gpu.memoryUsedMb / gpu.memoryTotalMb * 100.0 : 0.0;
    }
    NvmlUtilization utilization{};
    if (api.utilizationRates(device, &utilization) == 0) {
        gpu.load = utilization.gpu;
    }
    unsigned temperature = 0;
    if (api.temperature && api.temperature(device, 0, &temperature) == 0) {
        gpu.temperatureC = temperature;
    }
    unsigned fan = 0;
    if (api.fanSpeed && api.fanSpeed(device, &fan) == 0) {
        gpu.fanPercent = fan;
    }
    unsigned clockMhz = 0;
    if (api.clockInfo && api.clockInfo(device, 0, &clockMhz) == 0) {
        gpu.frequencyMhz = clockMhz;
    }
    if (api.deviceName) {
        char name[96] = {0};
        if (api.deviceName(device, name, sizeof(name)) == 0) {
            gpu.model = name;
        }
    }
    gpu.available = true;
    return true;
}

std::string windowsDrive(const std::string& configured) {
    if (!configured.empty()) {
        return configured;
    }
    wchar_t directory[MAX_PATH] = {0};
    if (::GetWindowsDirectoryW(directory, MAX_PATH) > 0) {
        return narrow(directory).substr(0, 3);  // "C:\"
    }
    return "C:\\";
}

class WindowsProvider : public SensorProvider {
public:
    explicit WindowsProvider(const SensorSettings& settings) : settings_(settings) {
        WSADATA data{};
        ::WSAStartup(MAKEWORD(2, 2), &data);
    }

    ~WindowsProvider() override { ::WSACleanup(); }

    std::string name() const override { return "windows"; }

    void refresh(Metrics& metrics) override {
        metrics = Metrics{};
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = lastSample_.time_since_epoch().count() == 0
                                   ? 1.0
                                   : std::chrono::duration<double>(now - lastSample_).count();
        lastSample_ = now;

        readCpu(metrics, elapsed);
        readMemory(metrics);
        readDisk(metrics);
        readNet(metrics, std::max(0.001, elapsed));
        readLhm(metrics);

        metrics.gpu.available = false;
        sampleNvml(metrics.gpu);

        metrics.dateDay = formatDateText(settings_.dateDayFormat, false);
        metrics.dateHour = formatDateText(settings_.dateHourFormat, true);
        metrics.uptimeSeconds = ::GetTickCount64() / 1000;

        for (const auto& custom : settings_.customCommands) {
            CustomMetrics value;
            if (std::FILE* pipe = _popen(custom.second.c_str(), "r")) {
                char line[512] = {0};
                if (std::fgets(line, sizeof(line), pipe) != nullptr) {
                    std::string text(line);
                    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                        text.pop_back();
                    }
                    value.text = text;
                    char* stop = nullptr;
                    const double parsed = std::strtod(text.c_str(), &stop);
                    if (stop != text.c_str()) {
                        value.numeric = parsed;
                    }
                }
                _pclose(pipe);
            }
            metrics.custom[custom.first] = value;
        }
    }

private:
    void readCpu(Metrics& metrics, double elapsed) {
        FILETIME idleTime{}, kernelTime{}, userTime{};
        if (::GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
            auto toUint64 = [](const FILETIME& time) {
                return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) |
                       time.dwLowDateTime;
            };
            const std::uint64_t idle = toUint64(idleTime);
            const std::uint64_t total = toUint64(kernelTime) + toUint64(userTime);
            if (previousTotal_ > 0 && total > previousTotal_) {
                const std::uint64_t deltaTotal = total - previousTotal_;
                const std::uint64_t deltaIdle = idle - previousIdle_;
                if (deltaTotal > 0) {
                    metrics.cpu.percent =
                        (1.0 - static_cast<double>(deltaIdle) / deltaTotal) * 100.0;
                    metrics.cpu.percent = std::clamp(metrics.cpu.percent, 0.0, 100.0);
                }
            }
            previousTotal_ = total;
            previousIdle_ = idle;
        }

        // No loadavg on Windows: keep exponentially smoothed averages so themes
        // that show 1/5/15 min values still display something coherent.
        if (metrics.cpu.percent >= 0.0) {
            const double alpha1 = std::clamp(elapsed / 60.0, 0.01, 1.0);
            const double alpha5 = std::clamp(elapsed / 300.0, 0.002, 1.0);
            const double alpha15 = std::clamp(elapsed / 900.0, 0.001, 1.0);
            load1_ += (metrics.cpu.percent - load1_) * alpha1;
            load5_ += (metrics.cpu.percent - load5_) * alpha5;
            load15_ += (metrics.cpu.percent - load15_) * alpha15;
        }
        metrics.cpu.load1 = load1_;
        metrics.cpu.load5 = load5_;
        metrics.cpu.load15 = load15_;

        DWORD megahertz = 0;
        if (readRegistryDword(HKEY_LOCAL_MACHINE,
                              L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz",
                              megahertz)) {
            metrics.cpu.frequencyMhz = megahertz;
        }
        metrics.cpu.model = readRegistryString(
            HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            L"ProcessorNameString");
    }

    void readMemory(Metrics& metrics) {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (!::GlobalMemoryStatusEx(&status)) {
            return;
        }
        metrics.memory.virtualTotal = status.ullTotalPhys;
        metrics.memory.virtualFree = status.ullAvailPhys;
        metrics.memory.virtualUsed = status.ullTotalPhys - status.ullAvailPhys;
        metrics.memory.virtualPercent =
            status.ullTotalPhys > 0
                ? static_cast<double>(metrics.memory.virtualUsed) / status.ullTotalPhys * 100.0
                : 0.0;

        // Page file as "swap".
        const std::uint64_t pageFileTotal =
            status.ullTotalPageFile > status.ullTotalPhys ? status.ullTotalPageFile - status.ullTotalPhys
                                                          : 0;
        const std::uint64_t pageFileFree =
            status.ullAvailPageFile > status.ullAvailPhys ? status.ullAvailPageFile - status.ullAvailPhys
                                                          : 0;
        metrics.memory.swapTotal = pageFileTotal;
        metrics.memory.swapFree = pageFileFree;
        metrics.memory.swapUsed = pageFileTotal - pageFileFree;
        metrics.memory.swapPercent =
            pageFileTotal > 0 ? static_cast<double>(metrics.memory.swapUsed) / pageFileTotal * 100.0
                              : 0.0;
    }

    void readDisk(Metrics& metrics) {
        const std::string drive = windowsDrive(settings_.diskMount);
        const std::wstring wide(drive.begin(), drive.end());
        ULARGE_INTEGER freeBytes{}, totalBytes{}, totalFree{};
        if (!::GetDiskFreeSpaceExW(wide.c_str(), &freeBytes, &totalBytes, &totalFree)) {
            return;
        }
        metrics.disk.mountPoint = drive;
        metrics.disk.total = totalBytes.QuadPart;
        metrics.disk.free = totalFree.QuadPart;
        metrics.disk.used = totalBytes.QuadPart - totalFree.QuadPart;
        metrics.disk.usedPercent = totalBytes.QuadPart > 0
                                       ? static_cast<double>(metrics.disk.used) /
                                             static_cast<double>(totalBytes.QuadPart) * 100.0
                                       : 0.0;
    }

    void readNet(Metrics& metrics, double elapsed) {
        // Friendly name ("Ethernet", "Wi-Fi", "Ethernet 2") -> interface index.
        // Exact match first, then prefix match (Windows suffixes duplicates with
        // " 2", " 3"), then the adapter that owns the default gateway.
        std::vector<std::pair<DWORD, std::string>> adapters;
        std::vector<DWORD> withGateway;
        {
            ULONG size = 16 * 1024;
            std::vector<unsigned char> buffer(size);
            auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            if (::GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, addresses,
                                       &size) != NO_ERROR) {
                return;
            }
            for (auto* entry = addresses; entry != nullptr; entry = entry->Next) {
                if (entry->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                    continue;
                }
                const std::string name = narrow(entry->FriendlyName);
                adapters.emplace_back(entry->IfIndex, name);
                if (entry->FirstGatewayAddress != nullptr) {
                    withGateway.push_back(entry->IfIndex);
                }
            }
        }

        auto resolve = [&adapters](const std::string& wanted) -> DWORD {
            if (wanted.empty()) {
                return 0;
            }
            for (const auto& adapter : adapters) {
                if (adapter.second == wanted) {
                    return adapter.first;
                }
            }
            for (const auto& adapter : adapters) {
                if (adapter.second.rfind(wanted, 0) == 0) {
                    return adapter.first;
                }
            }
            return 0;
        };

        DWORD ethIndex = resolve(settings_.ethInterface);
        DWORD wloIndex = resolve(settings_.wloInterface);
        if (ethIndex == 0 && wloIndex == 0) {
            // No configuration: pick the connected adapter for the wired card
            // and a "Wi-Fi"/"Wireless" one for the wireless card.
            for (const auto& adapter : adapters) {
                const std::string& name = adapter.second;
                if (name.find("Wi-Fi") != std::string::npos ||
                    name.find("Wireless") != std::string::npos ||
                    name.find("WLAN") != std::string::npos) {
                    if (wloIndex == 0) wloIndex = adapter.first;
                } else if (ethIndex == 0) {
                    ethIndex = adapter.first;
                }
            }
            for (DWORD index : withGateway) {
                if (index == wloIndex && wloIndex != 0) {
                    ethIndex = 0;  // the default route is wireless
                    break;
                }
                if (ethIndex == 0) {
                    ethIndex = index;
                }
            }
        }
        if (ethIndex != 0 && ethIndex == wloIndex) {
            wloIndex = 0;
        }

        // MIB_IFROW counters are 32-bit; applyNet() handles the wrap-around.
        // The friendly name ("Ethernet 2") is what the user configured, the
        // MIB_IFROW name is an internal \DEVICE\TCPIP_{GUID}: prefer the former.
        auto friendlyName = [&adapters](DWORD index) -> std::string {
            for (const auto& adapter : adapters) {
                if (adapter.first == index) {
                    return adapter.second;
                }
            }
            return {};
        };
        auto readInterface = [this, elapsed, &friendlyName](DWORD index, NetMetrics& out,
                                                            NetSample& previous) {
            if (index == 0) {
                return;
            }
            MIB_IFROW row{};
            row.dwIndex = index;
            if (::GetIfEntry(&row) != NO_ERROR) {
                return;
            }
            std::string name = friendlyName(index);
            if (name.empty()) {
                name = narrow(row.wszName);
            }
            applyNet(out, name, row.dwInOctets, row.dwOutOctets, elapsed, previous);
        };

        readInterface(ethIndex, metrics.eth, previousEth_);
        readInterface(wloIndex, metrics.wlo, previousWlo_);
    }

    void readLhm(Metrics& metrics) {
        std::string json;
        if (!httpGet("127.0.0.1", 8085, "/data.json", json)) {
            return;
        }
        double value = 0;
        if (sensorValue(json, "CPU Package", value) || sensorValue(json, "CPU Total", value) ||
            sensorValue(json, "Core Average", value)) {
            metrics.cpu.temperatureC = value;
            metrics.cpu.hasTemperature = true;
        }
        if (sensorValue(json, "CPU Fan", value)) {
            metrics.cpu.fanPercent = value;
            metrics.cpu.hasFan = true;
        }
    }

    struct NetSample {
        std::uint32_t in = 0;
        std::uint32_t out = 0;
        bool valid = false;
    };

    static std::uint64_t counterDelta(std::uint32_t current, std::uint32_t previous) {
        return current >= previous ? static_cast<std::uint64_t>(current - previous)
                                   : (static_cast<std::uint64_t>(0xFFFFFFFFu - previous) + current +
                                      1);
    }

    void applyNet(NetMetrics& out, const std::string& name, std::uint32_t received,
                  std::uint32_t sent, double elapsed, NetSample& previous) {
        out.interfaceName = name;
        out.downloadedBytes = received;
        out.uploadedBytes = sent;
        if (previous.valid) {
            out.downloadBps = static_cast<double>(counterDelta(received, previous.in)) / elapsed;
            out.uploadBps = static_cast<double>(counterDelta(sent, previous.out)) / elapsed;
        }
        previous.in = received;
        previous.out = sent;
        previous.valid = true;
    }

    SensorSettings settings_;
    std::uint64_t previousTotal_ = 0;
    std::uint64_t previousIdle_ = 0;
    double load1_ = 0.0;
    double load5_ = 0.0;
    double load15_ = 0.0;
    NetSample previousEth_;
    NetSample previousWlo_;
    std::chrono::steady_clock::time_point lastSample_{};
};

}  // namespace

std::unique_ptr<SensorProvider> makeSystemSensors(const SensorSettings& settings) {
    return std::make_unique<WindowsProvider>(settings);
}

}  // namespace lcdctl
