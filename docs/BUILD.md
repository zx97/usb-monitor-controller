# Building `USB-monitor-controller`

## 1. Prerequisites

Install the toolchain and libraries with Homebrew (works on Linux and macOS):

```sh
brew install cmake ninja pkg-config libpng jpeg-turbo freetype libusb openssl@3
```

| Dependency            | Required | Used for |
|-----------------------|----------|----------|
| CMake ≥ 3.16, Ninja   | yes      | build system |
| pkg-config            | yes      | locating the libraries |
| libpng                | yes      | PNG decode/encode |
| jpeg-turbo (libjpeg)  | yes      | JPEG decode/encode |
| freetype              | yes      | text rendering |
| libusb                | optional | native-USB (`TUR_USB`) transport |
| openssl@3             | optional | DES encryption of the `TUR_USB` protocol |

Without libusb/openssl the build still succeeds; only the `TUR_USB` revision is
reported as unavailable.

## 2. Configure and build

The dependencies live in the Homebrew prefix, so put its `pkg-config` files on
the search path first (the helper does that for you):

```sh
source tools/dev-env.sh
cmake -S . -B build -G Ninja
cmake --build build
```

Result: `build/lcd-cli` (and `build/liblcdctl.a`).

### CMake options

| Option | Default | Meaning |
|--------|---------|---------|
| `-DLCDCTL_BUILD_CLI=OFF` | `ON` | build only the library |
| `-DLCDCTL_ENABLE_LIBUSB=OFF` | `ON` | skip native-USB support entirely |
| `-DCMAKE_BUILD_TYPE=Debug` | `Release` | debug build |

During configuration the summary tells you what was enabled:

```
-- lcdctl: libusb found (1.0.30)
-- lcdctl: OpenSSL found (3.5.9)
-- lcdctl: native USB (TUR_USB) support ENABLED
```

### Manual build (no CMake)

```sh
source tools/dev-env.sh
g++ -std=c++17 -O2 -Iinclude \
    src/*.cpp src/drivers/*.cpp \
    $(pkg-config --cflags --libs libpng libjpeg freetype2 libusb-1.0 openssl) \
    -DLCDCTL_WITH_LIBUSB=1 -DLCDCTL_WITH_OPENSSL=1 \
    -o lcd-cli
```

## 3. Run

```sh
./build/lcd-cli list
./build/lcd-cli probe
```

The bundled default font is copied next to the binary at build time; you can
also pass any `.ttf` with `--font`.

## 4. Device permissions

### Linux

The screens appear as USB devices. Two things commonly get in the way:

1. **Serial permissions** (`/dev/ttyACM*`): the user must be in the group that
   owns the device (`dialout` on most distributions, `uucp` on Arch):

   ```sh
   sudo usermod -aG dialout "$USER"   # then log out / back in
   ```

2. **`brltty` claims the CH34x serial adapter** used by many of these screens
   (`idVendor 1a86`). If the port disappears or never appears, disable brltty or
   blacklist the device:

   ```sh
   sudo systemctl mask brltty-udev.service brltty.service   # Debian/Ubuntu
   # or add a udev rule in /etc/udev/rules.d/99-lcd.rules:
   # SUBSYSTEM=="usb", ATTR{idVendor}=="1a86", ATTR{idProduct}=="5722", MODE="0666"
   ```

3. **ModemManager** may probe the CDC-ACM port; if writes fail intermittently,
   add it to `/etc/udev/rules.d/99-lcd-mm.rules`:

   ```
   SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ENV{ID_MM_DEVICE_IGNORE}="1"
   ```

For **native-USB** (`TUR_USB`) screens on Linux, libusb detaches the kernel
driver automatically; no extra rule is required when the device has no kernel
driver bound. Otherwise grant access with a udev rule similar to the above.

### macOS

No driver is needed (CDC-ACM uses the built-in class driver). If a device is
still grabbed by a kernel extension, unplug/replug it. Native USB is accessed
through libusb without kext.

### Windows

- Serial screens need the **CH34x / VCP driver** (usually installed
  automatically) and appear as `COMx`.
- Native-USB screens need a **WinUSB/libusb** driver bound to the interface
  (e.g. with [Zadig](https://zadig.akeo.ie/)), and `libusb-1.0.dll` next to
  `lcd-cli.exe`.

## 5. Producing a distributable binary

`build/lcd-cli` links the Homebrew shared libraries. To check what it needs:

```sh
ldd build/lcd-cli
readelf -d build/lcd-cli | grep -i rpath
```

CMake sets an RPATH to the Homebrew lib directory so the binary runs from the
build tree. For a binary meant to run on other machines, choose one of:

- **Static linking**: build the dependencies statically (Homebrew does not ship
  static libs by default, so this usually means building libpng/jpeg/freetype/
  libusb from source) or link against system libraries with
  `-DCMAKE_EXE_LINKER_FLAGS=-static-libgcc -static-libstdc++`.
- **Bundle the shared libraries**: copy the `.so`/`.dylib` files reported by
  `ldd` next to the executable and set
  `RPATH=$ORIGIN` (`-DCMAKE_INSTALL_RPATH=$ORIGIN`).
- **Use the system toolchain** (`dnf install gcc-c++ cmake libpng-devel
  libjpeg-turbo-devel freetype-devel libusb1-devel openssl-devel`) for a binary
  that links the distro libraries instead of Homebrew's.

The font is only needed by the `text` command; embed or ship
`res/fonts/RobotoMono-Regular.ttf` next to the binary to keep it working.

## 6. Troubleshooting

| Symptom | Likely cause / fix |
|---------|--------------------|
| `no connected screen matching revision X` | Wrong revision, or a port claimed by `brltty`/`ModemManager`; check `lcd-cli list` and `dmesg`. |
| `cannot open serial port …` | Permissions (add user to `dialout`), or the screen reset and re-enumerated: retry. |
| Garbled/corrupted image on rev B | The upstream code sleeps 50 ms between bitmaps; ensure you did not remove it. |
| Rev C never answers HELLO | Screen asleep: send the RESTART/`reset` path once, or power-cycle. |
| `TUR_USB` unavailable | Rebuild with libusb + openssl, and bind a WinUSB/libusb driver (Windows) or add a udev rule (Linux). |
| `FreeType: cannot load font` | Pass `--font /path/to.ttf`; the bundled font was not copied next to the binary. |
