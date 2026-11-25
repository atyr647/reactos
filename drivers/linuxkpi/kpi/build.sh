#!/bin/bash
# usage: build.sh <configured linux-6.12 tree>   (see ../README.md for the tree changes)
set -e
SRC=$(cd "$1" && pwd); HERE=$(cd "$(dirname "$0")" && pwd); OUT=${2:-$HERE/out}
mkdir -p "$OUT"; cp "$HERE"/kpi_core.c "$HERE"/Kbuild "$OUT"/
KLIB="bitmap ctype find_bit hexdump hweight idr kstrtox list_sort llist plist radix-tree ratelimit rbtree refcount scatterlist seq_buf siphash sort string string_helpers timerqueue vsprintf xarray"
for f in $KLIB; do rm -f "$OUT/$f.c"; cp "$SRC/lib/$f.c" "$OUT/$f.c"; done
# boot-time initcalls and early params would define init_module a second time: drop them
sed -i -E '/^(early_initcall|core_initcall|subsys_initcall|late_initcall|device_initcall|fs_initcall|arch_initcall|postcore_initcall)\(/d; /^early_param\(/d' "$OUT"/*.c
make -C "$SRC" -j"$(nproc)" -k KBUILD_MODPOST_WARN=1 KCFLAGS="-mcmodel=small -fPIC -mpreferred-stack-boundary=4" "KLIB_SRCS=$(for f in $KLIB; do echo -n "$f.c "; done)" modules M="$OUT"
ls -la "$OUT"/kpi.ko
