#!/bin/bash
# usage: build2.sh <configured linux-6.12 tree> [outdir]   builds kpi2.ko (needs kpi.ko loaded first at run time)
set -e
SRC=$(cd "$1" && pwd); HERE=$(cd "$(dirname "$0")" && pwd); OUT=${2:-$HERE/out2}
mkdir -p "$OUT"; cp "$HERE"/kpi2_core.c "$HERE"/kpi2_dev.c "$OUT"/; cp "$HERE"/Kbuild2 "$OUT"/Kbuild
DMABUF="dma-fence dma-fence-chain dma-fence-unwrap dma-fence-array dma-resv"
for f in $DMABUF; do cp "$SRC/drivers/dma-buf/$f.c" "$OUT/$f.c"; done
EXTRA="${KPI2_EXTRA_FILES:-}"
for f in $EXTRA; do cp "$HERE/$f" "$OUT/$f"; done
sed -i -E '/^(early_initcall|core_initcall|subsys_initcall|late_initcall|device_initcall|fs_initcall|arch_initcall|postcore_initcall)\(/d; /^early_param\(/d' "$OUT"/*.c
SRCS="$(for f in $DMABUF; do echo -n "$f.c "; done) $(for f in $EXTRA; do echo -n "$f "; done)"
make -C "$SRC" -j"$(nproc)" -k KBUILD_MODPOST_WARN=1 KCFLAGS="-mcmodel=small -fPIC" "KPI2_SRCS=$SRCS" modules M="$OUT" 2>&1 | grep -E "error|Error|warning: .*undefined|kpi2.ko" | head -40
ls -la "$OUT"/kpi2.ko
