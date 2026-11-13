# Trying this build on real hardware (Ryzen 9 5950X + RX 7900 XT)

Status: **untested on real hardware.** Everything below is a checklist, not a promise. Work through it in
order; stop at the first step that fails and capture the evidence (photo of the screen, serial log).

## 0. Get the ISO

* GitHub: Actions tab on the fork -> "x64 live ISO" -> Run workflow. The artifact
  `reactos-x64-live-release-<sha>` holds `reactos-x64-live-release.iso` and a `.sha256`.
  (Verify with `sha256sum -c`.) Tag `x64-live-<anything>` to publish it as a release asset instead.
* Local: `cmake -DARCH=amd64 -DCMAKE_BUILD_TYPE=Release -DDLL_EXPORT_VERSION=0xA00 ...; ninja bootcd`.

The workflow builds the Linux 6.12 modules from a pristine kernel tarball (`drivers/linuxkpi/kpi/build-all.sh`, verified
locally from scratch, not yet run on GitHub) and puts NVMe, USB xHCI and HID on the ISO. The LinuxKPI host then loads at
system start. If a PC misbehaves with it, choose **"ReactOS Live Environment (without Linux drivers)"** in the boot menu
(boot option `/NOLINUXKPI`). The amdgpu/DRM modules are not part of the ISO yet.

## 1. Test in a VM first (do this before touching hardware)

```
qemu-system-x86_64 -machine q35 -cpu Nehalem -smp 4 -m 4096 \
  -drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -device ich9-ahci,id=ahci -drive file=reactos.iso,if=none,id=cd,media=cdrom \
  -device ide-cd,drive=cd,bus=ahci.0 -vga std
```

`-cpu Nehalem` matters: ReactOS itself is fine, but some apps (Godot) need SSE4.2 and TCG's AVX2 gather is buggy.

## 2. Real hardware: known blockers, easiest first

| # | Item | What to expect |
|---|---|---|
| 1 | Release build | No debug prompts/asserts. **Verified:** Release amd64 (0xA00) ISO, 333 MB, builds with 0 failures and boots to the LiveCD GUI under QEMU/OVMF (UEFI, q35, `-cpu Nehalem`, 4 vCPUs). |
| 2 | UEFI boot of the ISO | Disable Secure Boot in firmware (ReactOS loaders are unsigned). Boot the USB in UEFI mode. Write the ISO with `dd` (hybrid) or Rufus in **DD mode**. |
| 3 | Keyboard/mouse + boot from USB | After UEFI exit, USB input may be dead on xHCI-only boards (no legacy emulation). The Linux xHCI/HID stack now provides keyboard and mouse in the live environment once it has loaded (the boot menu itself and the first seconds of boot still depend on firmware USB support). USB storage is not supported yet. Needs a USB mass-storage path that works on the board's xHCI. ReactOS xHCI support is weak; if the stick is not seen, try a USB 2.0 port / BIOS "xHCI hand-off". |
| 4 | NVMe | ReactOS has no native NVMe driver. The LinuxKPI NVMe path works in QEMU (loaded at boot; read+write, FAT volume mounted, see README) and is on the CI ISO, but only the *live* environment sees it: Setup (text mode) does not load it yet, so installing onto NVMe is not possible yet. Interrupts use MSI-X (verified in QEMU; real SSDs untested). |
| 5 | SMP | 5950X = 32 threads. Untested; if it hangs early, set `/ONECPU` (or `/NUMPROC=4`) in freeldr.ini. |
| 6 | Network | Realtek 2.5GbE / Intel I225 boards: no driver known to be present. Assume no network. |
| 7 | Display | Without the amdgpu path the 7900 XT is driven by GOP framebuffer only (no acceleration). |
| 8 | amdgpu via LinuxKPI | MSI/MSI-X now exist; still needs firmware blobs, VBIOS/ACPI, GEM mmap and a user-mode stack. |

## 3. What to capture if it fails

* Serial/debug output (`/DEBUG /DEBUGPORT=COM1` in freeldr.ini) or a phone photo of the BSOD / last text.
* Motherboard model + BIOS version; whether CSM/Secure Boot were on.


## 4. Text-mode Setup under UEFI (new in this branch)

Upstream ReactOS Setup could not be used on a UEFI machine: its text screen driver (`blue.sys`) wrote to VGA text memory,
and Setup had no UEFI bootloader installation. This branch adds, verified in QEMU/OVMF (NVMe-only VM):

* `blue.sys` paints the 80x25 text screen into the UEFI framebuffer when the firmware tree says "UEFI Internal".
* Setup lists the Linux-driven NVMe disk, creates a FAT32 partition, formats it and copies the system.
* Bootloader: `\EFI\BOOT\BOOTX64.EFI` (FreeLoader) and `freeldr.ini` on the system partition (the firmware's fallback boot
  path, no NVRAM entry). The system partition is the install partition (no separate ESP, no GPT yet), and the disk must be
  FAT32. The ARC path is `multi(0)disk(0)rdisk(N)partition(M)` with N = Setup's disk number, **edit `freeldr.ini` if the
  firmware orders disks differently**.
* `linuxkpi` is a boot-start driver, so the installed system can read its own NVMe disk. Boot option `/NOLINUXKPI`
  skips it (a system installed on NVMe then cannot boot, of course).

Limits: MBR only (a GPT disk is not handled by Setup), and install only onto a disk Setup can partition itself. On a PC that
already has other operating systems, use a separate, empty NVMe drive or disconnect the others while installing.
