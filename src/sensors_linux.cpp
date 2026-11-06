// SPDX-License-Identifier: GPL-3.0-or-later
//
// Linux sensor provider: everything is read from procfs/sysfs, plus NVML
// (loaded at runtime) and the DRM sysfs attributes for GPUs.

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lcdctl/logging.hpp"
#include "lcdctl/sensors.hpp"

namespace lcdctl {
namespace {

std::string readFile(const std::string& path, std::size_t maxBytes = 65536) {
    std::ifstream stream(path);
    if (!stream) {
        return "";
    }
    std::string text;
    text.resize(maxBytes);
    stream.read(&text[0], static_cast<std::streamsize>(maxBytes));
    text.resize(static_cast<std::size_t>(stream.gcount()));
    return text;
}

bool readDouble(const std::string& path, double& out) {
    const std::string text = readFile(path, 256);
    if (text.empty()) {
        return false;
    }
    try {
        out = std::stod(text);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool readLong(const std::string& path, long long& out) {
    const std::string text = readFile(path, 256);
    if (text.empty()) {
        return false;
    }
    try {
        out = std::stoll(text);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<std::string> listDirectories(const std::string& path) {
    std::vector<std::string> entries;
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        return entries;
    }
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        entries.push_back(path + "/" + name);
    }
    closedir(dir);
    std::sort(entries.begin(), entries.end());
    return entries;
}

// Run a shell command and return its first output line. Bounded in time so a
// misbehaving custom sensor cannot stall the sampler.
std::string runCommand(const std::string& command, int timeoutMs = 500) {
    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        return "";
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        return "";
    }
    if (pid == 0) {
        dup2(pipeFds[1], STDOUT_FILENO);
        close(pipeFds[0]);
        close(pipeFds[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipeFds[1]);

    std::string output;
    char buffer[512];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0) {
            kill(pid, SIGKILL);
            break;
        }
        pollfd descriptor{pipeFds[0], POLLIN, 0};
        const int ready = poll(&descriptor, 1, static_cast<int>(remaining));
        if (ready <= 0) {
            continue;
        }
        const ssize_t count = read(pipeFds[0], buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        output.append(buffer, static_cast<std::size_t>(count));
        if (output.find('\n') != std::string::npos) {
            break;
        }
    }
    close(pipeFds[0]);
    int status = 0;
    waitpid(pid, &status, 0);

    const std::size_t newline = output.find('\n');
    if (newline != std::string::npos) {
        output.resize(newline);
    }
    return output;
}

struct HwmonSensor {
    std::string chip;
    std::string attribute;  // "temp1", "fan2"
    double value = 0.0;
    bool hasMax = false;
    double maxValue = 0.0;
};

std::vector<HwmonSensor> scanHwmon() {
    std::vector<HwmonSensor> sensors;
    for (const std::string& dir : listDirectories("/sys/class/hwmon")) {
        const std::string chip = readFile(dir + "/name");
        std::string chipName = chip;
        while (!chipName.empty() && (chipName.back() == '\n' || chipName.back() == ' ')) {
            chipName.pop_back();
        }
        for (const std::string& file : listDirectories(dir)) {
            const std::string base = file.substr(file.find_last_of('/') + 1);
            const std::size_t underscore = base.find('_');
            if (underscore == std::string::npos) {
                continue;
            }
            const std::string attribute = base.substr(0, underscore);
            const std::string suffix = base.substr(underscore);
            if (suffix != "_input") {
                continue;
            }
            if (attribute.rfind("temp", 0) != 0 && attribute.rfind("fan", 0) != 0) {
                continue;
            }
            HwmonSensor sensor;
            sensor.chip = chipName;
            sensor.attribute = attribute;
            if (!readDouble(dir + "/" + base, sensor.value)) {
                continue;
            }
            if (attribute.rfind("temp", 0) == 0) {
                sensor.value /= 1000.0;  // millidegrees
            }
            const std::string maxPath = dir + "/" + attribute + "_max";
            if (readDouble(maxPath, sensor.maxValue)) {
                sensor.hasMax = true;
                if (attribute.rfind("temp", 0) == 0) {
                    sensor.maxValue /= 1000.0;
                }
            }
            sensors.push_back(std::move(sensor));
        }
    }
    return sensors;
}

bool chipIsCpu(const std::string& chip) {
    static const char* names[] = {"coretemp", "k10temp", "zenpower", "cpu_thermal",
                                  "acpitz",    "it87",    "nct67"};
    for (const char* name : names) {
        if (chip.find(name) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// --- GPU backends ---------------------------------------------------------

using NvmlDevice = void*;

struct NvmlApi {
    bool loaded = false;
    void* handle = nullptr;
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
        result.handle = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (!result.handle) {
            result.handle = dlopen("libnvidia-ml.so", RTLD_LAZY | RTLD_LOCAL);
        }
        if (!result.handle) {
            return result;
        }
        auto symbol = [&](const char* name) { return dlsym(result.handle, name); };
        result.init = reinterpret_cast<int (*)()>(symbol("nvmlInit_v2"));
        result.shutdown = reinterpret_cast<int (*)()>(symbol("nvmlShutdown"));
        result.handleByIndex =
            reinterpret_cast<int (*)(unsigned, NvmlDevice*)>(symbol("nvmlDeviceGetHandleByIndex_v2"));
        result.memoryInfo = reinterpret_cast<int (*)(NvmlDevice, void*)>(symbol("nvmlDeviceGetMemoryInfo"));
        result.utilizationRates =
            reinterpret_cast<int (*)(NvmlDevice, void*)>(symbol("nvmlDeviceGetUtilizationRates"));
        result.temperature =
            reinterpret_cast<int (*)(NvmlDevice, int, unsigned*)>(symbol("nvmlDeviceGetTemperature"));
        result.fanSpeed = reinterpret_cast<int (*)(NvmlDevice, unsigned*)>(symbol("nvmlDeviceGetFanSpeed"));
        result.clockInfo =
            reinterpret_cast<int (*)(NvmlDevice, int, unsigned*)>(symbol("nvmlDeviceGetClockInfo"));
        result.deviceName =
            reinterpret_cast<int (*)(NvmlDevice, char*, unsigned)>(symbol("nvmlDeviceGetName"));
        result.loaded = result.init && result.handleByIndex && result.memoryInfo &&
                        result.utilizationRates;
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
    if (api.handleByIndex(0, &device) != 0 || !device) {
        return false;
    }
    NvmlMemory memory{};
    if (api.memoryInfo(device, &memory) == 0 && memory.total > 0) {
        gpu.memoryTotalMb = static_cast<double>(memory.total) / (1024.0 * 1024.0);
        gpu.memoryUsedMb = static_cast<double>(memory.used) / (1024.0 * 1024.0);
        gpu.memoryPercent = gpu.memoryTotalMb > 0 ? gpu.memoryUsedMb / gpu.memoryTotalMb * 100.0 : 0.0;
    }
    NvmlUtilization utilization{};
    if (api.utilizationRates(device, &utilization) == 0) {
        gpu.load = utilization.gpu;
    }
    unsigned temperature = 0;
    if (api.temperature && api.temperature(device, 0 /* NVML_TEMPERATURE_GPU */, &temperature) == 0) {
        gpu.temperatureC = temperature;
    }
    unsigned fan = 0;
    if (api.fanSpeed && api.fanSpeed(device, &fan) == 0) {
        gpu.fanPercent = fan;
    }
    unsigned clockMhz = 0;
    if (api.clockInfo && api.clockInfo(device, 0 /* NVML_CLOCK_GRAPHICS */, &clockMhz) == 0) {
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

bool sampleAmdGpu(GpuMetrics& gpu) {
    for (const std::string& dir : listDirectories("/sys/class/drm")) {
        const std::string busy = dir + "/device/gpu_busy_percent";
        double percent = 0;
        if (!readDouble(busy, percent)) {
            continue;
        }
        gpu.available = true;
        gpu.load = percent;
        long long vramTotal = 0;
        long long vramUsed = 0;
        if (readLong(dir + "/device/mem_info_vram_total", vramTotal) && vramTotal > 0) {
            gpu.memoryTotalMb = static_cast<double>(vramTotal) / (1024.0 * 1024.0);
            if (readLong(dir + "/device/mem_info_vram_used", vramUsed)) {
                gpu.memoryUsedMb = static_cast<double>(vramUsed) / (1024.0 * 1024.0);
                gpu.memoryPercent = gpu.memoryUsedMb / gpu.memoryTotalMb * 100.0;
            }
        }
        // Temperature and fan live in the hwmon node of the card.
        for (const std::string& hwmon : listDirectories(dir + "/device/hwmon")) {
            double value = 0;
            if (readDouble(hwmon + "/temp1_input", value)) {
                gpu.temperatureC = value / 1000.0;
            }
            if (readDouble(hwmon + "/fan1_input", value)) {
                gpu.fanPercent = value;
            }
            double maxValue = 0;
            if (readDouble(hwmon + "/fan1_max", maxValue) && maxValue > 0) {
                gpu.fanPercent = gpu.fanPercent / maxValue * 100.0;
            }
        }
        double clock = 0;
        if (readDouble(dir + "/device/pp_dpm_sclk", clock)) {
            // Not a plain number: ignore, the clock stays unknown.
        }
        return true;
    }
    return false;
}

// --- Provider -------------------------------------------------------------

class LinuxProvider : public SensorProvider {
public:
    explicit LinuxProvider(const SensorSettings& settings) : settings_(settings) {}

    std::string name() const override { return "linux"; }

    void refresh(Metrics& metrics) override {
        metrics = Metrics{};
        const auto now = std::chrono::steady_clock::now();
        const double elapsed =
            lastSample_.time_since_epoch().count() == 0
                ? 1.0
                : std::chrono::duration<double>(now - lastSample_).count();
        lastSample_ = now;

        readCpu(metrics);
        readMemory(metrics);
        readDisk(metrics);
        readNet(metrics, std::max(0.001, elapsed));
        readHwmon(metrics);
        readUptime(metrics);

        metrics.dateDay = formatDateText(settings_.dateDayFormat, false);
        metrics.dateHour = formatDateText(settings_.dateHourFormat, true);

        metrics.gpu.available = false;
        if (!sampleNvml(metrics.gpu)) {
            sampleAmdGpu(metrics.gpu);
        }

        for (const auto& custom : settings_.customCommands) {
            const std::string output = runCommand(custom.second);
            CustomMetrics value;
            value.text = output;
            char* end = nullptr;
            const double numeric = std::strtod(output.c_str(), &end);
            if (end && end != output.c_str()) {
                value.numeric = numeric;
            }
            metrics.custom[custom.first] = value;
        }
    }

private:
    void readCpu(Metrics& metrics) {
        // /proc/stat: cpu user nice system idle iowait irq softirq steal ...
        const std::string stat = readFile("/proc/stat", 4096);
        const std::size_t newline = stat.find('\n');
        long long values[10] = {0};
        int count = 0;
        if (newline != std::string::npos) {
            std::istringstream stream(stat.substr(0, newline));
            std::string label;
            stream >> label;
            long long value = 0;
            while (count < 10 && (stream >> value)) {
                values[count++] = value;
            }
        }
        if (count >= 4) {
            const long long idle = values[3] + (count > 4 ? values[4] : 0);
            const long long total = [&] {
                long long sum = 0;
                for (int i = 0; i < count; ++i) sum += values[i];
                return sum;
            }();
            if (previousCpuTotal_ > 0 && total > previousCpuTotal_) {
                const long long deltaTotal = total - previousCpuTotal_;
                const long long deltaIdle = idle - previousCpuIdle_;
                metrics.cpu.percent =
                    deltaTotal > 0 ? (1.0 - static_cast<double>(deltaIdle) / deltaTotal) * 100.0 : 0.0;
                metrics.cpu.percent = std::clamp(metrics.cpu.percent, 0.0, 100.0);
            }
            previousCpuTotal_ = total;
            previousCpuIdle_ = idle;
        }

        // CPU frequency: average of the per-core scaling_cur_freq (kHz).
        double frequency = 0;
        int cores = 0;
        for (const std::string& cpu : listDirectories("/sys/devices/system/cpu")) {
            if (cpu.find("/cpu") == std::string::npos || cpu.size() < 5 ||
                !std::isdigit(static_cast<unsigned char>(cpu.back()))) {
                continue;
            }
            double value = 0;
            if (readDouble(cpu + "/cpufreq/scaling_cur_freq", value)) {
                frequency += value / 1000.0;  // kHz -> MHz
                ++cores;
            }
        }
        if (cores > 0) {
            metrics.cpu.frequencyMhz = frequency / cores;
        } else {
            double total = 0;
            int found = 0;
            const std::string cpuinfo = readFile("/proc/cpuinfo");
            std::istringstream stream(cpuinfo);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.rfind("cpu MHz", 0) != 0) continue;
                const std::size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                total += std::stod(line.substr(colon + 1));
                ++found;
            }
            if (found > 0) {
                metrics.cpu.frequencyMhz = total / found;
            }
        }

        const std::string loadavg = readFile("/proc/loadavg", 256);
        std::istringstream loadStream(loadavg);
        double load1 = 0, load5 = 0, load15 = 0;
        if (loadStream >> load1 >> load5 >> load15) {
            const long coresCount = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
            metrics.cpu.load1 = load1 / coresCount * 100.0;
            metrics.cpu.load5 = load5 / coresCount * 100.0;
            metrics.cpu.load15 = load15 / coresCount * 100.0;
        }

        const std::string model = readFile("/sys/devices/virtual/dmi/id/product_name", 256);
        std::string cpuModel;
        {
            std::istringstream stream(readFile("/proc/cpuinfo"));
            std::string line;
            while (std::getline(stream, line)) {
                if (line.rfind("model name", 0) == 0) {
                    const std::size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        cpuModel = line.substr(colon + 2);
                    }
                    break;
                }
            }
        }
        metrics.cpu.model = cpuModel;
        (void)model;
    }

    void readMemory(Metrics& metrics) {
        const std::string meminfo = readFile("/proc/meminfo", 8192);
        std::istringstream stream(meminfo);
        std::string key;
        std::uint64_t value = 0;
        std::string unit;
        std::uint64_t total = 0, available = 0, swapTotal = 0, swapFree = 0;
        while (stream >> key >> value >> unit) {
            const std::uint64_t bytes = value * 1024;  // /proc/meminfo is in kB
            if (key == "MemTotal:") total = bytes;
            else if (key == "MemAvailable:") available = bytes;
            else if (key == "SwapTotal:") swapTotal = bytes;
            else if (key == "SwapFree:") swapFree = bytes;
        }
        metrics.memory.virtualTotal = total;
        metrics.memory.virtualFree = available;
        metrics.memory.virtualUsed = total > available ? total - available : 0;
        metrics.memory.virtualPercent =
            total > 0 ? static_cast<double>(total - available) / total * 100.0 : 0.0;
        metrics.memory.swapTotal = swapTotal;
        metrics.memory.swapFree = swapFree;
        metrics.memory.swapUsed = swapTotal > swapFree ? swapTotal - swapFree : 0;
        metrics.memory.swapPercent =
            swapTotal > 0 ? static_cast<double>(swapTotal - swapFree) / swapTotal * 100.0 : 0.0;
    }

    void readDisk(Metrics& metrics) {
        // An empty DISK_MOUNT means "the filesystem holding the user's data".
        // On immutable distributions "/" is a small read-only composefs, so
        // $HOME is the meaningful answer.
        std::string mount = settings_.diskMount;
        if (mount.empty()) {
            const char* home = std::getenv("HOME");
            mount = (home && *home) ? home : "/";
        }
        struct statvfs info{};
        if (statvfs(mount.c_str(), &info) != 0) {
            return;
        }
        const std::uint64_t blockSize = info.f_frsize ? info.f_frsize : info.f_bsize;
        const std::uint64_t total = static_cast<std::uint64_t>(info.f_blocks) * blockSize;
        const std::uint64_t free = static_cast<std::uint64_t>(info.f_bavail) * blockSize;
        const std::uint64_t used = total > free ? total - free : 0;
        metrics.disk.mountPoint = mount;
        metrics.disk.total = total;
        metrics.disk.free = free;
        metrics.disk.used = used;
        metrics.disk.usedPercent = total > 0 ? static_cast<double>(used) / total * 100.0 : 0.0;
    }

    // Default-route interface from /proc/net/route ("Iface Destination Gateway ...").
    std::string defaultRouteInterface() const {
        const std::string routes = readFile("/proc/net/route", 16384);
        std::istringstream stream(routes);
        std::string line;
        std::getline(stream, line);  // header
        while (std::getline(stream, line)) {
            std::istringstream fields(line);
            std::string interface;
            std::string destination;
            std::string gateway;
            if (!(fields >> interface >> destination >> gateway)) {
                continue;
            }
            if (destination == "00000000") {
                return interface;
            }
        }
        return "";
    }

    // First wireless-looking interface ("wl*", "wlan*", "wlp*").
    std::string wirelessInterface() const {
        for (const std::string& path : listDirectories("/sys/class/net")) {
            const std::string name = path.substr(path.find_last_of('/') + 1);
            if (name.rfind("wl", 0) == 0) {
                return name;
            }
        }
        return "";
    }

    void readNet(Metrics& metrics, double elapsed) {
        // /proc/net/dev: "iface: rx_bytes packets errs drop fifo frame compressed multicast tx_bytes ..."
        std::string eth = settings_.ethInterface;
        std::string wlo = settings_.wloInterface;
        if (eth.empty()) {
            eth = defaultRouteInterface();
            if (eth.rfind("wl", 0) == 0) {
                // The default route is wireless: leave the "eth" card empty so
                // both theme cards do not display the same counters.
                if (wlo.empty()) wlo = eth;
                eth.clear();
            }
        }
        if (wlo.empty()) {
            wlo = wirelessInterface();
            if (wlo == eth) wlo.clear();
        }

        const std::string dev = readFile("/proc/net/dev", 65536);
        std::istringstream stream(dev);
        std::string line;
        while (std::getline(stream, line)) {
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string name = line.substr(0, colon);
            while (!name.empty() && name.front() == ' ') name.erase(name.begin());
            while (!name.empty() && name.back() == ' ') name.pop_back();

            std::istringstream values(line.substr(colon + 1));
            std::uint64_t rx = 0, tx = 0;
            std::uint64_t ignore = 0;
            if (!(values >> rx)) continue;
            for (int i = 0; i < 7; ++i) values >> ignore;
            if (!(values >> tx)) continue;

            if (!eth.empty() && name == eth) {
                applyNet(metrics.eth, name, rx, tx, elapsed, previousEth_);
            } else if (!wlo.empty() && name == wlo) {
                applyNet(metrics.wlo, name, rx, tx, elapsed, previousWlo_);
            }
        }
        if (metrics.eth.interfaceName.empty()) {
            metrics.eth.interfaceName = eth;
        }
        if (metrics.wlo.interfaceName.empty()) {
            metrics.wlo.interfaceName = wlo;
        }
    }

    struct NetSample {
        std::uint64_t rx = 0;
        std::uint64_t tx = 0;
        bool valid = false;
    };

    void applyNet(NetMetrics& out, const std::string& name, std::uint64_t rx, std::uint64_t tx,
                  double elapsed, NetSample& previous) {
        out.interfaceName = name;
        out.downloadedBytes = rx;
        out.uploadedBytes = tx;
        if (previous.valid) {
            const std::uint64_t rxDelta = rx >= previous.rx ? rx - previous.rx : 0;
            const std::uint64_t txDelta = tx >= previous.tx ? tx - previous.tx : 0;
            out.downloadBps = static_cast<double>(rxDelta) / elapsed;
            out.uploadBps = static_cast<double>(txDelta) / elapsed;
        }
        previous.rx = rx;
        previous.tx = tx;
        previous.valid = true;
    }

    void readHwmon(Metrics& metrics) {
        const std::vector<HwmonSensor> sensors = scanHwmon();
        double bestTemperature = 0;
        bool hasTemperature = false;
        double fanPercent = -1;
        bool hasFan = false;

        for (const HwmonSensor& sensor : sensors) {
            if (sensor.attribute.rfind("temp", 0) == 0 && chipIsCpu(sensor.chip)) {
                if (!hasTemperature || sensor.value > bestTemperature) {
                    bestTemperature = sensor.value;
                    hasTemperature = true;
                }
            }
            if (sensor.attribute.rfind("fan", 0) == 0) {
                const bool wanted =
                    settings_.cpuFan == "AUTO"
                        ? chipIsCpu(sensor.chip)
                        : (settings_.cpuFan ==
                           sensor.chip + "/" + sensor.attribute);
                if (!wanted) {
                    continue;
                }
                double percent = sensor.value;
                if (sensor.hasMax && sensor.maxValue > 0) {
                    percent = sensor.value / sensor.maxValue * 100.0;
                } else if (percent > 100.0) {
                    // No fan_max: assume a 3000 rpm ceiling, like most desktop fans.
                    percent = percent / 3000.0 * 100.0;
                }
                if (!hasFan || percent > fanPercent) {
                    fanPercent = percent;
                    hasFan = true;
                }
            }
        }

        metrics.cpu.hasTemperature = hasTemperature;
        metrics.cpu.temperatureC = hasTemperature ? bestTemperature : 0.0;
        metrics.cpu.hasFan = hasFan;
        metrics.cpu.fanPercent = hasFan ? std::clamp(fanPercent, 0.0, 100.0) : 0.0;
    }

    void readUptime(Metrics& metrics) {
        const std::string text = readFile("/proc/uptime", 256);
        if (text.empty()) {
            return;
        }
        try {
            metrics.uptimeSeconds = static_cast<std::uint64_t>(std::stod(text));
        } catch (const std::exception&) {
        }
    }

    SensorSettings settings_;
    long long previousCpuTotal_ = 0;
    long long previousCpuIdle_ = 0;
    NetSample previousEth_;
    NetSample previousWlo_;
    std::chrono::steady_clock::time_point lastSample_{};
};

}  // namespace

std::unique_ptr<SensorProvider> makeLinuxSensors(const SensorSettings& settings) {
    return std::make_unique<LinuxProvider>(settings);
}

}  // namespace lcdctl
