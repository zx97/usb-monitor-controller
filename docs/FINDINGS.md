# Findings, measurements and hardware quirks

Durable notes gathered while porting/driving the screens. Written so a future
session (human or agent) does not have to re-discover them.

Environment used: Fedora-like Linux, Homebrew at `/home/linuxbrew/.linuxbrew`,
GCC 16, CMake/Ninja, and **one physical screen**: a UsbPCMonitor 3.5" (revision
A) on `/dev/ttyACM0`.

- [1. The reference hardware](#1-the-reference-hardware)
- [2. Throughput and animation budget](#2-throughput-and-animation-budget)
- [3. Hardware quirks and bugs found](#3-hardware-quirks-and-bugs-found)
- [4. Protocol / code gotchas](#4-protocol--code-gotchas)
- [5. Build and packaging constraints](#5-build-and-packaging-constraints)
- [6. Capability matrix](#6-capability-matrix)
- [7. How to reproduce the evidence](#7-how-to-reproduce-the-evidence)
- [8. Not implemented](#8-not-implemented)

---

## 1. The reference hardware

```
/dev/ttyACM0   VID:PID=1a86:5722   serial=USB35INCHIPSV2   -> revision A
probe: sub-revision = 1 (UsbPCMonitor 3.5"), 320x480
```

Only **revision A** could be validated on real hardware. Everything else
(B/C/D/WeAct/TUR_USB) is a faithful port validated offline (simulation +
byte-parity), *not* against a device. Treat those as "ported, untested".

The device is owned by group `dialout`; `brltty` and `ModemManager` are the
usual culprits when the port does not appear or disappears (see
`tools/udev/99-lcd.rules`).

---

## 2. Throughput and animation budget

There is **no video engine** on revisions A/B/D/WeAct: an animation is a
succession of `DISPLAY_BITMAP` calls. The link is the bottleneck.

Measured on the reference rev A (RGB565, 2 bytes/pixel, 6-byte command overhead):

| Region | Pixels | ms/frame | fps |
|--------|--------|----------|-----|
| 320×480 (full) | 153 600 | 2368 | **0.4** |
| 320×400 | 128 000 | 2000 | 0.5 |
| 320×240 | 76 800 | 1200 | 0.8 |
| 320×120 | 38 400 | 600 | 1.7 |
| 240×160 | 38 400 | 600 | 1.7 |
| 320×48 | 15 360 | 240 | 4.2 |
| 320×32 | 10 240 | 160 | 6.2 |
| 160×40 | 6 400 | 100 | 9.9 |
| 320×24 | 7 680 | 119 | 8.4 |
| 80×20 | 1 600 | 26 | 38.7 |
| 320×8  | 2 560 | 40 | 25.1 |

**Key facts**

* Effective throughput is a flat **≈ 122 kB/s ≈ 1.0 Mbit/s** at every size.
  The serial port is configured at 115200 baud, but USB CDC ignores that: the
  *firmware* caps the link at ~1 Mbit/s.
* Cost is ~**16 µs per pixel** regardless of shape.
* Design rule: **`width × height × fps ≤ ~62 500`** (pixels per second).

**Consequences**

* Full-screen animation is impossible on rev A (0.4 fps).
* A horizontal **scrolling ticker / marquee** is comfortable: a 320×24 band runs
  at ~8 fps; 320×32 at ~6 fps.
* A **smooth full-screen teleprompter is not feasible** (0.5–0.8 fps), but a
  **paged prompter** (redraw a whole screen every few seconds) works perfectly —
  each page costs ~2.4 s on rev A, so use ≥ 4 s per page.
* Any animation must redraw **only the changed rectangle** (`displayBitmap(img,
  x, y)`), which is what the upstream system monitor already does (it crops text
  to its bounding box).

---

## 3. Hardware quirks and bugs found

### 3.1 The native `CLEAR` command (102) is ignored by this rev A unit

`lcd-cli clear` did nothing. Capture proved our bytes were **byte-identical** to
the upstream Python implementation:

```
SET_ORIENTATION(0x79) + CLEAR(0x66) + SET_ORIENTATION(0x79)   (38 bytes)
```

so the controller simply ignores command 102 — the upstream app has the same
behaviour on this model.

**Fix (implemented):** `LcdCommRevA::clear()` repaints a blank full-screen
bitmap via the generic `LcdComm::fillScreen(color)` helper, like revisions B and
C already do. Confirmed by the user: the screen goes blank. `lcd-cli clear
--color <c>` blanks in any colour.

Screenshots/images/text *do* work — only the native clear is inert.

### 3.2 sysfs enumeration: `PRODUCT` is on the *interface*, serial on the *parent*

`/sys/class/tty/ttyACM0/device` resolves to the **interface** directory
(`.../usb1/1-2/1-2:1.0`):

* `PRODUCT=1a86/5722/100` lives in `<interface>/uevent`;
* `serial`, `product`, `idVendor`, `idProduct` live in the **parent** USB device
  directory (`.../usb1/1-2/`).

A first implementation stripped everything after `:` and read the wrong
directory, yielding `VID:PID=0000:0000`. Correct approach: read `PRODUCT` from
the interface uevent, and `serial`/`product` from the parent directory.

### 3.3 Revision C reset is simplified

The upstream polls `comports()` for up to 15 s on each side of the reset. Our
port does `RESTART` → close → sleep 5 s → `reopen()`. Documented in
`src/drivers/rev_c.cpp`; untested on hardware.

---

## 4. Protocol / code gotchas

### 4.1 Pillow rotation is counter-clockwise, ours is clockwise

The upstream Python uses `Image.rotate(angle, expand=True)` (PIL = **counter**
clockwise). `lcdctl::Image::rotate()` is **clockwise**. Conversion used when
porting:

| Python (PIL) | C++ |
|--------------|-----|
| `rotate(90, expand=True)`  | `rotate(270)` |
| `rotate(270, expand=True)` | `rotate(90)`  |
| `rotate(180)`              | `rotate(180)` |

This bit the rev D driver (landscape) and rev C; both are commented at the
conversion sites.

### 4.2 Revision D chunks are counted in **bytes**, not pixels

`rev_d.cpp` prefixes **every 63 bytes** of image payload with `0x50` (63 bytes
≈ 31.5 pixels). A decoder that counts 63 *pixels* desynchronizes at pixel 31.
The offline decoder therefore de-prefixes into a contiguous buffer first.

### 4.3 RGB565 quantisation in tests

Rev A/B/D/WeAct transmit RGB565, so white comes back as **(248, 252, 248)**, not
(255, 255, 255). Rev C transmits 8-bit BGRA/BGR and keeps exact values. Test
expectations must account for this (the clear regression test does).

### 4.4 Orientation: driver software rotation vs device transform

Some drivers rotate the image in software before sending (rev C rotates 90° for
portrait; rev D rotates for landscape; rev B rotates 180 for reverse
orientations). The panel then applies its *own* orientation transform. Result:
the offline PNG is the **transmitted framebuffer**, not necessarily what the eye
sees. For an upright preview, pick an orientation with no software rotation
(e.g. `--orientation landscape` for rev C). Rev A performs no software rotation
at all, so every orientation is a raw dump.

### 4.5 `TUR_USB`: single DES lives in the OpenSSL legacy provider

On OpenSSL ≥ 3, `EVP_des_cbc()` fails unless the `legacy` provider is loaded
(`OSSL_PROVIDER_load(nullptr, "legacy")`). The port does this on first use, and
the DES-CBC output was verified byte-identical against `pycryptodome`.

### 4.6 Misc

* `--help` must exit **0**; a missing command prints usage and exits 1.
* Running the Python reference imports `library.log`, which creates `log.log`
  **in the current directory** — hence `log.log` in `.gitignore`.
* Manual `g++` links of throwaway programs need
  `LD_LIBRARY_PATH=$(pkg-config --libs-only-L ...)` because they have no RPATH.

---

## 5. Build and packaging constraints

* Homebrew ships **no static libraries** (no `.a`) and the host has **no
  `libc.a`**, so `-static` is impossible. `tools/build-static.sh` therefore
  builds zlib/libpng/jpeg-turbo/freetype/libusb (and OpenSSL with
  `--with-openssl`) from pinned, checksummed sources into a local prefix and
  links them statically, plus `-static-libstdc++ -static-libgcc`. glibc stays
  dynamic. Result: ~2.8 MB, `ldd` shows only `libc`/`libm`/`ld-linux`.
* `libusb` **dev headers** are only available via Homebrew (`brew install
  libusb`); the distro had the runtime `.so` only.
* `openssl` resolves to `openssl@4` here; `PKG_CONFIG_PATH` for brew is built by
  globbing `$(brew --prefix)/opt/*/lib/pkgconfig` (versions vary).
* `nasm` enables jpeg-turbo SIMD (only matters for the TUR_USB JPEG fallback).
* `TUR_USB` is auto-disabled in the static build unless `--with-openssl` (the CMake
  requirement is libusb **and** OpenSSL).

---

## 6. Capability matrix

| Revision | Transport | Video engine | Tested |
|----------|-----------|--------------|--------|
| A | USB CDC serial | no (frame pushing) | ✅ on hardware |
| B | USB CDC serial | no | offline only |
| C | USB CDC serial | `STARTMODE_VIDEO`/`STOP_VIDEO` exist (media playback on some units) | offline only |
| D | USB CDC serial | no | offline only |
| WeAct A/B | USB CDC serial | no | offline only |
| TUR_USB | native USB + DES | **yes, H.264** (commands 111/112/121/122/123, chunk-size negotiation, frame-rate command) | offline only |

The H.264 path exists in the upstream Python driver (`send_video()`,
`upload_file()` to `/tmp/sdcard/mmcblk0p1/…`) but is **not ported** here.

---

## 7. How to reproduce the evidence

```sh
make                                   # build
make test                              # offline round-trip + clear checks (12/12)
tests/protocol_parity/run.sh A         # byte-identical to the Python reference
lcd-cli probe                          # identify the connected screen
lcd-cli list                           # serial + native USB inventory
lcd-cli clear                          # blank the screen (rev A workaround)
lcd-cli image shot.png --simulate --revision C --width 480 --height 800 \
        --orientation landscape -o preview.png   # offline render, no hardware
```

Throughput was measured by timing repeated `displayPILImage()` calls of various
region sizes (a throwaway harness, ~16 µs/pixel).

---

## 8. Not implemented

* TUR_USB H.264 video streaming / file upload to the device SD card.
* Revision C video/media mode.
* libusb hotplug notifications in the static build (`--disable-udev`): re-run
  `lcd-cli list` after plugging a device.
* A ticker/teleprompter helper: feasible for band-sized regions (see §2), not
  implemented.
* Windows support: the serial transport is POSIX-only (`#ifdef` stub elsewhere).
