# SPDX-License-Identifier: GPL-3.0-or-later
#
# USB-monitor-controller - convenience Makefile.
#
#   make            configure (if needed) and build
#   make test       run the offline simulation round-trip self test
#   make list       list connected screens
#   make probe      open a screen and print what was detected
#   make sim        render a sample frame offline to screencap.png
#   make monitor    run the system-monitor daemon (tray icon when available)
#   make themes     list the themes available to the daemon
#   make preview    render the demo theme to theme-preview.png
#   make editor     open the Qt6 theme editor
#   make import-themes  copy upstream themes/fonts (see res/themes/README.md)
#   make install    install for the current user (~/LocalApplications/usb-monitor-controller)
#   make uninstall  remove that installation on the demo theme
#   make static     build a distributable binary with static dependencies
#   make clean      remove build directories
#
# Dependencies come from Homebrew (libpng, jpeg-turbo, freetype, libusb, openssl).
# Everything is configured automatically: just type `make`.

BREW ?= $(shell printf '%s' "$${HOMEBREW_PREFIX:-/home/linuxbrew/.linuxbrew}")

# Collect every Homebrew pkg-config directory (versions vary: openssl@3, openssl@4...).
BREW_PC := $(shell printf '%s' "$(BREW)/lib/pkgconfig"; \
	for d in $(BREW)/opt/*/lib/pkgconfig; do [ -d "$$d" ] && printf ':%s' "$$d"; done)

export PKG_CONFIG_PATH := $(BREW_PC)
export CMAKE_PREFIX_PATH := $(BREW)

BUILD_DIR   ?= build
BUILD_TYPE  ?= Release
CMAKE_FLAGS ?=
JOBS        ?= $(shell nproc 2>/dev/null || echo 4)

.PHONY: all build help test list probe sim static static-clean clean distclean \
        monitor monitor-headless themes preview editor import-themes install uninstall

all: build

help:
	@echo "Targets:"
	@echo "  make            configure and build (default)"
	@echo "  make test       run the simulation self test"
	@echo "  make list       list connected LCD screens"
	@echo "  make probe      open a screen and print its details"
	@echo "  make sim        render a sample frame offline -> screencap.png"
	@echo "  make monitor    run the system-monitor daemon (tray icon if available)"
	@echo "  make monitor-headless  daemon without GUI (systemd/ssh)"
	@echo "  make themes     list the themes available"
	@echo "  make preview    render the demo theme -> theme-preview.png"
	@echo "  make editor     open the Qt6 theme editor"
	@echo "  make import-themes  copy upstream themes + fonts (IMPORT_ARGS=...)"
	@echo "  make install    install for the current user (PREFIX=... to change)"
	@echo "  make uninstall  remove that installation"
	@echo "  make static     build a distributable, statically-linked binary"
	@echo "  make static-clean  rebuild it from scratch (recompiles dependencies)"
	@echo "  make clean      remove build/ and build-static/"
	@echo "  make distclean  also remove dist-static/"

# --- configure + build -------------------------------------------------------

$(BUILD_DIR)/build.ninja:
	cmake -S . -B $(BUILD_DIR) -G Ninja \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(CMAKE_FLAGS)

build: $(BUILD_DIR)/build.ninja
	cmake --build $(BUILD_DIR) --parallel $(JOBS)

# --- convenience targets -----------------------------------------------------

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

list: build
	./$(BUILD_DIR)/lcd-cli list

probe: build
	./$(BUILD_DIR)/lcd-cli probe --verbose

# Offline rendering example: no hardware required.
sim: build
	./$(BUILD_DIR)/lcd-cli text "USB-monitor-controller" --simulate --revision A \
		--x 20 --y 220 --size 20 --color cyan --background black -o screencap.png
	@echo "Rendered frame written to screencap.png"

monitor: build
	./$(BUILD_DIR)/lcd-monitor $(ARGS)

monitor-headless: build
	./$(BUILD_DIR)/lcd-monitor --headless $(ARGS)

themes: build
	./$(BUILD_DIR)/lcd-theme list

preview: build
	./$(BUILD_DIR)/lcd-theme preview demo-320x480 -o theme-preview.png
	@echo "Rendered theme-preview.png"

import-themes:
	python3 tools/import-themes.py $(IMPORT_ARGS)

# Per-user installation: binaries + res/ + docs + KDE menu entries.
install: build
	./tools/install-linux.sh --no-build $(if $(PREFIX),--prefix $(PREFIX),)

uninstall:
	./tools/install-linux.sh --uninstall $(if $(PREFIX),--prefix $(PREFIX),)

editor: build
	./$(BUILD_DIR)/lcd-theme-editor "$(BUILD_DIR)/res/themes" "$(BUILD_DIR)/res/fonts" demo-320x480

static:
	tools/build-static.sh

static-clean:
	tools/build-static.sh --clean

# --- cleanup -----------------------------------------------------------------

clean:
	rm -rf $(BUILD_DIR) build-static build-warn build-nousb

distclean: clean
	rm -rf dist dist-static screencap.png
