#!/bin/bash
# Build libdrm for ReactOS x64 (mingw-w64): upstream libdrm's KMS API (xf86drmMode.c) and libdrm_amdgpu compiled
# unchanged, with src/ros_drm.c in place of the Linux-only xf86drm.c and src/ros_posix.c for mmap/fcntl.
#   usage: build.sh <prefix> [libdrm source dir]     (downloads libdrm $VER if no source dir is given)
# Result: <prefix>/lib/libdrm.a, libdrm_amdgpu.a, <prefix>/include, <prefix>/lib/pkgconfig/libdrm{,_amdgpu}.pc
# (static libraries: they end up inside the program or Vulkan driver that uses them; link that with -static so
# libdrm_amdgpu's winpthread is static too: ReactOS has no libwinpthread-1.dll).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
VER=2.4.134
SHA=ac5e74d157830eb8bee44c6a6bf3ad49774ef0dd2a72bdad74a8f20308b52a95
PREFIX=$(mkdir -p "$1" && cd "$1" && pwd)
SRC=${2:-}
CC=${CC:-x86_64-w64-mingw32-gcc}
AR=${AR:-x86_64-w64-mingw32-ar}

if [ -z "$SRC" ]; then
  SRC=$PREFIX/src/libdrm-$VER
  if [ ! -d "$SRC" ]; then
    mkdir -p "$PREFIX/src"
    curl -fsSL --retry 3 -o "$PREFIX/src/libdrm.tar.xz" "https://dri.freedesktop.org/libdrm/libdrm-$VER.tar.xz"
    echo "$SHA  $PREFIX/src/libdrm.tar.xz" | sha256sum -c --quiet -
    tar -C "$PREFIX/src" -xJf "$PREFIX/src/libdrm.tar.xz"
  fi
fi
SRC=$(cd "$SRC" && pwd)

OBJ=$PREFIX/obj; rm -rf "$OBJ"; mkdir -p "$OBJ/core" "$OBJ/amdgpu" "$PREFIX/lib/pkgconfig" "$PREFIX/include/libdrm"
CFLAGS="-O2 -g -std=gnu11 -D_FILE_OFFSET_BITS=64 -D_WIN32_WINNT=0x0A00 -DHAVE_VISIBILITY=0 -DHAVE_LIBDRM_ATOMIC_PRIMITIVES=1 \
  -I$HERE/include -include ros_posix.h -I$SRC -I$SRC/include/drm -I$SRC/amdgpu -I$REPO/drivers/linuxkpi \
  -Wall -Wno-unused-function -Wno-pointer-sign"

compile() { # <objdir> <sources...>
  local dir=$1; shift
  for f in "$@"; do
    $CC $CFLAGS -c "$f" -o "$dir/$(basename "${f%.c}").o"
  done
}

# xf86drmMode.c casts pointers through unsigned long, which is 32 bits on Windows: build a copy with uintptr_t
mkdir -p "$OBJ/src"
sed -e 's/#define U642VOID(x) ((void \*)(unsigned long)(x))/#define U642VOID(x) ((void *)(uintptr_t)(x))/' \
    -e 's/#define VOID2U64(x) ((uint64_t)(unsigned long)(x))/#define VOID2U64(x) ((uint64_t)(uintptr_t)(x))/' \
    "$SRC/xf86drmMode.c" > "$OBJ/src/xf86drmMode.c"
[ "$(grep -c '(uintptr_t)(x)' "$OBJ/src/xf86drmMode.c")" = 2 ] || { echo "xf86drmMode.c: pointer casts not found"; exit 1; }
compile "$OBJ/core" "$HERE/src/ros_drm.c" "$HERE/src/ros_posix.c" "$OBJ/src/xf86drmMode.c" "$SRC/xf86drmHash.c" \
  "$SRC/xf86drmRandom.c" "$SRC/xf86drmSL.c"
CFLAGS="$CFLAGS -DAMDGPU_ASIC_ID_TABLE=ros_amdgpu_ids_path()"     # GPU names: amdgpu.ids next to the DLL
compile "$OBJ/amdgpu" "$SRC"/amdgpu/*.c
rm -f "$PREFIX/lib/libdrm.a" "$PREFIX/lib/libdrm_amdgpu.a"
$AR rcs "$PREFIX/lib/libdrm.a" "$OBJ"/core/*.o
$AR rcs "$PREFIX/lib/libdrm_amdgpu.a" "$OBJ"/amdgpu/*.o

# headers where Mesa and libdrm users look for them: <xf86drm.h>, <libdrm/drm.h>, <drm.h> (libdrm's include dir),
# <amdgpu.h>, plus the POSIX compat headers they need on Windows
# (install -C: unchanged headers keep their time stamps, so dependent builds such as Mesa do not rebuild everything)
inst() { install -C -m 644 "$@"; }
inst "$SRC"/xf86drm.h "$SRC"/xf86drmMode.h "$SRC"/libsync.h "$SRC"/amdgpu/amdgpu.h "$PREFIX/include/libdrm/"
inst "$SRC"/include/drm/*.h "$PREFIX/include/libdrm/"
inst "$HERE"/include/xf86drm_ros.h "$HERE"/include/ros_posix.h "$HERE"/include/poll.h "$PREFIX/include/libdrm/"
mkdir -p "$PREFIX/include/libdrm/sys"
inst "$HERE"/include/sys/*.h "$PREFIX/include/libdrm/sys/"
inst "$REPO"/drivers/linuxkpi/lkpi_drm.h "$PREFIX/include/libdrm/"
cp "$SRC"/data/amdgpu.ids "$PREFIX/lib/" 2>/dev/null || true

cat > "$PREFIX/lib/pkgconfig/libdrm.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libdrm
Description: Userspace interface to kernel DRM services (ReactOS LinuxKPI port)
Version: $VER
Libs: -L\${libdir} -ldrm
Cflags: -I\${includedir} -I\${includedir}/libdrm
EOF
cat > "$PREFIX/lib/pkgconfig/libdrm_amdgpu.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libdrm_amdgpu
Description: Userspace interface to kernel DRM services for amdgpu (ReactOS LinuxKPI port)
Version: $VER
Requires.private: libdrm
Libs: -L\${libdir} -ldrm_amdgpu -ldrm -lpthread
Cflags: -I\${includedir} -I\${includedir}/libdrm
EOF
echo "libdrm-ros $VER in $PREFIX"
