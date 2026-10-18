#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Source this file to use the Homebrew-provided dependencies:
#   source tools/dev-env.sh
#   cmake -S . -B build && cmake --build build

BREW="${HOMEBREW_PREFIX:-/home/linuxbrew/.linuxbrew}"

export PKG_CONFIG_PATH="$BREW/lib/pkgconfig"
for formula in libusb libpng jpeg-turbo freetype openssl@3 zlib-ng-compat bzip2; do
    if [ -d "$BREW/opt/$formula/lib/pkgconfig" ]; then
        export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$BREW/opt/$formula/lib/pkgconfig"
    fi
done

export CMAKE_PREFIX_PATH="$BREW"

echo "PKG_CONFIG_PATH=$PKG_CONFIG_PATH"
