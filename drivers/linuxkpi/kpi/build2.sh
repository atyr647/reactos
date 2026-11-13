#!/bin/bash
# usage: build2.sh <configured linux-6.12 tree> [outdir]   builds kpi2.ko (needs kpi.ko loaded first at run time)
set -e
SRC=$(cd "$1" && pwd); HERE=$(cd "$(dirname "$0")" && pwd); OUT=${2:-$HERE/out2}
mkdir -p "$OUT"; cp "$HERE"/kpi2_core.c "$HERE"/kpi2_dev.c "$HERE"/kpi2_mm.c "$HERE"/kpi2_pci.c "$HERE"/kpi2_amd.c "$HERE"/kpi2_irq.c "$HERE"/kpi2_drm.c "$HERE"/kpi3_drv.c "$HERE"/kpi4_blk.c "$HERE"/kpi5_msi.c "$HERE"/kpi6_scsi.c "$OUT"/; cp "$HERE"/Kbuild2 "$OUT"/Kbuild
# USB/HID/xHCI support: generic library code and PCI quirks taken straight from the tree
USBX="kfifo genalloc"
for f in $USBX; do cp "$SRC/lib/$f.c" "$OUT/$f.c"; done
cp "$SRC/drivers/usb/host/pci-quirks.c" "$OUT/pci-quirks.c"
DMABUF="dma-fence dma-fence-chain dma-fence-unwrap dma-fence-array dma-resv"
cp "$SRC/drivers/video/hdmi.c" "$OUT/hdmi.c"
for f in $DMABUF; do cp "$SRC/drivers/dma-buf/$f.c" "$OUT/$f.c"; done
EXTRA="${KPI2_EXTRA_FILES:-}"
for f in $EXTRA; do cp "$HERE/$f" "$OUT/$f"; done
sed -i -E '/^(early_initcall|core_initcall|subsys_initcall|late_initcall|device_initcall|fs_initcall|arch_initcall|postcore_initcall)\(/d; /^early_param\(/d' "$OUT"/*.c
SRCS="hdmi.c kfifo.c genalloc.c pci-quirks.c $(for f in $DMABUF; do echo -n "$f.c "; done) $(for f in $EXTRA; do echo -n "$f "; done)"
make -C "$SRC" -j"$(nproc)" -k KBUILD_MODPOST_WARN=1 KCFLAGS="-mcmodel=small -fPIC -mpreferred-stack-boundary=4" "KPI2_SRCS=$SRCS" modules M="$OUT" 2>&1 | grep -E "error|Error|warning: .*undefined|kpi2.ko" | grep -v WARNING | head -40
ls -la "$OUT"/kpi2.ko

# optional self test module: build2.sh <tree> <out2> test  -> $OUT/kpi2test.ko
if [ "$3" = "test" ]; then
  T="$OUT/test"; mkdir -p "$T"; cp "$HERE/test/kpi2test.c" "$HERE/test/Kbuild" "$T/"
  make -C "$SRC" -j"$(nproc)" -k KBUILD_MODPOST_WARN=1 KCFLAGS="-mcmodel=small -fPIC -mpreferred-stack-boundary=4" modules M="$T" 2>&1 | grep -E " error|kpi2test.ko" | head
  ls -la "$T"/kpi2test.ko
fi
