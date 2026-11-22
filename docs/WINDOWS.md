# Windows build

`USB-monitor-controller` builds natively on Windows 10/11 with **MSYS2
(UCRT64)** and **Qt 6**. Everything except the temperature/fan sensors, which
need an external provider, works the same as on Linux.

Validated on Windows 10 22H2 (19045) inside QEMU/KVM: build, `lcd-theme-selftest`
(58 checks), `lcd-selftest` (14 protocol round-trips), theme preview and the Qt
editor. The serial transport was compiled and enumerates through SetupAPI, but
no screen could be plugged into that VM: **driving a real panel from Windows is
not yet confirmed on hardware.**

```mermaid
flowchart LR
    MSYS[MSYS2 UCRT64] --> GCC[mingw-w64 g++ 15]
    MSYS --> QT[Qt 6 Widgets]
    MSYS --> DEPS[libpng · jpeg-turbo · freetype · libusb · openssl · yaml-cpp]
    DEPS --> BUILD[cmake + ninja]
    QT --> BUILD
    BUILD --> EXE[lcd-cli · lcd-theme · lcd-monitor · lcd-theme-editor]
    EXE --> PKG[dist-win/ + zip, DLLs included]
```

## 1. Prerequisites

Install [MSYS2](https://www.msys2.org/), then in the **UCRT64** shell:

```sh
pacman -Syu --noconfirm
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,qt6-base,libpng,libjpeg-turbo,freetype,libusb,openssl,yaml-cpp}
```

MSYS2 is also what the CI/VM path uses: extract
`msys2-base-x86_64-latest.tar` on `C:\` (the `.tar.xz` needs an external `xz`
on Windows, the plain `.tar` works with the built-in `tar.exe`).

## 2. Build and package

```sh
cd /c/src/usb-monitor-controller
tools/build-windows.sh
```

The script configures `build-win/`, builds, runs both self tests, then produces:

- `dist-win/` — portable folder: executables, `res/` (fonts, themes, icons),
  Qt runtime + `platforms/qwindows.dll` (and `qoffscreen.dll`), MinGW DLLs and
  the third-party DLLs actually used;
- `usb-monitor-controller-windows-x64.zip` — the same folder, zipped.

Run it:

```
dist-win\lcd-monitor.exe --tray        # tray icon, drives the screen
dist-win\lcd-theme-editor.exe          # theme editor
dist-win\lcd-theme.exe preview demo-320x480 -o preview.png
```

Manual build, without the script:

```sh
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/ucrt64
cmake --build build-win -j 8
```

## 3. Installer (for another PC)

`dist-win/usb-monitor-controller-<version>-windows-x64-setup.exe` is an Inno
Setup installer built from the same `dist-win/` folder:

```sh
tools/build-windows.sh             # portable folder
tools/build-windows-installer.sh   # setup .exe (needs Inno Setup 6)
```

What it does:

- **per-user installation** (`%LOCALAPPDATA%\Programs\USB Monitor Controller`),
  no administrator rights, no service;
- Start menu entries (monitor + theme editor), optional desktop icon;
- optional **"Start at login"**: a shortcut in the *Startup* folder running
  `lcd-monitor.exe --tray` (you can also toggle it later from the tray menu);
- registers in *Settings → Apps* with an uninstaller;
- uninstall stops a running instance, removes the files, the shortcuts and the
  configuration (`%APPDATA%\usb-monitor-controller`).

Silent deployment (second PC, script, remote install):

```
usb-monitor-controller-0.1.0-windows-x64-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TASKS=autostart
```

Verified in the Windows 10 VM: silent install (40 files), tray binary runs
(`lcd-monitor.exe --once`), the startup shortcut is created, and
`unins000.exe /VERYSILENT` removes the installation directory completely.
The installer is built for Windows 10+ (`MinVersion=10.0`), x64 only.

The portable zip stays the reference artefact: it needs no installation and
contains exactly the same files.

## 4. User interface on Windows

The daemon is a **GUI-subsystem** executable: no console window opens when it is
started from the Start menu, the desktop shortcut or the *Startup* folder. When
it *is* started from a terminal (`--once`, `--simulate`, `--headless`), it
attaches to the parent console so the output stays visible; a redirected
stdout/stderr (`> file`) is left untouched.

Two ways to drive it with the mouse:

- **Tray icon** — same menu as under KDE (Screen on/off, Brightness,
  Orientation, Theme, Control panel, Edit theme, Start at login, Quit).
  Windows keeps new notification icons in the **overflow area**: click the `^`
  chevron next to the clock and drag the icon onto the taskbar to pin it. A
  balloon says so on the first start (once; `app.TRAY_TIP_SHOWN` in the config).
- **Control window** — the same controls in a plain window, reachable by
  double-clicking the tray icon, from the tray menu (*Control panel…*), from the
  Start menu entry **Control panel** (`lcd-monitor --window`), or automatically
  when no tray icon can be created after 30 s.

If the screen is not plugged in, the window still opens and reports
`No screen: …` while retrying the connection every 5 s — plug the panel and it
comes up. A second launch (Start menu, shortcut) does not start a second daemon:
it asks the running instance to show its window (shared-memory guard), then
exits.

## 5. Windows-specific behaviour

| Topic | Linux | Windows |
|---|---|---|
| Serial port | `/dev/ttyACM0` (termios) | `COM3` (Win32 comm API, `\\.\COM3`) |
| Port list | sysfs (`ttyACM*`, `ttyUSB*`) | SetupAPI `GUID_DEVINTERFACE_COMPORT`, VID/PID parsed from the hardware id |
| CPU % | `/proc/stat` | `GetSystemTimes` |
| CPU MHz | `cpufreq` | registry `~MHz` |
| Load 1/5/15 | `/proc/loadavg` | exponentially smoothed CPU % (no loadavg on Windows) |
| RAM | `/proc/meminfo` | `GlobalMemoryStatusEx` |
| Disk | `statvfs($HOME)` | `GetDiskFreeSpaceExW`, drive from `DISK_MOUNT` or the system drive |
| Network | `/proc/net/dev` | `GetAdaptersAddresses` + `GetIfEntry` (32-bit counters, wrap handled) |
| GPU | NVML / AMD sysfs | NVML (`nvml.dll`) |
| Temperature / fan | hwmon | **LibreHardwareMonitor** at `http://127.0.0.1:8085` if running, otherwise displayed as `--` |
| Autostart | `~/.config/autostart/*.desktop` | `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` |
| Config | `~/.config/usb-monitor-controller/config.yaml` | `%APPDATA%\usb-monitor-controller\config.yaml` |

For temperatures, install
[LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor),
enable *Options → Remote Web Server → Run* (port 8085). This is the same
provider the Python project uses on Windows.

## 6. Known limitations

- No hardware validation of the serial link under Windows (the VM had no USB
  screen passthrough). `lcd-cli list` runs and returns nothing without a device.
- The tray icon and the control window are Qt widgets: they were verified by
  rendering offscreen (`--screenshot`, `LCDCTL_WINDOW_SCREENSHOT`) in the VM,
  not by clicking in a live session.
- Temperatures and fan speeds require LibreHardwareMonitor; GPU metrics require
  an NVIDIA card (NVML).
- Native USB (`TUR_USB`) uses libusb: it builds and links, but the WinUSB driver
  must be bound to the device with Zadig on a real machine.

## 7. Cross-checking from Linux

The Windows binary cannot be run here, but the protocol encoders are covered by
`lcd-selftest`, which the Windows build runs too (14 checks, all passing in the
VM). Theme rendering on Windows was compared with the Linux previews through the
editor screenshot (`Cyberpunk`, same pixels).
