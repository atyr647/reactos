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
drivers/usb/host/xhci-hcd drivers/usb/host/xhci-pci drivers/hid/hid drivers/hid/usbhid/usbhid drivers/hid/hid-generic drivers/usb/storage/usb-storage"
make -C "$SRC" -j"$(nproc)" -k KBUILD_MODPOST_WARN=1 KCFLAGS="$KC" $(for m in $MODS; do echo -n "$m.ko "; done) >"$WORK/mods.log" 2>&1 || true

LIST="$OUT/kpi_out/kpi.ko;$OUT/kpi2_out/kpi2.ko"
for m in $MODS; do
  [ -f "$SRC/$m.ko" ] || { echo "missing $m.ko (see $WORK/mods.log)"; exit 1; }
  cp "$SRC/$m.ko" "$OUT/"
  LIST="$LIST;$OUT/$(basename "$m").ko"
done
echo -n "$LIST" > "$OUT/modules.list"
echo "built: $LIST"
