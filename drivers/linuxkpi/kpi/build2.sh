#!/bin/bash
# usage: build2.sh <configured linux-6.12 tree> [outdir]   builds kpi2.ko (needs kpi.ko loaded first at run time)
set -e
SRC=$(cd "$1" && pwd); HERE=$(cd "$(dirname "$0")" && pwd); OUT=${2:-$HERE/out2}
mkdir -p "$OUT"; cp "$HERE"/kpi2_core.c "$HERE"/kpi2_dev.c "$HERE"/kpi2_mm.c "$HERE"/kpi2_pci.c "$HERE"/kpi2_amd.c "$HERE"/kpi2_irq.c "$HERE"/kpi2_drm.c "$HERE"/kpi2_file.c "$HERE"/kpi3_drv.c "$HERE"/kpi4_blk.c "$HERE"/kpi5_msi.c "$HERE"/kpi6_scsi.c "$HERE"/kpi7_net.c "$OUT"/; cp "$HERE"/Kbuild2 "$OUT"/Kbuild
# USB/HID/xHCI support: generic library code and PCI quirks taken straight from the tree
USBX="kfifo genalloc"
for f in $USBX; do cp "$SRC/lib/$f.c" "$OUT/$f.c"; done
cp "$SRC/drivers/usb/host/pci-quirks.c" "$OUT/pci-quirks.c"
# network core helpers taken straight from the tree (address lists, BQL, bit reversal, timecounter)
NETX="net/core/dev_addr_lists.c lib/dynamic_queue_limits.c lib/bitrev.c kernel/time/timecounter.c"
for f in $NETX; do cp "$SRC/$f" "$OUT/$(basename "$f")"; done
# dma-buf and sync_file: buffer and fence sharing through fds (kpi_file.c gives them files and an fd table)
DMABUF="dma-fence dma-fence-chain dma-fence-unwrap dma-fence-array dma-resv dma-buf sync_file"
cp "$SRC/drivers/video/hdmi.c" "$OUT/hdmi.c"
for f in $DMABUF; do cp "$SRC/drivers/dma-buf/$f.c" "$OUT/$f.c"; done
EXTRA="${KPI2_EXTRA_FILES:-}"
for f in $EXTRA; do cp "$HERE/$f" "$OUT/$f"; done
sed -i -E '/^(early_initcall|core_initcall|subsys_initcall|late_initcall|device_initcall|fs_initcall|arch_initcall|postcore_initcall|__exitcall)\(/d; /^early_param\(/d' "$OUT"/*.c
# initcalls are removed above; dma-buf's mounts its pseudo file system, so kpi_file.c calls it
echo 'int kpi_dma_buf_init(void) { return dma_buf_init(); }' >> "$OUT/dma-buf.c"
SRCS="hdmi.c kfifo.c genalloc.c pci-quirks.c $(for f in $NETX; do echo -n "$(basename "$f") "; done)$(for f in $DMABUF; do echo -n "$f.c "; done) $(for f in $EXTRA; do echo -n "$f "; done)"
rm -f "$OUT"/kpi2.ko     # a failed build must not leave the previous module looking like the result
make -C "$SRC" -j"$(nproc)" -k KBUILD_MODPOST_WARN=1 KCFLAGS="-mcmodel=small -fPIC -mpreferred-stack-boundary=4" "KPI2_SRCS=$SRCS" modules M="$OUT" 2>&1 | grep -E "error|Error|warning: .*undefined|kpi2.ko" | grep -v WARNING | head -40
ls -la "$OUT"/kpi2.ko

# optional self test modules: build2.sh <tree> <out2> test  -> $OUT/test/{kpi2test,fputest}.ko
if [ "$3" = "test" ]; then
  T="$OUT/test"; mkdir -p "$T"; cp "$HERE/test/kpi2test.c" "$HERE/test/fputest.c" "$HERE/test/Kbuild" "$T/"
  make -C "$SRC" -j"$(nproc)" -k KBUILD_MODPOST_WARN=1 KCFLAGS="-mcmodel=small -fPIC -mpreferred-stack-boundary=4" modules M="$T" 2>&1 | grep -E " error|test.ko" | head
  ls -la "$T"/kpi2test.ko "$T"/fputest.ko
fi
