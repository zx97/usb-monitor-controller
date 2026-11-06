// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcdctl/sensors.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace lcdctl {
namespace {

// Values of library/sensors/sensors_stub_static.py, kept in sync so a C++
// preview and a Python preview of the same theme are comparable.
constexpr double kPercentage = 50.0;
constexpr double kTemperature = 67.3;
constexpr double kCpuFrequencyMhz = 2400.0;
constexpr double kDiskTotalGb = 1000.0;
constexpr double kMemoryTotalGb = 64.0;
constexpr double kGpuMemoryTotalGb = 32.0;
constexpr double kNetworkBytes = 1061000000.0;
constexpr int kGpuFps = 120;
constexpr double kGpuFrequencyMhz = 1500.0;
constexpr std::time_t kStaticDate = 1694014609;
constexpr std::uint64_t kStaticUptime = 4294036;

std::string strftimeText(const char* format, std::time_t when) {
    std::tm local{};
    localtime_r(&when, &local);
    char buffer[128];
    const std::size_t written = std::strftime(buffer, sizeof(buffer), format, &local);
    return std::string(buffer, written);
}

class StaticProvider : public SensorProvider {
public:
    explicit StaticProvider(const SensorSettings& settings) : settings_(settings) {}

    std::string name() const override { return "static"; }

    void refresh(Metrics& m) override {
        m = Metrics{};

        m.cpu.percent = kPercentage;
        m.cpu.frequencyMhz = kCpuFrequencyMhz;
        m.cpu.load1 = m.cpu.load5 = m.cpu.load15 = kPercentage;
        m.cpu.temperatureC = kTemperature;
        m.cpu.hasTemperature = true;
        m.cpu.fanPercent = kPercentage;
        m.cpu.hasFan = true;
        m.cpu.model = "Static CPU";

        m.gpu.available = true;
        m.gpu.load = kPercentage;
        m.gpu.memoryPercent = kPercentage;
        m.gpu.memoryTotalMb = kGpuMemoryTotalGb * 1024.0;
        m.gpu.memoryUsedMb = m.gpu.memoryTotalMb / 100.0 * kPercentage;
        m.gpu.temperatureC = kTemperature;
        m.gpu.fanPercent = kPercentage;
        m.gpu.frequencyMhz = kGpuFrequencyMhz;
        m.gpu.fps = kGpuFps;
        m.gpu.model = "Static GPU";

        const double memoryUsed = std::floor(kMemoryTotalGb / 100.0 * kPercentage) * 1e9;
        const double memoryFree = std::floor(kMemoryTotalGb / 100.0 * (100.0 - kPercentage)) * 1e9;
        m.memory.virtualPercent = kPercentage;
        m.memory.swapPercent = kPercentage;
        m.memory.virtualUsed = static_cast<std::uint64_t>(memoryUsed);
        m.memory.virtualFree = static_cast<std::uint64_t>(memoryFree);
        m.memory.virtualTotal = m.memory.virtualUsed + m.memory.virtualFree;
        m.memory.swapUsed = m.memory.virtualUsed;
        m.memory.swapFree = m.memory.virtualFree;

        m.disk.usedPercent = kPercentage;
        m.disk.used = static_cast<std::uint64_t>(
            std::floor(kDiskTotalGb / 100.0 * kPercentage) * 1e9);
        m.disk.free = static_cast<std::uint64_t>(
            std::floor(kDiskTotalGb / 100.0 * (100.0 - kPercentage)) * 1e9);
        m.disk.total = m.disk.used + m.disk.free;
        m.disk.mountPoint = settings_.diskMount;

        for (NetMetrics* net : {&m.eth, &m.wlo}) {
            net->uploadBps = kNetworkBytes;
            net->downloadBps = kNetworkBytes;
            net->uploadedBytes = static_cast<std::uint64_t>(kNetworkBytes) * 1000;
            net->downloadedBytes = static_cast<std::uint64_t>(kNetworkBytes) * 1000;
        }
        m.eth.interfaceName = settings_.ethInterface.empty() ? "eth0" : settings_.ethInterface;
        m.wlo.interfaceName = settings_.wloInterface.empty() ? "wlan0" : settings_.wloInterface;

        m.dateDay = formatDateText(settings_.dateDayFormat, false, kStaticDate);
        m.dateHour = formatDateText(settings_.dateHourFormat, true, kStaticDate);
        m.uptimeSeconds = kStaticUptime;
    }

private:
    SensorSettings settings_;
};

class RandomProvider : public SensorProvider {
public:
    explicit RandomProvider(const SensorSettings& settings, unsigned seed = 10)
        : settings_(settings), rng_(seed) {}

    std::string name() const override { return "random"; }

    void refresh(Metrics& m) override {
        m = Metrics{};
        std::uniform_real_distribution<double> percent(0.0, 100.0);
        std::uniform_real_distribution<double> cpuFreq(800.0, 3400.0);
        std::uniform_real_distribution<double> temperature(30.0, 90.0);
        std::uniform_real_distribution<double> gpuMemory(300.0, 16000.0);
        std::uniform_real_distribution<double> rate(0.0, 5.0e7);

        m.cpu.percent = percent(rng_);
        m.cpu.frequencyMhz = cpuFreq(rng_);
        m.cpu.load1 = percent(rng_);
        m.cpu.load5 = percent(rng_);
        m.cpu.load15 = percent(rng_);
        m.cpu.temperatureC = temperature(rng_);
        m.cpu.hasTemperature = true;
        m.cpu.fanPercent = percent(rng_);
        m.cpu.hasFan = true;

        m.gpu.available = true;
        m.gpu.load = percent(rng_);
        m.gpu.memoryPercent = percent(rng_);
        m.gpu.memoryUsedMb = gpuMemory(rng_);
        m.gpu.memoryTotalMb = 16000.0;
        m.gpu.temperatureC = temperature(rng_);
        m.gpu.fanPercent = percent(rng_);
        m.gpu.frequencyMhz = cpuFreq(rng_);
        m.gpu.fps = static_cast<int>(percent(rng_) * 2);

        m.memory.virtualPercent = percent(rng_);
        m.memory.swapPercent = percent(rng_);
        m.memory.virtualTotal = 64ull * 1000 * 1000 * 1000;
        m.memory.virtualUsed =
            static_cast<std::uint64_t>(m.memory.virtualTotal * m.memory.virtualPercent / 100.0);
        m.memory.virtualFree = m.memory.virtualTotal - m.memory.virtualUsed;
        m.memory.swapTotal = 8ull * 1000 * 1000 * 1000;
        m.disk.usedPercent = percent(rng_);
        m.disk.total = 1000ull * 1000 * 1000 * 1000;
        m.disk.used = static_cast<std::uint64_t>(m.disk.total * m.disk.usedPercent / 100.0);
        m.disk.free = m.disk.total - m.disk.used;
        m.disk.mountPoint = settings_.diskMount;

        for (NetMetrics* net : {&m.eth, &m.wlo}) {
            net->uploadBps = rate(rng_);
            net->downloadBps = rate(rng_);
            net->uploadedBytes = static_cast<std::uint64_t>(rate(rng_) * 1000);
            net->downloadedBytes = static_cast<std::uint64_t>(rate(rng_) * 1000);
        }
        m.eth.interfaceName = settings_.ethInterface;
        m.wlo.interfaceName = settings_.wloInterface;

        m.dateDay = formatDateText(settings_.dateDayFormat, false);
        m.dateHour = formatDateText(settings_.dateHourFormat, true);
        m.uptimeSeconds = static_cast<std::uint64_t>(std::time(nullptr)) % 1000000;
    }

private:
    SensorSettings settings_;
    std::mt19937 rng_;
};

// Wraps a provider and overrides a few metrics ("--stub static:cpu=42").
class OverrideProvider : public SensorProvider {
public:
    OverrideProvider(std::unique_ptr<SensorProvider> base, std::map<std::string, double> values)
        : base_(std::move(base)), values_(std::move(values)) {}

    std::string name() const override { return base_->name() + "+overrides"; }

    void refresh(Metrics& m) override {
        base_->refresh(m);
        for (const auto& entry : values_) {
            set(m, entry.first, entry.second);
        }
    }

private:
    static void set(Metrics& m, const std::string& key, double value) {
        if (key == "cpu" || key == "cpu.percentage") {
            m.cpu.percent = value;
        } else if (key == "cpu.frequency") {
            m.cpu.frequencyMhz = value;
        } else if (key == "cpu.temperature") {
            m.cpu.temperatureC = value;
        } else if (key == "cpu.fan") {
            m.cpu.fanPercent = value;
        } else if (key == "gpu" || key == "gpu.percentage") {
            m.gpu.load = value;
            m.gpu.available = true;
        } else if (key == "gpu.temperature") {
            m.gpu.temperatureC = value;
        } else if (key == "gpu.memory") {
            m.gpu.memoryPercent = value;
        } else if (key == "mem" || key == "memory") {
            m.memory.virtualPercent = value;
        } else if (key == "disk") {
            m.disk.usedPercent = value;
        } else if (key == "net") {
            m.eth.uploadBps = m.eth.downloadBps = value;
            m.wlo.uploadBps = m.wlo.downloadBps = value;
        } else if (!key.empty()) {
            m.custom[key] = CustomMetrics{value, std::to_string(static_cast<long long>(value))};
        }
    }

    std::unique_ptr<SensorProvider> base_;
    std::map<std::string, double> values_;
};

}  // namespace

std::string bytesToHuman(double bytes, bool perSecond) {
    static const char* symbols[] = {"", "k", "M", "G", "T", "P", "E", "Z", "Y"};
    const char* symbol = symbols[0];
    double value = bytes;
    for (int i = 8; i >= 1; --i) {
        const double factor = std::ldexp(1.0, i * 10);
        if (bytes >= factor) {
            value = bytes / factor;
            symbol = symbols[i];
            break;
        }
    }
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream << std::setprecision(1) << value << ' ' << symbol;
    if (perSecond) {
        stream << "/s";
    }
    return stream.str();
}

std::string formatDateText(const std::string& formatKey, bool hour, std::time_t when) {
    if (when == static_cast<std::time_t>(-1)) {
        when = std::time(nullptr);
    }
    const std::string key = formatKey.empty() ? "medium" : formatKey;
    const char* format = nullptr;
    if (hour) {
        if (key == "short") {
            format = "%H:%M";
        } else if (key == "long") {
            format = "%H:%M:%S %Z";
        } else if (key == "full") {
            format = "%H:%M:%S %Z";
        } else {
            format = "%H:%M:%S";
        }
    } else {
        if (key == "short") {
            format = "%x";
        } else if (key == "long") {
            format = "%B %d, %Y";
        } else if (key == "full") {
            format = "%A, %B %d, %Y";
        } else {
            format = "%b %d, %Y";
        }
    }
    return strftimeText(format, when);
}

void MetricsStore::publish(const Metrics& metrics) {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_ = metrics;
}

Metrics MetricsStore::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

std::unique_ptr<SensorProvider> makeStaticSensors(const SensorSettings& settings) {
    return std::make_unique<StaticProvider>(settings);
}

std::unique_ptr<SensorProvider> makeRandomSensors(const SensorSettings& settings) {
    return std::make_unique<RandomProvider>(settings);
}

std::unique_ptr<SensorProvider> makeStubSensors(const std::string& spec,
                                                const SensorSettings& settings) {
    // "static" | "random" | "static:cpu=42,gpu=12"
    std::string kind = spec;
    std::string overrides;
    const std::size_t colon = spec.find(':');
    if (colon != std::string::npos) {
        kind = spec.substr(0, colon);
        overrides = spec.substr(colon + 1);
    }

    std::unique_ptr<SensorProvider> base;
    if (kind == "random" || kind == "stub") {
        base = makeRandomSensors(settings);
    } else {
        base = makeStaticSensors(settings);
    }

    std::map<std::string, double> values;
    std::size_t start = 0;
    while (start < overrides.size()) {
        const std::size_t comma = overrides.find(',', start);
        const std::string pair =
            overrides.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const std::size_t equal = pair.find('=');
        if (equal != std::string::npos) {
            const std::string key = pair.substr(0, equal);
            try {
                values[key] = std::stod(pair.substr(equal + 1));
            } catch (const std::exception&) {
                throw std::invalid_argument("invalid stub override: " + pair);
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }

    if (values.empty()) {
        return base;
    }
    return std::make_unique<OverrideProvider>(std::move(base), std::move(values));
}

}  // namespace lcdctl
