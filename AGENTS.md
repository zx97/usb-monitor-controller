# AGENTS.md — USB-monitor-controller

Guidance for anyone (human or AI agent) working in this repository.

## What this is

A **C++17** controller for the small USB LCD screens supported by
[turing-smart-screen-python](https://github.com/mathoudebine/turing-smart-screen-python)
(Turing, XuanFang, Kipye Qiye, WeAct). It ports the LCD subsystem only — no
sensors, themes or scheduler. Derived work: keep the code **GPL-3.0-or-later**.

## Read these first

- `docs/FINDINGS.md` — measured throughput, hardware quirks, protocol gotchas.
  **Read it before changing a driver or debugging a device.**
- `docs/ARCHITECTURE.md` — layers, per-revision wire protocols, simulation.
- `docs/BUILD.md` / `docs/STATIC_BUILD.md` — building and packaging.

## Build and test

```sh
make            # configure (Ninja) + build  -> build/lcd-cli
make test       # offline simulation checks (no hardware needed)
tests/protocol_parity/run.sh A   # byte-parity vs the Python reference
```

Dependencies come from **Homebrew** (`libpng`, `jpeg-turbo`, `freetype`,
`libusb`, `openssl@3`). Install anything missing with `brew install …` — the
Makefile discovers brew's pkg-config files automatically.

## Ground rules

- C++17, no new third-party dependency without a good reason.
- Every new file starts with the SPDX + GPL header. **Comments and docs in
  English** (chat with the user may be in French).
- Drivers must stay **byte-compatible** with the upstream Python implementation.
  `tests/protocol_parity/run.sh` exists to prove it; extend it when you touch an
  encoder.
- A change is done only when it is *observed*: build, run `make test`, and — for
  anything touching a protocol — validate against real hardware or a decoder.
  `--simulate` renders the transmitted frame to a PNG precisely for that.
- Keep diffs focused; one topic per commit.

## Hardware available

A **UsbPCMonitor 3.5" (revision A)** on `/dev/ttyACM0` — the only screen that
can be tested live. Revisions B/C/D/WeAct/TUR_USB are ported but **not
hardware-validated**; say so explicitly when reporting results.

## Quick map

| Path | Role |
|------|------|
| `include/lcdctl/`, `src/` | library (transports, drivers, image/font, factory) |
| `src/drivers/rev_*.cpp`, `weact.cpp`, `turing_usb.cpp` | one file per revision |
| `src/simulate*.cpp`, `include/lcdctl/simulate.hpp` | offline decode-to-PNG |
| `src/main.cpp` | `lcd-cli` |
| `tests/protocol_parity/`, `tests/simulate_roundtrip.cpp` | regression + parity |
| `tools/build-static.sh` | distributable static build |
| `tools/udev/99-lcd.rules` | Linux permissions / ModemManager / brltty |

## Known traps (short list)

- Native `CLEAR` (102) is ignored by some rev A units → use `fillScreen()`.
- PIL `rotate()` is counter-clockwise; `Image::rotate()` is clockwise.
- Rev D prefixes every **63 bytes** with `0x50` (not 63 pixels).
- RGB565 rounds white to (248, 252, 248).
- The simulation PNG is the *transmitted* framebuffer (the panel applies its own
  orientation transform on top).
- Full-screen animation is impossible on rev A (~0.4 fps); keep regions small
  (`width × height × fps ≤ ~62 500`).
