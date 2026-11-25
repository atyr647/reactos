#!/bin/bash
# Compare every DRM ioctl number as Linux x86-64 computes it with what libdrm-ros/Mesa compute on Windows x64
# (mingw-w64, this directory's headers). An ioctl number encodes its argument's size, so a difference means the
# structure is laid out differently (Windows' long is 32 bits): such an ioctl must be translated (see drmWaitVBlank in
# src/ros_drm.c) or not used.
#   usage: abicheck.sh <uapi include dir with drm.h and amdgpu_drm.h>   (libdrm's include/drm or Mesa's include/drm-uapi)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
INC=$(cd "$1" && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
grep -hoE "^#define (DRM_IOCTL_[A-Z0-9_]+)\s+DRM_IO[WR]*\(" "$INC/drm.h" "$INC/amdgpu_drm.h" | awk '{print $2}' |
  sort -u > "$T/names"
{ echo '#include "drm.h"'; echo '#include "amdgpu_drm.h"'; echo 'unsigned int vals[] = {'
  sed 's/.*/  &,/' "$T/names"; echo '};'; } > "$T/ioc.c"
gcc -S -o "$T/linux.s" -I"$INC" "$T/ioc.c"
x86_64-w64-mingw32-gcc -S -o "$T/win.s" -I"$HERE/include" -include ros_posix.h -I"$INC" "$T/ioc.c"
n=$(wc -l < "$T/names")
grep -E "^\s+\.long" "$T/linux.s" | awk '{print $2}' | head -"$n" > "$T/l"
grep -E "^\s+\.long" "$T/win.s" | awk '{print $2}' | head -"$n" > "$T/w"
echo "$n ioctls; layout differs for:"
paste "$T/names" "$T/l" "$T/w" | awk '$2 != $3 {print "  " $1}'
