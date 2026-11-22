#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build and package USB-monitor-controller for Windows.
#
# Run this inside an MSYS2 UCRT64 shell (Windows), from the repository root or
# anywhere:
#
#   C:\msys64\usr\bin\bash -lc "cd /c/src/usb-monitor-controller && tools/build-windows.sh"
#
# Prerequisites (one command):
#
#   pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,qt6-base,libpng,libjpeg-turbo,freetype,libusb,openssl,yaml-cpp}
#
# Produces dist-win/ (portable folder) and usb-monitor-controller-windows-x64.zip.

set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"

build="build-win"
dist="dist-win"
zipfile="usb-monitor-controller-windows-x64.zip"

echo "== configure =="
cmake -S . -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=/ucrt64

echo "== build =="
cmake --build "$build" -j"$(nproc 2>/dev/null || echo 4)"

echo "== self test (no hardware) =="
"./$build/lcd-theme-selftest.exe"
"./$build/lcd-selftest.exe"

echo "== package =="
rm -rf "$dist" "$zipfile"
mkdir -p "$dist"
cp "$build"/lcd-*.exe "$dist"/
# Bundled resources (fonts, themes, icons) next to the binaries.
cp -r "$build/res" "$dist/res"

# Qt runtime + plugins. --no-opengl-sw keeps the package small; the tray icon and
# the editor only need the widgets platform plugin.
if command -v windeployqt >/dev/null 2>&1; then
    for exe in lcd-monitor.exe lcd-theme-editor.exe; do
        windeployqt --release --no-translations --no-opengl-sw --no-system-d3d-compiler \
            "$dist/$exe" >/dev/null
    done
else
    echo "warning: windeployqt not found, Qt DLLs are missing from the package" >&2
fi

# Offscreen platform plugin: used by --screenshot / CI rendering without a
# visible desktop.
if [ -d "$dist/platforms" ] && [ -f /ucrt64/share/qt6/plugins/platforms/qoffscreen.dll ]; then
    cp -n /ucrt64/share/qt6/plugins/platforms/qoffscreen.dll "$dist/platforms/" 2>/dev/null || true
fi

# MinGW runtime and the third-party DLLs the executables actually use.
for exe in "$dist"/*.exe; do
    ldd "$exe" | awk '/=> \// {print $3}' | sort -u | while read -r dll; do
        case "$dll" in
            /ucrt64/*|/mingw64/*) cp -n "$dll" "$dist/" 2>/dev/null || true ;;
        esac
    done
done

strip "$dist"/*.exe 2>/dev/null || true

echo "== archive =="
if command -v zip >/dev/null 2>&1; then
    (cd "$dist" && zip -qr "../$zipfile" .)
else
    # No zip in the MSYS2 base: use Windows' Compress-Archive.
    powershell -NoProfile -Command "Compress-Archive -Force -Path '$dist\\*' -DestinationPath '$zipfile'" >/dev/null
fi

echo
echo "ready: $repo/$zipfile"
echo "run  : $repo/$dist/lcd-monitor.exe --tray"
