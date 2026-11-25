#!/bin/bash
# Build every Linux module the LinuxKPI host needs, from a pristine Linux 6.12 tarball.
#   usage: build-all.sh <workdir> [tarball]
# Output: <workdir>/out/*.ko and <workdir>/out/modules.list (semicolon list, load order) for
#   cmake -DLKPI_MODULES="$(cat <workdir>/out/modules.list)" ...
# Needs: gcc make flex bison bc libelf-dev libssl-dev perl patch xz curl
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mkdir -p "$1" && cd "$1" && pwd)
VER=6.12
TARBALL=${2:-$WORK/linux-$VER.tar.xz}
SRC=$WORK/linux-$VER
OUT=$WORK/out
KC="-mcmodel=small -fPIC -mpreferred-stack-boundary=4"

[ -f "$TARBALL" ] || curl -fL --retry 3 -o "$TARBALL" "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$VER.tar.xz"
if [ ! -d "$SRC" ]; then
  tar -C "$WORK" -xJf "$TARBALL"
  patch -d "$SRC" -p1 < "$HERE/linux-6.12-kpi.patch"
fi
cp "$HERE/linux-6.12.config" "$SRC/.config"
make -C "$SRC" olddefconfig >/dev/null
make -C "$SRC" -j"$(nproc)" modules_prepare >/dev/null

mkdir -p "$OUT"
"$HERE/build.sh"  "$SRC" "$OUT/kpi_out"
"$HERE/build2.sh" "$SRC" "$OUT/kpi2_out"

# in-tree modules (load order matters: dependencies first). Their .o files are the in-tree objects, the .ko is
# produced without a vmlinux symbol check.
MODS="drivers/nvme/host/nvme-core drivers/nvme/host/nvme drivers/usb/common/usb-common drivers/usb/core/usbcore \
drivers/usb/host/xhci-hcd drivers/usb/host/xhci-pci drivers/hid/hid drivers/hid/usbhid/usbhid drivers/hid/hid-generic drivers/usb/storage/usb-storage \
drivers/net/phy/libphy drivers/net/phy/mdio_devres drivers/net/phy/realtek drivers/net/ethernet/realtek/r8169 \
drivers/net/ethernet/intel/igc/igc drivers/net/ethernet/intel/e1000e/e1000e drivers/net/ethernet/intel/igb/igb"
make -C "$SRC" -j"$(nproc)" -k KBUILD_MODPOST_WARN=1 KCFLAGS="$KC" $(for m in $MODS; do echo -n "$m.ko "; done) >"$WORK/mods.log" 2>&1 || true

LIST="$OUT/kpi_out/kpi.ko;$OUT/kpi2_out/kpi2.ko"
for m in $MODS; do
  [ -f "$SRC/$m.ko" ] || { echo "missing $m.ko (see $WORK/mods.log)"; exit 1; }
  cp "$SRC/$m.ko" "$OUT/"
  LIST="$LIST;$OUT/$(basename "$m").ko"
done
echo -n "$LIST" > "$OUT/modules.list"
echo "built: $LIST"

# The GPU group (cmake -DLKPI_GPU_MODULES="$(cat <workdir>/out/gpu_modules.list)"): loaded only with the boot option
# /LINUXKPI_GPU, after the system volume is mounted. DRM core and helpers, amdgpu (Radeon), bochs (QEMU/Bochs VGA).
GPU_MODS="drivers/gpu/drm/drm drivers/gpu/drm/drm_kms_helper drivers/gpu/drm/ttm/ttm drivers/gpu/drm/drm_ttm_helper \
drivers/gpu/drm/display/drm_display_helper drivers/gpu/drm/scheduler/gpu-sched drivers/gpu/drm/drm_exec \
drivers/gpu/drm/drm_buddy drivers/gpu/drm/drm_suballoc_helper drivers/gpu/drm/drm_vram_helper \
drivers/gpu/drm/amd/amdxcp/amdxcp drivers/gpu/drm/amd/amdgpu/amdgpu drivers/gpu/drm/tiny/bochs"
make -C "$SRC" -j"$(nproc)" -k KBUILD_MODPOST_WARN=1 KCFLAGS="$KC" $(for m in $GPU_MODS; do echo -n "$m.ko "; done) >"$WORK/gpu.log" 2>&1 || true
mkdir -p "$OUT/gpu"
GLIST=""
for m in $GPU_MODS; do
  [ -f "$SRC/$m.ko" ] || { echo "missing $m.ko (see $WORK/gpu.log)"; exit 1; }
  cp "$SRC/$m.ko" "$OUT/gpu/"
  GLIST="$GLIST${GLIST:+;}$OUT/gpu/$(basename "$m").ko"
done
echo -n "$GLIST" > "$OUT/gpu_modules.list"
echo "GPU group: $GLIST"

# firmware for the CD (cmake -DLKPI_FIRMWARE_DIR=<workdir>/out/firmware), pinned and checksummed
"$HERE/fetch-firmware.sh" "$OUT/firmware"
