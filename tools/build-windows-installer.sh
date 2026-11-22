#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the Windows installer from an existing dist-win/ folder.
#
#   tools/build-windows.sh            # first: build + portable package
#   tools/build-windows-installer.sh  # then: setup .exe
#
# Requires Inno Setup 6 ("winget install JRSoftware.InnoSetup" or the classic
# installer); ISCC.exe is looked up in the usual locations, or set ISCC=.

set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"

if [ ! -d dist-win ]; then
    echo "dist-win/ is missing: run tools/build-windows.sh first" >&2
    exit 1
fi

version="$(sed -n 's/^project(usb-monitor-controller .*VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
version="${version:-0.1.0}"

iscc="${ISCC:-}"
if [ -z "$iscc" ]; then
    for candidate in \
        "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
        "/c/Program Files/Inno Setup 6/ISCC.exe" \
        "$(command -v iscc.exe 2>/dev/null || true)"; do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then
            iscc="$candidate"
            break
        fi
    done
fi
if [ -z "$iscc" ]; then
    echo "ISCC.exe (Inno Setup 6) not found; install it or set ISCC=" >&2
    exit 1
fi

echo "== Inno Setup: $iscc (version $version) =="
# MSYS2 rewrites arguments that start with "/" into Windows paths; disable that
# for the ISCC call (-D is not a path).
script="$(cygpath -w "$repo/packaging/windows/usb-monitor-controller.iss" 2>/dev/null || echo "$repo/packaging/windows/usb-monitor-controller.iss")"
MSYS2_ARG_CONV_EXCL='*' "$iscc" "-DAppVersion=$version" "$script"

echo
ls -lh dist-win/*setup.exe
