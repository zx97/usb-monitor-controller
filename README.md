# USB-monitor-controller

A small **C++17** controller for the tiny USB LCD screens supported by
[turing-smart-screen-python](https://github.com/mathoudebine/turing-smart-screen-python)
(Turing Smart Screen / TURZX, XuanFang, Kipye Qiye, WeAct Studio…).

It drives the screens directly over USB — either their **USB CDC serial**
interface (`/dev/ttyACM*`, `COMx`) or their **native USB vendor interface**
(Turing 4.6″/5.2″/8″/8.8″/9.2″/12.3″ models) — without the Python system-monitor
/ theme / sensor stack.

> This project is a C++ port of the LCD subsystem of
> `turing-smart-screen-python`. It contains **no Python** at runtime and keeps
> the wire protocols byte-for-byte identical.

---

## Features

- Device enumeration and best-effort revision auto-detection.
- One driver class per hardware revision, behind a common `LcdComm` interface.
- Drawing helpers on top of a single `displayPILImage()` primitive:
  bitmap, text (FreeType), progress bar, line graph, radial progress bar.
- PNG/JPEG image loading (libpng / libjpeg-turbo).
- `lcd-cli` command line tool: list, probe, clear, on/off, brightness, LED,
  orientation, text, image.
- Native-USB (TUR_USB) support with the vendor DES-encrypted protocol, built
  automatically when libusb + OpenSSL are available.

## Supported screens

| Revision  | Product(s)                                          | Transport         | Status |
|-----------|-----------------------------------------------------|-------------------|--------|
| `A`       | Turing 3.5", UsbPCMonitor 3.5"/5"/7"                | USB CDC serial    | ✅ ported, hardware-validated |
| `B`       | XuanFang 3.5" (incl. flagship)                      | USB CDC serial    | ✅ ported, not hardware-tested |
| `C`       | Turing 2.1"/2.8"/5"/8"/8.8"                         | USB CDC serial    | ✅ ported, not hardware-tested |
| `D`       | Kipye Qiye Smart Display 3.5"                       | USB CDC serial    | ✅ ported, not hardware-tested |
| `WEACT_A` | WeAct Studio Display FS V1 3.5"                     | USB CDC serial    | ✅ ported, not hardware-tested |
| `WEACT_B` | WeAct Studio Display FS V1 0.96"                    | USB CDC serial    | ✅ ported, not hardware-tested |
| `TUR_USB` | Turing 4.6"/5.2"/8"/8.8"/9.2"/12.3" (HW rev. 1.x)   | Native USB (libusb)| ✅ ported, not hardware-tested |

"Hardware-validated" means the code was exercised against a real screen while
this port was developed. The other drivers are faithful ports of the upstream
Python code (unit-testable but not yet confirmed on physical hardware).

## Dependencies

Everything is installable with [Homebrew](https://brew.sh):

```sh
brew install cmake ninja pkg-config libpng jpeg-turbo freetype libusb openssl@3
```

`libusb` + `openssl@3` are optional: without them the project still builds and
only native-USB (`TUR_USB`) support is disabled.

## Build

The easiest way — the Makefile sets up the Homebrew `pkg-config` path for you:

```sh
make
```

That configures CMake (Ninja) and builds `build/lcd-cli`. Other targets:

```sh
make test     # offline simulation round-trip self test (no hardware)
make list     # list connected screens
make probe    # open a screen and print what was detected
make sim      # render a sample frame offline to screencap.png
make static   # build a distributable binary with static dependencies
make help
```

Equivalently, by hand:

```sh
source tools/dev-env.sh          # puts the Homebrew pkg-config files on the path
cmake -S . -B build -G Ninja
cmake --build build
```

See [`docs/BUILD.md`](docs/BUILD.md) for details (options, packaging,
troubleshooting) and [`docs/STATIC_BUILD.md`](docs/STATIC_BUILD.md) for the
distributable static build.

## Usage

```sh
# Enumerate every supported screen (serial + native USB)
./build/lcd-cli list

# Open a screen, auto-detect its revision and print what was found
./build/lcd-cli probe

# Basic operations
./build/lcd-cli clear
./build/lcd-cli clear --color black      # blank in a specific colour
./build/lcd-cli brightness 40
./build/lcd-cli orientation landscape
./build/lcd-cli text "Hello USB" --x 20 --y 200 --size 24 --color lime --background black
./build/lcd-cli image res/backgrounds/example_320x480.png
./build/lcd-cli off
```

Force a specific revision / port / size when auto-detection is not enough:

```sh
./build/lcd-cli probe  --revision C --port /dev/ttyACM0 --width 480 --height 800
./build/lcd-cli clear  --revision TUR_USB
```

## Simulation (no hardware) — render to PNG

Any drawing command can run offline: the driver writes into a recording
transport, the captured byte stream is decoded back into the framebuffer the
screen would have shown, and the result is written to a PNG. This exercises the
real protocol encoders of revisions A/B/C/D/WeAct without a screen.

```sh
./build/lcd-cli text "Hello" --simulate --revision B -o rev_b.png
./build/lcd-cli image logo.png --simulate --revision C --width 480 --height 800 -o rev_c.png
./build/lcd-cli clear --simulate --revision D -o rev_d.png
```

`--simulate` requires an explicit `--revision`; `--output`/`-o` selects the PNG
(default `screencap.png`). The PNG is the **transmitted framebuffer**: the
driver's software rotation for the active orientation is already applied on top
of it (the panel applies its own orientation transform afterwards). To get an
upright preview for a revision that rotates in software, pick an orientation
with no rotation — e.g. landscape for revision C:

```sh
./build/lcd-cli text "Hello" --simulate --revision C --width 480 --height 800 \
    --orientation landscape -o rev_c.png
```

The self test does the same thing programmatically for every revision and
asserts the decoded framebuffer matches the input image:

```sh
make test        # or: ./build/lcd-selftest
```

> Native-USB (`TUR_USB`) cannot be simulated: its protocol carries encrypted
> PNG/JPEG payloads rather than a decodable framebuffer.

## Console / teleprompter

`console` renders a terminal-like scrolling text area: lines appear at the top,
and once the area is full the content shifts up so the top line disappears and
the new line arrives at the bottom. Text is **white on black, anti-aliased**
(configurable with `--color` / `--background`).

```sh
# live console: stream a log or any command output
tail -f app.log | ./build/lcd-cli console

# teleprompter: one new line every 3 seconds
./build/lcd-cli console --file script.txt --interval 3

# smaller area scrolls much faster (recommended on revision A)
./build/lcd-cli console --file script.txt --interval 2 --height 160 --size 13

# preview offline, no hardware
./build/lcd-cli console --file script.txt --simulate --revision A -o console.png
```

`--lines-per-tick N` advances N lines per tick and `--loop` restarts the file.
The area defaults to the whole screen from `--x/--y`; `--width/--height` shrink
it. On revision A a full-screen area costs ~2.4 s per scrolled line, so prefer a
reduced height (the tool prints the estimate when it starts).

## Library use

```cpp
#include "lcdctl/factory.hpp"

auto lcd = lcdctl::createLcd(lcdctl::Revision::A, "/dev/ttyACM0");
lcd->initializeComm();
lcd->setBrightness(50);
lcd->displayText("Hello", 20, 20, {});
lcd->displayBitmap("logo.png");
```

The library is header-only-consumable through `include/lcdctl/` and is built as
a static library `liblcdctl.a`.

## Documentation

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — code structure and the USB
  wire protocols of every revision.
- [`docs/FINDINGS.md`](docs/FINDINGS.md) — measured throughput, hardware quirks
  and protocol gotchas discovered while driving real screens.
- [`docs/BUILD.md`](docs/BUILD.md) — build, packaging, permissions,
  troubleshooting.

## Tests

Two independent checks:

1. **Simulation round-trip** (`make test`, i.e. `build/lcd-selftest`): drives
   each revision (A/B/C/D/WeAct) offline with a known pattern, decodes the
   captured wire bytes and asserts the framebuffer matches the input image.
   No hardware needed.
2. **Protocol parity** (`tests/protocol_parity/run.sh A`): checks that the C++
   revision A driver emits **exactly** the same byte stream as the upstream
   Python implementation for the same image (requires a local
   `turing-smart-screen-python` checkout).

```sh
make test
tests/protocol_parity/run.sh A
```

## License

GPL-3.0-or-later. This is a derived work: the protocols and logic are ported
from `turing-smart-screen-python`
(Copyright © 2021 Matthieu Houdebine and contributors). See [`LICENSE`](LICENSE).
