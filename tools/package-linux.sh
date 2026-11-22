#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build a release tarball for Linux:
#
#   tools/package-linux.sh                    -> dist/usb-monitor-controller-<ver>-linux-x86_64.tar.gz
#   tools/package-linux.sh --no-build         (reuse build/)
#   tools/package-linux.sh --out /tmp/release
#
# The tarball is the equivalent of the Windows zip: extract it anywhere and run
# bin/lcd-monitor. It contains the four binaries, res/ (themes, fonts, icons),
# the documentation and the licence.

set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"

version="$(sed -n 's/^project(usb-monitor-controller .*VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
version="${version:-0.1.0}"
out="dist"
build=1
build_dir="${BUILD_DIR:-build}"

while [ $# -gt 0 ]; do
    case "$1" in
        --out) out="$2"; shift 2 ;;
        --build-dir) build_dir="$2"; shift 2 ;;
        --no-build) build=0; shift ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

if [ "$build" -eq 1 ]; then
    make -C "$repo" -j"$(nproc 2>/dev/null || echo 4)" >/dev/null
fi

name="usb-monitor-controller-$version-linux-x86_64"
stage="$out/$name"
rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/share/doc"

for binary in lcd-monitor lcd-cli lcd-theme lcd-theme-editor; do
    if [ -x "$build_dir/$binary" ]; then
        install -m 755 "$build_dir/$binary" "$stage/bin/$binary"
    fi
done

mkdir -p "$stage/res"
for directory in themes fonts icons; do
    cp -r "$build_dir/res/$directory" "$stage/res/$directory"
done

install -m 644 README.md AUTHORS.md CHANGELOG.md LICENSE "$stage/share/doc/"
cp -r docs "$stage/share/doc/docs"
install -m 755 tools/install-linux.sh "$stage/install-linux.sh"

tar -C "$out" -czf "$out/$name.tar.gz" "$name"
echo "release: $out/$name.tar.gz ($(du -h "$out/$name.tar.gz" | cut -f1))"
tar -tzf "$out/$name.tar.gz" | sed -n '1,5p' | sed 's/^/  /' 
