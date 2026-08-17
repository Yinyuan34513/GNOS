#!/bin/sh
# build-fastfetch.sh — build fastfetch (https://github.com/fastfetch-cli/fastfetch)
# for GNOS: static musl, C23 via the locally built clang, minimal modules.
#
# The toolchain wrapper pair lives in build/musl-clang/:
#   cc          clang 24 + musl headers (see the comments there)
#   ld.musl-ld  GNU ld that appends musl's crt1/crti/libc.a/crtn + libgcc
# Every optional fastfetch dependency is switched off: the result detects
# only through /proc, /sys, ioctl and libc -- no X11/wayland/dbus/opengl
# links, because none of those exist on GNOS.
#
# Re-run this script by hand after changing the flags or fetching a new
# fastfetch tree; the Makefile only relinks when the binary is missing.
set -e

BUILD=$(cd "$(dirname "$0")/.." && pwd)/build
FF_SRC=$BUILD/ffsrc
FF_BLD=$BUILD/ffbuild
MUSL=$BUILD/muslsrc/musl
CC=$BUILD/musl-clang/cc
UAPI=$BUILD/fflinux/include

if [ ! -d "$FF_SRC" ]; then
    echo "fastfetch source missing: clone https://github.com/fastfetch-cli/fastfetch.git into $FF_SRC" >&2
    exit 1
fi

cmake -B "$FF_BLD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_C_FLAGS="-O2 -static -fno-pie -fno-pic -isystem $UAPI" \
  -DCMAKE_REQUIRED_INCLUDES="$UAPI" \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DENABLE_VULKAN=OFF -DENABLE_WAYLAND=OFF -DENABLE_XCB_RANDR=OFF \
  -DENABLE_XRANDR=OFF -DENABLE_DRM=OFF -DENABLE_VADRM=OFF -DENABLE_VAX11=OFF \
  -DENABLE_VDPAU=OFF -DENABLE_GIO=OFF -DENABLE_DCONF=OFF -DENABLE_EET=OFF \
  -DENABLE_DBUS=OFF -DENABLE_SQLITE3=OFF -DENABLE_RPM=OFF \
  -DENABLE_IMAGEMAGICK7=OFF -DENABLE_IMAGEMAGICK6=OFF -DENABLE_CHAFA=OFF \
  -DENABLE_EGL=OFF -DENABLE_GLX=OFF -DENABLE_OPENCL=OFF -DENABLE_FREETYPE=OFF \
  -DENABLE_PULSE=OFF -DENABLE_DDCUTIL=OFF -DENABLE_ELF=OFF -DENABLE_ZLIB=OFF \
  -DENABLE_LIBZFS=OFF -DENABLE_LUA=OFF -DENABLE_QUICKJS=OFF \
  -DENABLE_WORDEXP=OFF -DENABLE_LTO=OFF \
  -DBUILD_TESTS=OFF -DIS_MUSL=ON -DENABLE_THREADS=ON \
  -DENABLE_EMBEDDED_PCIIDS=OFF -DENABLE_EMBEDDED_AMDGPUIDS=OFF

ninja -C "$FF_BLD" fastfetch flashfetch
