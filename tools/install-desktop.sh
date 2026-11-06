#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Install KDE/XDG application-menu entries for the current build.
#
#   tools/install-desktop.sh            # uses ./build
#   tools/install-desktop.sh /path/build
#
# Creates:
#   ~/.local/share/applications/usb-monitor-controller.desktop   (tray daemon)
#   ~/.local/share/applications/usb-lcd-theme-editor.desktop     (theme editor)
#
# Remove them with:  rm ~/.local/share/applications/usb-*.desktop

set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$repo/build}"
app_dir="${XDG_DATA_HOME:-$HOME/.local/share}/applications"

if [[ ! -x "$build/lcd-monitor" ]]; then
    echo "error: $build/lcd-monitor not found — run 'make' first" >&2
    exit 1
fi

mkdir -p "$app_dir"

icon="$build/res/icons/monitor-icon-17865/64.png"
[[ -f "$icon" ]] || icon="video-display"

cat > "$app_dir/usb-monitor-controller.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=USB Monitor Controller
GenericName=USB LCD system monitor
Comment=Display CPU, GPU, memory, disk and network stats on a USB LCD screen
Exec=$build/lcd-monitor --tray
Icon=$icon
Terminal=false
Categories=System;Monitor;
Keywords=lcd;display;monitor;usb;turing;system;
StartupNotify=false
EOF

cat > "$app_dir/usb-lcd-theme-editor.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=USB LCD Theme Editor
Comment=Edit a theme.yaml layout with a live preview
Exec=$build/lcd-theme-editor $build/res/themes $build/res/fonts
Icon=$icon
Terminal=false
Categories=Settings;DesktopSettings;
Keywords=lcd;theme;editor;usb;
StartupNotify=false
EOF

chmod +x "$app_dir"/usb-*.desktop

# Refresh the KDE menu cache when available (harmless elsewhere).
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 >/dev/null 2>&1 || true
elif command -v kbuildsycoca5 >/dev/null 2>&1; then
    kbuildsycoca5 >/dev/null 2>&1 || true
fi

echo "Installed:"
echo "  $app_dir/usb-monitor-controller.desktop"
echo "  $app_dir/usb-lcd-theme-editor.desktop"
echo
echo "They now appear in the KDE application menu (search: \"USB Monitor\")."
