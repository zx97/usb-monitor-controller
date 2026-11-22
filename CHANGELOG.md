# Changelog

All notable changes are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [0.1.0] — unreleased

First public release: a C++17 port of the LCD subsystem of
[turing-smart-screen-python](https://github.com/mathoudebine/turing-smart-screen-python)
with the system-monitor layer rebuilt in C++.

### Added

- **Drivers**: revisions A (Turing 3.5", UsbPCMonitor), B (XuanFang), C
  (Turing 2.1"/2.8"/5"/8"/8.8"), D (Kipye Qiye), WeAct A/B and the native-USB
  TUR_USB models — byte-compatible with the Python encoders
  (`tests/protocol_parity`).
- **Image/font pipeline**: `Image` (PNG/JPEG, crop, resize, rotate, paste),
  FreeType text with Pillow-compatible anchors, UTF-8.
- **Offline simulation**: `--simulate` re-encodes a frame and decodes it back to
  a PNG, for every revision except TUR_USB.
- **Themes**: reads `res/themes/<name>/theme.yaml` unmodified (32 themes
  bundled), `ThemeRenderer` with change detection and graph history, static
  images/texts, `STATS` for CPU, GPU, memory, disk, network, date, uptime and
  custom commands.
- **Sensors**: Linux (procfs, sysfs, hwmon, cpufreq, NVML, AMD DRM) and Windows
  (Win32 APIs, NVML, optional LibreHardwareMonitor), with static/random stubs
  for tests and previews.
- **Daemon** `lcd-monitor`: deadline-driven scheduler, partial updates, worker
  thread (the UI never waits for the panel), reconnection after a device reset,
  single instance, control window and tray icon (KDE/XDG and Windows).
- **Orientation**: portrait/landscape in both directions, persisted, with a full
  repaint when the panel is rotated or turned back on.
- **Tools**: `lcd-cli` (low-level control, console/teleprompter), `lcd-theme`
  (list/show/validate/preview), `lcd-theme-editor` (Qt6, live preview, save with
  backup).
- **Packaging**: `make install` (per-user Linux), `tools/build-windows.sh`
  (portable Windows folder + zip), `tools/build-windows-installer.sh`
  (Inno Setup, per-user), `tools/build-static.sh` (distributable `lcd-cli`),
  `tools/import-themes.py` (themes/fonts from a checkout), systemd unit, udev
  rules and KDE/XDG desktop entries.
- **Checks**: `make test` (simulation round-trip, 14 protocol checks; theme
  engine, 69 checks), optional `protocol_parity` against the Python project,
  anonymity checker and CI workflows for Linux and Windows.

### Known limitations

- Revisions B/C/D/WeAct/TUR_USB are ported and unit-tested but not
  hardware-validated; only revision A (UsbPCMonitor 3.5") was exercised on a
  real panel.
- `STATS/WEATHER` and `STATS/PING` themes are parsed but not rendered yet.
- Temperatures and fan speeds on Windows require LibreHardwareMonitor; GPU
  metrics require an NVIDIA card (NVML).
- A full-screen redraw on revision A takes ~2.4 s (serial throughput); keep the
  refreshed regions small.
