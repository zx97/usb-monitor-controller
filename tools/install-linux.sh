#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Install USB-monitor-controller for the current user (Linux).
#
#   tools/install-linux.sh                       # ~/LocalApplications/usb-monitor-controller
#   tools/install-linux.sh --prefix ~/apps/umbc  # elsewhere
#   tools/install-linux.sh --no-desktop          # files only
#   tools/install-linux.sh --uninstall
#
# Layout:
#   <prefix>/bin/lcd-monitor            daemon + tray icon + control window
#   <prefix>/bin/lcd-cli                low-level screen tool
#   <prefix>/bin/lcd-theme              themes: list / show / validate / preview
#   <prefix>/bin/lcd-theme-editor       Qt6 theme editor
#   <prefix>/res/{themes,fonts,icons}
#   <prefix>/share/doc/…                README, LICENSE, docs
#
# The binaries are copied from build/ and stay linked against the same
# libraries the build used (Homebrew + the distribution), exactly like when run
# from the source tree.

set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="${HOME}/LocalApplications/usb-monitor-controller"
desktop=1
build=1
uninstall=0
build_dir="${BUILD_DIR:-$repo/build}"

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) prefix="$2"; shift 2 ;;
        --build-dir) build_dir="$2"; shift 2 ;;
        --no-desktop) desktop=0; shift ;;
        --no-build) build=0; shift ;;
        --uninstall) uninstall=1; shift ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

desktop_dir="${XDG_DATA_HOME:-$HOME/.local/share}/applications"

if [ "$uninstall" -eq 1 ]; then
    echo "== removing $prefix =="
    rm -rf "$prefix"
    rm -f "$desktop_dir/usb-monitor-controller.desktop" "$desktop_dir/usb-lcd-theme-editor.desktop"
    if command -v kbuildsycoca6 >/dev/null 2>&1; then kbuildsycoca6 >/dev/null 2>&1 || true; fi
    echo "done (the configuration in ~/.config/usb-monitor-controller is kept)"
    exit 0
fi

if [ "$build" -eq 1 ]; then
    echo "== build =="
    make -C "$repo" -j"$(nproc 2>/dev/null || echo 4)" >/dev/null
fi

if [ ! -x "$build_dir/lcd-monitor" ]; then
    echo "error: $build_dir/lcd-monitor not found — run make first" >&2
    exit 1
fi

echo "== install into $prefix =="
mkdir -p "$prefix/bin" "$prefix/share/doc" "$prefix/share/systemd"
for binary in lcd-monitor lcd-cli lcd-theme lcd-theme-editor; do
    if [ -x "$build_dir/$binary" ]; then
        install -m 755 "$build_dir/$binary" "$prefix/bin/$binary"
    fi
done

rm -rf "$prefix/res"
for directory in themes fonts icons; do
    mkdir -p "$prefix/res"
    cp -r "$build_dir/res/$directory" "$prefix/res/$directory"
done

install -m 644 "$repo/README.md" "$repo/LICENSE" "$prefix/share/doc/"
cp -r "$repo/docs" "$prefix/share/doc/docs"
if [ -f "$repo/tools/systemd/lcd-monitor.service" ]; then
    install -m 644 "$repo/tools/systemd/lcd-monitor.service" "$prefix/share/systemd/"
fi

if [ "$desktop" -eq 1 ]; then
    mkdir -p "$desktop_dir"
    icon="$prefix/res/icons/monitor-icon-17865/64.png"
    [ -f "$icon" ] || icon="video-display"

    cat > "$desktop_dir/usb-monitor-controller.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=USB Monitor Controller
GenericName=USB LCD system monitor
Comment=Display CPU, GPU, memory, disk and network stats on a USB LCD screen
Exec=$prefix/bin/lcd-monitor --tray
Icon=$icon
Terminal=false
Categories=System;Monitor;
Keywords=lcd;display;monitor;usb;turing;system;
StartupNotify=false
EOF

    cat > "$desktop_dir/usb-lcd-theme-editor.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=USB LCD Theme Editor
Comment=Edit a theme.yaml layout with a live preview
Exec=$prefix/bin/lcd-theme-editor $prefix/res/themes $prefix/res/fonts
Icon=$icon
Terminal=false
Categories=Settings;DesktopSettings;
Keywords=lcd;theme;editor;usb;
StartupNotify=false
EOF

    chmod +x "$desktop_dir"/usb-*.desktop
    if command -v kbuildsycoca6 >/dev/null 2>&1; then kbuildsycoca6 >/dev/null 2>&1 || true; fi
    echo "desktop entries: $desktop_dir/usb-*.desktop"
fi

echo
echo "installed: $prefix"
echo "  $prefix/bin/lcd-monitor --tray      # start (or use the KDE menu)"
echo "  $prefix/bin/lcd-monitor --once      # one sample as JSON"
echo "  $prefix/bin/lcd-theme list          # themes"
echo
echo "If an older instance is running, restart it to pick up this copy:"
echo "  pkill -f 'lcd-monitor --tray'; setsid $prefix/bin/lcd-monitor --tray >/dev/null 2>&1 &"
