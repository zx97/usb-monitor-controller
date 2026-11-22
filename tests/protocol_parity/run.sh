#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Protocol parity test: the C++ revision A driver must produce exactly the same
# byte stream as the upstream Python implementation for the same image.
#
# Usage: tests/protocol_parity/run.sh [revision]   (only "A" is implemented)
#
# Set UPSTREAM to point at a turing-smart-screen-python checkout if it is not in
# the default location.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
UPSTREAM="${UPSTREAM:-$HOME/src/turing-smart-screen-python}"
REVISION="${1:-A}"

if [ "$REVISION" != "A" ]; then
    echo "Only revision A is implemented in this parity test." >&2
    exit 0
fi
if [ ! -d "$UPSTREAM/library/lcd" ]; then
    echo "Upstream checkout not found at $UPSTREAM (set UPSTREAM=...). Skipping." >&2
    exit 0
fi

TMP="$(mktemp -d "${TMPDIR:-/dev/shm}/lcd-parity.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# shellcheck disable=SC1091
source "$ROOT/tools/dev-env.sh" >/dev/null

if [ ! -f "$ROOT/build/liblcdctl.a" ]; then
    cmake -S "$ROOT" -B "$ROOT/build" -G Ninja >/dev/null
    cmake --build "$ROOT/build" >/dev/null
fi

LIBS="$(pkg-config --libs libpng libjpeg freetype2 libusb-1.0 openssl)"
g++ -std=c++17 -I"$ROOT/include" "$HERE/rev_a_dump.cpp" \
    "$ROOT/build/liblcdctl.a" $LIBS -o "$TMP/rev_a_dump"

# The manual link has no RPATH: point the loader at the Homebrew libraries.
export LD_LIBRARY_PATH="$(pkg-config --libs-only-L libjpeg libpng freetype2 libusb-1.0 openssl \
    | tr ' ' '\n' | sed 's/^-L//' | paste -sd:)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$TMP/rev_a_dump" "$TMP/cpp.bin"
UPSTREAM="$UPSTREAM" python3 "$HERE/rev_a_dump.py" "$TMP/py.bin"

echo "cpp:    $(sha256sum < "$TMP/cpp.bin" | cut -d' ' -f1)"
echo "python: $(sha256sum < "$TMP/py.bin" | cut -d' ' -f1)"

if cmp -s "$TMP/cpp.bin" "$TMP/py.bin"; then
    echo "PASS: C++ and Python outputs are identical"
else
    echo "FAIL: outputs differ" >&2
    cmp -l "$TMP/cpp.bin" "$TMP/py.bin" | head >&2
    exit 1
fi
