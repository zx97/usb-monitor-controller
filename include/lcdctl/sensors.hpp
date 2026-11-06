// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - hardware metrics used by the theme renderer.
//
// Replaces library/sensors/* of turing-smart-screen-python (psutil / GPUtil /
// LibreHardwareMonitor) with direct Linux reads: /proc, /sys/class/hwmon,
// /sys/class/drm, NVML (dlopen) and statvfs. No Python, no daemon dependency.

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lcdctl {

struct CpuMetrics {
    double percent = 0.0;
    double frequencyMhz = 0.0;
    double load1 = 0.0;
    double load5 = 0.0;
    double load15 = 0.0;
    double temperatureC = 0.0;
    double fanPercent = 0.0;
    bool hasTemperature = false;
    bool hasFan = false;
    std::string model;
};

struct GpuMetrics {
    bool available = false;
    double load = 0.0;
    double memoryPercent = 0.0;
    double memoryUsedMb = 0.0;
    double memoryTotalMb = 0.0;
    double temperatureC = 0.0;
    double fanPercent = 0.0;
    double frequencyMhz = 0.0;
    int fps = 0;
    std::string model;
};

struct MemoryMetrics {
    double virtualPercent = 0.0;
    double swapPercent = 0.0;
    std::uint64_t virtualUsed = 0;
    std::uint64_t virtualFree = 0;
    std::uint64_t virtualTotal = 0;
    std::uint64_t swapUsed = 0;
    std::uint64_t swapFree = 0;
    std::uint64_t swapTotal = 0;
};

struct DiskMetrics {
    double usedPercent = 0.0;
    std::uint64_t used = 0;
    std::uint64_t free = 0;
    std::uint64_t total = 0;
    std::string mountPoint = "/";
};

struct NetMetrics {
    double uploadBps = 0.0;
    double downloadBps = 0.0;
    std::uint64_t uploadedBytes = 0;
    std::uint64_t downloadedBytes = 0;
    std::string interfaceName;
};

struct CustomMetrics {
    double numeric = std::nan("");
    std::string text;
};

struct Metrics {
    CpuMetrics cpu;
    GpuMetrics gpu;
    MemoryMetrics memory;
    DiskMetrics disk;
    NetMetrics eth;
    NetMetrics wlo;
    std::string dateDay;
    std::string dateHour;
    std::uint64_t uptimeSeconds = 0;
    std::map<std::string, CustomMetrics> custom;
};

// Settings that influence how metrics are collected (config.yaml "config:").
struct SensorSettings {
    std::string ethInterface;      // "eno1"
    std::string wloInterface;      // "wlp11s0"
    std::string diskMount;   // empty => filesystem holding $HOME
    std::string cpuFan = "AUTO";   // "AUTO" or "controller/fan2"
    std::string dateDayFormat = "medium";   // short|medium|long|full
    std::string dateHourFormat = "medium";
    std::vector<std::pair<std::string, std::string>> customCommands;  // name -> command
};

// A source of metrics. `refresh()` is called from one sampler thread only.
class SensorProvider {
public:
    virtual ~SensorProvider() = default;
    virtual void refresh(Metrics& metrics) = 0;
    virtual std::string name() const = 0;
};

// Fixed values (--stub static): deterministic, used by tests, previews and the
// editor so a theme renders identically on every machine.
std::unique_ptr<SensorProvider> makeStaticSensors(const SensorSettings& settings);

// Pseudo-random values around the static ones (--stub random): moving bars and
// graphs for a quick visual check.
std::unique_ptr<SensorProvider> makeRandomSensors(const SensorSettings& settings);

// Linux implementation: /proc/stat, /proc/meminfo, statvfs, /proc/net/dev,
// hwmon, cpufreq and an optional NVML/AMD GPU backend.
std::unique_ptr<SensorProvider> makeLinuxSensors(const SensorSettings& settings);

// Parse "--stub static[:cpu=42,gpu=12]" style overrides.
std::unique_ptr<SensorProvider> makeStubSensors(const std::string& spec,
                                                const SensorSettings& settings);

// "12.3 G" / "1.2 k/s" - psutil.bytes2human() equivalent (1024-based, no "B").
std::string bytesToHuman(double bytes, bool perSecond = false);

// Format a local date ("DAY") or time ("HOUR") the way the Python project does
// with Babel for FORMAT: short|medium|long|full. Exposed for tests and for the
// daemon preview.
std::string formatDateText(const std::string& formatKey, bool hour,
                           std::time_t when = static_cast<std::time_t>(-1));

// Thread-safe hand-off between the sampler thread and the render loop.
class MetricsStore {
public:
    void publish(const Metrics& metrics);
    Metrics snapshot() const;

private:
    mutable std::mutex mutex_;
    Metrics metrics_;
};

}  // namespace lcdctl
