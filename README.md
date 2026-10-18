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

```sh
source tools/dev-env.sh          # puts the Homebrew pkg-config files on the path
cmake -S . -B build -G Ninja
cmake --build build
```

The binary is `build/lcd-cli`. See [`docs/BUILD.md`](docs/BUILD.md) for details
(options, packaging, troubleshooting).

## Usage

```sh
# Enumerate every supported screen (serial + native USB)
./build/lcd-cli list

# Open a screen, auto-detect its revision and print what was found
./build/lcd-cli probe

# Basic operations
./build/lcd-cli clear
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
- [`docs/BUILD.md`](docs/BUILD.md) — build, packaging, permissions,
  troubleshooting.

## Tests

`tests/protocol_parity/run.sh` checks that the C++ revision A driver emits
**exactly** the same byte stream as the upstream Python implementation for the
same image (requires a local `turing-smart-screen-python` checkout):

```sh
tests/protocol_parity/run.sh A
# PASS: C++ and Python outputs are identical
```

## License

GPL-3.0-or-later. This is a derived work: the protocols and logic are ported
from `turing-smart-screen-python`
(Copyright © 2021 Matthieu Houdebine and contributors). See [`LICENSE`](LICENSE).
