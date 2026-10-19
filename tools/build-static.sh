#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build-static.sh - produce a self-contained, distributable `lcd-cli`.
#
# The normal `cmake` build links the Homebrew shared libraries, so the
# resulting binary only runs on machines that have those exact .so files.
# This script instead downloads, builds and installs STATIC libraries
# (zlib, libpng, jpeg-turbo, freetype, libusb and optionally OpenSSL) into a
# local prefix, then links the project against that prefix only.
#
# What stays dynamic: glibc (libc/libm/ld-linux) and the system loader.  There
# is no libc.a on this host and Homebrew ships no static libc, so a fully
# static (musl-style) binary is not achievable here.  Linking glibc
# dynamically is portable across Linux distributions of the same architecture.
# libstdc++ and libgcc ARE linked statically.
#
# Output:
#   dist-static/lcd-cli                         the distributable binary
#   dist-static/res/fonts/RobotoMono-Regular.ttf the bundled font
#
# Usage:
#   tools/build-static.sh [--prefix DIR] [--with-openssl] [--clean] [-j N]
#
# Options:
#   --prefix DIR     install the static dependencies here
#                    (default: build-static/prefix)
#   --with-openssl   also build OpenSSL statically; this enables the TUR_USB
#                    revision (slow: OpenSSL takes several minutes to compile)
#   --clean          wipe the local prefix and build trees before building
#                    (downloaded tarballs are kept)
#   -j, --jobs N     parallel build jobs (default: number of CPUs)
#   -h, --help       show this help and exit
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Paths and defaults
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

WORK_DIR="${PROJECT_ROOT}/build-static"
DOWNLOAD_DIR="${WORK_DIR}/downloads"
SRC_DIR="${WORK_DIR}/src"
BUILD_DIR="${WORK_DIR}/build"
TOOLCHAIN_DIR="${WORK_DIR}/toolchain"
DIST_DIR="${PROJECT_ROOT}/dist-static"

# Local install prefix for the freshly built static libraries.
PREFIX="${PREFIX:-${WORK_DIR}/prefix}"

# The project build tree is keyed on the prefix so switching prefixes (e.g. the
# --with-openssl run) can never reuse a CMake cache configured for another one.
PROJECT_BUILD_DIR="${WORK_DIR}/project-$(basename "$PREFIX")"

# Homebrew is the sanctioned package manager for build tools.
BREW="${HOMEBREW_PREFIX:-/home/linuxbrew/.linuxbrew}"

WITH_OPENSSL=0
CLEAN=0
JOBS="$(nproc 2>/dev/null || echo 4)"

# ---------------------------------------------------------------------------
# Pinned dependency versions (release tarballs + SHA-256)
# ---------------------------------------------------------------------------
ZLIB_VERSION="1.3.1"
ZLIB_URL="https://zlib.net/fossils/zlib-1.3.1.tar.gz"
ZLIB_SHA256="9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"

PNG_VERSION="1.6.44"
PNG_URL="https://download.sourceforge.net/libpng/libpng-1.6.44.tar.xz"
PNG_SHA256="60c4da1d5b7f0aa8d158da48e8f8afa9773c1c8baa5d21974df61f1886b8ce8e"

JPEG_VERSION="3.0.4"
JPEG_URL="https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.0.4/libjpeg-turbo-3.0.4.tar.gz"
JPEG_SHA256="99130559e7d62e8d695f2c0eaeef912c5828d5b84a0537dcb24c9678c9d5b76b"

FREETYPE_VERSION="2.13.3"
FREETYPE_URL="https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz"
FREETYPE_SHA256="0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289"

LIBUSB_VERSION="1.0.27"
LIBUSB_URL="https://github.com/libusb/libusb/releases/download/v1.0.27/libusb-1.0.27.tar.bz2"
LIBUSB_SHA256="ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575"

OPENSSL_VERSION="3.0.15"
OPENSSL_URL="https://www.openssl.org/source/openssl-3.0.15.tar.gz"
OPENSSL_SHA256="23c666d0edf20f14249b3d8f0368acaee9ab585b09e1de82107c66e1f3ec9533"

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
    C_BLUE=$'\033[1;34m'; C_YELLOW=$'\033[1;33m'; C_RED=$'\033[1;31m'; C_RESET=$'\033[0m'
else
    C_BLUE=""; C_YELLOW=""; C_RED=""; C_RESET=""
fi

info() { printf '%s[static]%s %s\n' "$C_BLUE" "$C_RESET" "$*"; }
warn() { printf '%s[static]%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
die()  { printf '%s[static] ERROR:%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
build-static.sh - produce a self-contained, distributable `lcd-cli`.

The normal cmake build links the Homebrew shared libraries, so the resulting
binary only runs on machines that have those exact .so files.  This script
downloads and builds STATIC copies of zlib, libpng, jpeg-turbo, freetype and
libusb (and optionally OpenSSL) into a local prefix, then links the project
against that prefix only.  glibc stays dynamic (there is no static libc on
this host); libstdc++ and libgcc are linked statically.

Usage:
  tools/build-static.sh [--prefix DIR] [--with-openssl] [--clean] [-j N]

Options:
  --prefix DIR     install the static dependencies here (default: build-static/prefix)
  --with-openssl   also build OpenSSL statically; enables the TUR_USB revision
                   (slow: OpenSSL takes several minutes to compile)
  --clean          wipe the local prefix and build trees before building
                   (downloaded tarballs are kept in the cache)
  -j, --jobs N     parallel build jobs (default: number of CPUs)
  -h, --help       show this help and exit

Output:
  dist-static/lcd-cli                          the distributable binary
  dist-static/res/fonts/RobotoMono-Regular.ttf the bundled font
EOF
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix)
            [ "$#" -ge 2 ] || die "--prefix requires a directory"
            PREFIX="$2"; shift 2 ;;
        --prefix=*)
            PREFIX="${1#*=}"; shift ;;
        --with-openssl)
            WITH_OPENSSL=1; shift ;;
        --clean)
            CLEAN=1; shift ;;
        -j|--jobs)
            [ "$#" -ge 2 ] || die "$1 requires a number"
            JOBS="$2"; shift 2 ;;
        -j*)
            JOBS="${1#-j}"; shift ;;
        --jobs=*)
            JOBS="${1#*=}"; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            die "unknown argument: $1 (try --help)" ;;
    esac
done

# Make the prefix absolute (relative paths are resolved against the repo root).
case "$PREFIX" in
    /*) ;;
    *) PREFIX="${PROJECT_ROOT}/${PREFIX}" ;;
esac

# ---------------------------------------------------------------------------
# Small utilities
# ---------------------------------------------------------------------------
need_cmd() {
    local cmd
    for cmd in "$@"; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            if command -v brew >/dev/null 2>&1; then
                info "installing build tool via Homebrew: $cmd"
                brew install "$cmd" || die "could not install '$cmd' (needed by the build)"
            else
                die "required command '$cmd' not found and Homebrew is unavailable"
            fi
        fi
    done
}

# Download $2 from $1 into the cache, verifying the SHA-256 $3.
fetch() {
    local url="$1" file="$2" sha="$3"
    local dest="${DOWNLOAD_DIR}/${file}"
    mkdir -p "${DOWNLOAD_DIR}"

    if [ -s "$dest" ]; then
        if printf '%s  %s\n' "$sha" "$dest" | sha256sum -c --status - 2>/dev/null; then
            info "using cached ${file}"
            return 0
        fi
        warn "cached ${file} failed the checksum, downloading again"
        rm -f "$dest"
    fi

    info "downloading ${file}"
    curl -fL --retry 3 --connect-timeout 20 -o "${dest}.part" "$url" \
        || die "download failed: ${url}"
    printf '%s  %s\n' "$sha" "${dest}.part" | sha256sum -c --status - \
        || { rm -f "${dest}.part"; die "checksum mismatch for ${file}"; }
    mv "${dest}.part" "$dest"
}

# Extract $2 (a tarball in the download cache) into src/$1 from scratch.
unpack() {
    local dir="$1" file="$2"
    rm -rf "${SRC_DIR:?}/${dir}"
    mkdir -p "${SRC_DIR}"
    info "extracting ${file}"
    tar -xf "${DOWNLOAD_DIR}/${file}" -C "${SRC_DIR}" \
        || die "failed to extract ${file}"
    [ -d "${SRC_DIR}/${dir}" ] || die "expected directory ${SRC_DIR}/${dir} after extracting ${file}"
}

# ---------------------------------------------------------------------------
# libstdc++ / libgcc static runtimes
#
# The distro compiler can link libgcc statically out of the box, but most
# Fedora images do not ship libstdc++.a.  We therefore stage a static
# libstdc++.a in an isolated directory and point LIBRARY_PATH at it, so that
# `-static-libstdc++` resolves.  Three sources are tried, in order:
#   1. the system compiler (when libstdc++-static is installed),
#   2. a Homebrew GCC installation,
#   3. the distro libstdc++-static RPM (downloaded and extracted, no root).
# ---------------------------------------------------------------------------
RUNTIME_DIR="${TOOLCHAIN_DIR}/lib"
LIBRARY_PATH_EXTRA=""

stage_runtime_archive() {
    # $1: source path of an archive, $2: destination file name
    local src="$1" name="$2"
    [ -f "$src" ] || return 1
    cp -fL "$src" "${RUNTIME_DIR}/${name}"
}

prepare_cxx_runtime() {
    mkdir -p "${RUNTIME_DIR}"
    if [ -f "${RUNTIME_DIR}/libstdc++.a" ]; then
        LIBRARY_PATH_EXTRA="${RUNTIME_DIR}"
        return 0
    fi

    local sys_libstdcxx sys_libsupcxx brew_gpp brew_libstdcxx
    sys_libstdcxx="$(g++ -print-file-name=libstdc++.a)"
    if [ -f "$sys_libstdcxx" ]; then
        info "staging system static libstdc++ (${sys_libstdcxx})"
        stage_runtime_archive "$sys_libstdcxx" libstdc++.a || die "could not stage ${sys_libstdcxx}"
        sys_libsupcxx="$(g++ -print-file-name=libsupc++.a)"
        stage_runtime_archive "$sys_libsupcxx" libsupc++.a || true
        LIBRARY_PATH_EXTRA="${RUNTIME_DIR}"
        return 0
    fi

    brew_gpp="$(ls -1 "${BREW}"/bin/g++-* 2>/dev/null | head -n 1 || true)"
    if [ -n "$brew_gpp" ]; then
        brew_libstdcxx="$("$brew_gpp" -print-file-name=libstdc++.a 2>/dev/null || true)"
        if [ -f "$brew_libstdcxx" ]; then
            info "staging Homebrew static libstdc++ (${brew_libstdcxx})"
            stage_runtime_archive "$brew_libstdcxx" libstdc++.a || die "could not stage ${brew_libstdcxx}"
            stage_runtime_archive "$("$brew_gpp" -print-file-name=libsupc++.a 2>/dev/null || true)" libsupc++.a || true
            LIBRARY_PATH_EXTRA="${RUNTIME_DIR}"
            return 0
        fi
    fi

    if command -v dnf >/dev/null 2>&1 && command -v rpm2cpio >/dev/null 2>&1 && command -v cpio >/dev/null 2>&1; then
        warn "system libstdc++.a missing, extracting it from the libstdc++-static package"
        local rpmdir="${TOOLCHAIN_DIR}/rpm"
        mkdir -p "$rpmdir"
        if dnf download --destdir="$rpmdir" libstdc++-static </dev/null >/dev/null 2>&1; then
            local rpm
            rpm="$(ls -1 "$rpmdir"/libstdc++-static-*.x86_64.rpm 2>/dev/null | head -n 1 || true)"
            if [ -n "$rpm" ]; then
                ( cd "$rpmdir" && rpm2cpio "$rpm" | cpio -idm --quiet ) || true
                local extracted
                extracted="$(find "$rpmdir" -name 'libstdc++.a' -print -quit)"
                if [ -n "$extracted" ]; then
                    stage_runtime_archive "$extracted" libstdc++.a || die "could not stage extracted libstdc++.a"
                    LIBRARY_PATH_EXTRA="${RUNTIME_DIR}"
                    return 0
                fi
            fi
        fi
    fi

    die "no static libstdc++ found. Install 'libstdc++-static' (distro) or a Homebrew GCC, then re-run."
}

# ---------------------------------------------------------------------------
# Dependency builds
# ---------------------------------------------------------------------------
build_zlib() {
    local stamp="${PREFIX}/.stamp-zlib-${ZLIB_VERSION}"
    [ -f "$stamp" ] && { info "zlib ${ZLIB_VERSION} already built"; return 0; }
    fetch "$ZLIB_URL" "zlib-${ZLIB_VERSION}.tar.gz" "$ZLIB_SHA256"
    unpack "zlib-${ZLIB_VERSION}" "zlib-${ZLIB_VERSION}.tar.gz"
    info "building zlib ${ZLIB_VERSION}"
    ( cd "${SRC_DIR}/zlib-${ZLIB_VERSION}" \
        && ./configure --prefix="$PREFIX" --static \
        && make -j"$JOBS" \
        && make install ) || die "zlib build failed"
    touch "$stamp"
}

build_libpng() {
    local stamp="${PREFIX}/.stamp-libpng-${PNG_VERSION}"
    [ -f "$stamp" ] && { info "libpng ${PNG_VERSION} already built"; return 0; }
    fetch "$PNG_URL" "libpng-${PNG_VERSION}.tar.xz" "$PNG_SHA256"
    unpack "libpng-${PNG_VERSION}" "libpng-${PNG_VERSION}.tar.xz"
    info "building libpng ${PNG_VERSION}"
    cmake -S "${SRC_DIR}/libpng-${PNG_VERSION}" -B "${BUILD_DIR}/libpng" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DZLIB_ROOT="$PREFIX" \
        -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_EXECUTABLES=OFF \
        || die "libpng configure failed"
    cmake --build "${BUILD_DIR}/libpng" -j "$JOBS" || die "libpng build failed"
    cmake --install "${BUILD_DIR}/libpng" || die "libpng install failed"

    # Some libpng releases only install libpng16.pc; the project asks for libpng.
    if [ ! -e "${PREFIX}/lib/pkgconfig/libpng.pc" ] && [ -e "${PREFIX}/lib/pkgconfig/libpng16.pc" ]; then
        ln -sf libpng16.pc "${PREFIX}/lib/pkgconfig/libpng.pc"
    fi
    touch "$stamp"
}

build_jpeg() {
    local stamp="${PREFIX}/.stamp-jpeg-turbo-${JPEG_VERSION}"
    [ -f "$stamp" ] && { info "jpeg-turbo ${JPEG_VERSION} already built"; return 0; }
    fetch "$JPEG_URL" "libjpeg-turbo-${JPEG_VERSION}.tar.gz" "$JPEG_SHA256"
    unpack "libjpeg-turbo-${JPEG_VERSION}" "libjpeg-turbo-${JPEG_VERSION}.tar.gz"

    local simd=0
    command -v nasm >/dev/null 2>&1 && simd=1
    info "building jpeg-turbo ${JPEG_VERSION} (SIMD=$simd)"
    cmake -S "${SRC_DIR}/libjpeg-turbo-${JPEG_VERSION}" -B "${BUILD_DIR}/jpeg-turbo" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
        -DWITH_TURBOJPEG=OFF -DWITH_TESTS=OFF -DWITH_SIMD="$simd" \
        || die "jpeg-turbo configure failed"
    cmake --build "${BUILD_DIR}/jpeg-turbo" -j "$JOBS" || die "jpeg-turbo build failed"
    cmake --install "${BUILD_DIR}/jpeg-turbo" || die "jpeg-turbo install failed"
    touch "$stamp"
}

build_freetype() {
    local stamp="${PREFIX}/.stamp-freetype-${FREETYPE_VERSION}"
    [ -f "$stamp" ] && { info "freetype ${FREETYPE_VERSION} already built"; return 0; }
    fetch "$FREETYPE_URL" "freetype-${FREETYPE_VERSION}.tar.xz" "$FREETYPE_SHA256"
    unpack "freetype-${FREETYPE_VERSION}" "freetype-${FREETYPE_VERSION}.tar.xz"
    info "building freetype ${FREETYPE_VERSION}"
    (
        cd "${SRC_DIR}/freetype-${FREETYPE_VERSION}"
        # Keep the dependency tree minimal: no harfbuzz / brotli / bzip2.
        # pkg-config must resolve zlib and libpng from our prefix, statically.
        PKG_CONFIG="$PKG_CONFIG_STATIC" \
        PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig" \
        PKG_CONFIG_LIBDIR="${PREFIX}/lib/pkgconfig" \
        CPPFLAGS="-I${PREFIX}/include" \
        LDFLAGS="-L${PREFIX}/lib" \
        ./configure --prefix="$PREFIX" \
            --disable-shared --enable-static \
            --without-harfbuzz --without-brotli --without-bzip2 \
            --with-zlib=yes --with-png=yes \
        && make -j"$JOBS" \
        && make install
    ) || die "freetype build failed"
    touch "$stamp"
}

build_libusb() {
    local stamp="${PREFIX}/.stamp-libusb-${LIBUSB_VERSION}"
    [ -f "$stamp" ] && { info "libusb ${LIBUSB_VERSION} already built"; return 0; }
    fetch "$LIBUSB_URL" "libusb-${LIBUSB_VERSION}.tar.bz2" "$LIBUSB_SHA256"
    unpack "libusb-${LIBUSB_VERSION}" "libusb-${LIBUSB_VERSION}.tar.bz2"
    info "building libusb ${LIBUSB_VERSION}"
    # udev is disabled: a static libudev does not exist on this host and the
    # sysfs/devfs fallback is enough to enumerate the screens.
    ( cd "${SRC_DIR}/libusb-${LIBUSB_VERSION}" \
        && ./configure --prefix="$PREFIX" --disable-shared --enable-static --disable-udev \
        && make -j"$JOBS" \
        && make install ) || die "libusb build failed"
    touch "$stamp"
}

build_openssl() {
    local stamp="${PREFIX}/.stamp-openssl-${OPENSSL_VERSION}"
    [ -f "$stamp" ] && { info "OpenSSL ${OPENSSL_VERSION} already built"; return 0; }
    fetch "$OPENSSL_URL" "openssl-${OPENSSL_VERSION}.tar.gz" "$OPENSSL_SHA256"
    unpack "openssl-${OPENSSL_VERSION}" "openssl-${OPENSSL_VERSION}.tar.gz"
    info "building OpenSSL ${OPENSSL_VERSION} (this takes a while)"
    ( cd "${SRC_DIR}/openssl-${OPENSSL_VERSION}" \
        && ./Configure linux-x86_64 --prefix="$PREFIX" --openssldir="$PREFIX/ssl" \
               no-shared no-tests -fPIC \
        && make -j"$JOBS" \
        && make install_sw ) || die "OpenSSL build failed"
    touch "$stamp"
}

# ---------------------------------------------------------------------------
# Configure + build the project against the static prefix
# ---------------------------------------------------------------------------
build_project() {
    info "configuring lcd-cli against the static prefix"
    # --fresh: never reuse a CMakeCache that may have been configured for a
    # different prefix (that is how a dynamic OpenSSL dependency sneaked in).
    cmake --fresh -S "$PROJECT_ROOT" -B "$PROJECT_BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++" \
        -DLCDCTL_BUILD_TESTS=OFF \
        -DLCDCTL_BUILD_CLI=ON \
        -DLCDCTL_ENABLE_LIBUSB=ON \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_SKIP_RPATH=ON \
        -DPKG_CONFIG_EXECUTABLE="$PKG_CONFIG_STATIC" \
        || die "CMake configure failed"

    info "building lcd-cli"
    cmake --build "$PROJECT_BUILD_DIR" --target lcd-cli -j "$JOBS" \
        || die "lcd-cli build failed"
}

assemble_dist() {
    info "assembling ${DIST_DIR#"${PROJECT_ROOT}/"}"
    mkdir -p "$DIST_DIR" "$DIST_DIR/res/fonts"
    install -m 0755 "${PROJECT_BUILD_DIR}/lcd-cli" "${DIST_DIR}/lcd-cli"
    install -m 0644 "${PROJECT_ROOT}/res/fonts/RobotoMono-Regular.ttf" \
        "${DIST_DIR}/res/fonts/RobotoMono-Regular.ttf"

    # Drop debug information pulled in by the static archives; this is a
    # distributable, not a debugging artifact.
    if command -v strip >/dev/null 2>&1; then
        strip --strip-unneeded "${DIST_DIR}/lcd-cli" \
            || warn "strip failed, binary left unstripped"
    fi
}

# ---------------------------------------------------------------------------
# Verification
# ---------------------------------------------------------------------------
verify() {
    info "verifying the distributable"

    echo
    echo "=== ldd ${DIST_DIR#"${PROJECT_ROOT}/"}/lcd-cli ==="
    ldd "${DIST_DIR}/lcd-cli" || true

    if ldd "${DIST_DIR}/lcd-cli" 2>&1 | grep -q '/home/linuxbrew'; then
        die "the binary still depends on Homebrew shared libraries"
    fi
    if ldd "${DIST_DIR}/lcd-cli" 2>&1 | grep -qE 'libstdc\+\+\.so|libgcc_s\.so'; then
        die "libstdc++/libgcc were not linked statically"
    fi
    echo "(no Homebrew dependencies, libstdc++/libgcc static)"

    echo
    echo "=== ${DIST_DIR#"${PROJECT_ROOT}/"}/lcd-cli list ==="
    "${DIST_DIR}/lcd-cli" list || die "'lcd-cli list' failed"

    echo
    echo "=== ${DIST_DIR#"${PROJECT_ROOT}/"}/lcd-cli --help ==="
    local help_rc=0
    "${DIST_DIR}/lcd-cli" --help || help_rc=$?
    if [ "$help_rc" -ne 0 ]; then
        # Pre-existing CLI behavior: `--help` with no command prints the usage
        # but returns 1 (main.cpp returns 1 when no positional command is set).
        warn "note: --help printed the usage but exited with status ${help_rc} (pre-existing CLI behavior)"
    fi
    echo "(--help OK)"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    local start_seconds=$SECONDS
    cd "$PROJECT_ROOT"

    info "project:  ${PROJECT_ROOT}"
    info "prefix:   ${PREFIX}"
    info "output:   ${DIST_DIR}"
    info "openssl:  $([ "$WITH_OPENSSL" -eq 1 ] && echo "yes (TUR_USB enabled)" || echo "no (TUR_USB disabled)")"
    info "jobs:     ${JOBS}"

    need_cmd cmake ninja pkg-config curl tar g++ make
    command -v nasm >/dev/null 2>&1 || warn "nasm not found: jpeg-turbo will be built without SIMD acceleration"

    if [ "$CLEAN" -eq 1 ]; then
        info "--clean: removing ${PREFIX} and intermediate build trees"
        rm -rf "$PREFIX" "${BUILD_DIR}" "${SRC_DIR}" "${TOOLCHAIN_DIR}" "${DIST_DIR}"
        # Project build trees are keyed on the prefix name: remove every variant.
        rm -rf "${WORK_DIR}/project" "${WORK_DIR}"/project-*
    fi

    mkdir -p "$PREFIX" "$WORK_DIR"

    # pkg-config wrapper that always queries for static libraries, so that
    # private dependencies (-lz for libpng/freetype, -ludev-free libusb, ...)
    # are pulled in as well.
    PKG_CONFIG_STATIC="${WORK_DIR}/pkg-config-static"
    cat > "$PKG_CONFIG_STATIC" <<EOF
#!/bin/sh
# Generated by tools/build-static.sh - force static pkg-config queries.
exec "$(command -v pkg-config)" --static "\$@"
EOF
    chmod +x "$PKG_CONFIG_STATIC"

    prepare_cxx_runtime
    if [ -n "$LIBRARY_PATH_EXTRA" ]; then
        export LIBRARY_PATH="${LIBRARY_PATH_EXTRA}${LIBRARY_PATH:+:${LIBRARY_PATH}}"
        info "static C++ runtime: ${RUNTIME_DIR}/libstdc++.a"
    fi

    # Build order matters: libpng and freetype need zlib; freetype needs libpng.
    build_zlib
    build_libpng
    build_jpeg
    build_freetype
    build_libusb
    [ "$WITH_OPENSSL" -eq 1 ] && build_openssl

    # Only the local prefix is exported, so pkg-config/CMake cannot silently
    # pick up the Homebrew shared libraries.
    export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig"
    export PKG_CONFIG_LIBDIR="${PREFIX}/lib/pkgconfig"

    build_project
    assemble_dist
    verify

    local elapsed=$(( SECONDS - start_seconds ))
    info "done in ${elapsed}s -> ${DIST_DIR}/lcd-cli"

    # Warn only if the artifact directories would show up in `git status`.
    if command -v git >/dev/null 2>&1 \
        && git -C "$PROJECT_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        local unignored=() d
        for d in build-static dist-static; do
            git -C "$PROJECT_ROOT" check-ignore -q "$d" || unignored+=("${d}/")
        done
        if [ "${#unignored[@]}" -gt 0 ]; then
            warn "note: ${unignored[*]} not covered by .gitignore; add them if you commit from this tree"
        fi
    fi
}

main "$@"
