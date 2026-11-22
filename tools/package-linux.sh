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
# the documentation and the licence, plus a README stating the runtime
# dependencies (the binaries are dynamically linked against Qt 6 and the usual
# image libraries, unlike the Windows package which bundles its DLLs).
#
# A source tarball is produced too, so a machine whose distribution does not
# provide the same SONAMEs (Fedora's libjpeg.so.62 vs Ubuntu's libjpeg.so.8) can
# build with `make install`.

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

cat > "$stage/share/doc/README-linux.txt" <<'EOF'
USB-monitor-controller — Linux binary package
=============================================

The four binaries are dynamically linked against the system libraries of the
machine that built this package (Ubuntu 24.04 / Debian 12 family). Install the
runtime dependencies before running them:

  Debian / Ubuntu
    sudo apt install qt6-base-dev libpng16-16 libjpeg-turbo8 libfreetype6 \
                     libusb-1.0-0 libssl3 libyaml-cpp0.8

  Fedora
    sudo dnf install qt6-qtbase libpng jpeg-turbo freetype libusb1 openssl-libs \
                     yaml-cpp

  Arch
    sudo pacman -S qt6-base libpng libjpeg-turbo freetype2 libusb openssl yaml-cpp

`bin/lcd-monitor --once` prints one JSON sample and exits: if it works, the
dependencies are satisfied.

The SONAME of libjpeg differs between distributions (Ubuntu: libjpeg.so.8,
Fedora: libjpeg.so.62): a binary built on Ubuntu will not start on Fedora. Use
the source tarball in that case:

  tar -xzf usb-monitor-controller-<version>-src.tar.gz
  cd usb-monitor-controller-<version>
  make && sudo make install     # or ./tools/install-linux.sh for a per-user copy

Documentation: share/doc/docs/ (THEMES.md, WINDOWS.md, BUILD.md, ...).
EOF

tar -C "$out" -czf "$out/$name.tar.gz" "$name"
echo "binary package: $out/$name.tar.gz ($(du -h "$out/$name.tar.gz" | cut -f1))"

# Source tarball: `make install` on any distribution (same content as the git
# tree, docs and themes included).
srcname="usb-monitor-controller-$version-src"
git -C "$repo" archive --format=tar.gz --prefix="$srcname/" -o "$out/$srcname.tar.gz" HEAD
echo "source package: $out/$srcname.tar.gz ($(du -h "$out/$srcname.tar.gz" | cut -f1))"
tar -tzf "$out/$name.tar.gz" | sed -n '1,5p' | sed 's/^/  /' 
