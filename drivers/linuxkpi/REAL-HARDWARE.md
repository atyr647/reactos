# Trying this build on real hardware (Ryzen 9 5950X + RX 7900 XT)

Status: **untested on real hardware.** Everything below is a checklist, not a promise. Work through it in
order; stop at the first step that fails and capture the evidence (photo of the screen, serial log).

## 0. Get the ISO

* GitHub: Actions tab on the fork -> "x64 live ISO" -> Run workflow. The artifact
  `reactos-x64-live-release-<sha>` holds `reactos-x64-live-release.iso` and a `.sha256`.
  (Verify with `sha256sum -c`.) Tag `x64-live-<anything>` to publish it as a release asset instead.
* Local: `cmake -DARCH=amd64 -DCMAKE_BUILD_TYPE=Release -DDLL_EXPORT_VERSION=0xA00 ...; ninja bootcd`.

The workflow builds the Linux 6.12 modules from a pristine kernel tarball (`drivers/linuxkpi/kpi/build-all.sh`, verified
locally from scratch, not yet run on GitHub): NVMe, USB (xHCI, HID, mass storage) and Ethernet (r8169, igc, e1000e, igb)
load at system start; the GPU group (DRM + amdgpu) is on the ISO too but only loads from the experimental boot entry
(section 5). It also fetches the needed firmware (`drivers/linuxkpi/kpi/fetch-firmware.sh`). If a PC misbehaves with the
Linux drivers, choose **"ReactOS Live Environment (without Linux drivers)"** in the boot menu (boot option
`/NOLINUXKPI`).

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
| 1 | Release build | No debug prompts/asserts. **Verified:** Release amd64 (0xA00) ISO builds with 0 failures and boots to the LiveCD GUI under QEMU/OVMF (UEFI, q35, `-cpu Nehalem`, 4 vCPUs). |
| 2 | UEFI boot of the ISO | Disable Secure Boot in firmware (ReactOS loaders are unsigned). Boot the USB in UEFI mode. Write the ISO with `dd` (hybrid) or Rufus in **DD mode**. |
| 3 | Keyboard/mouse + USB sticks | The Linux xHCI/HID stack provides keyboard and mouse once it has loaded (the boot menu and the first seconds still depend on firmware USB support). USB sticks work as disks (read/write, flushed at shutdown), and the stick the ISO was written to can be the boot medium: pick a "USB stick / RAM disk" entry. All verified in QEMU. |
| 4 | NVMe | Linux NVMe driver at boot (MSI-X): Setup lists the disk, installs onto it, and the installed system boots from it alone. Verified in QEMU; real SSDs untested. |
| 5 | SMP | 5950X = 32 threads. Untested; if it hangs early, set `/ONECPU` (or `/NUMPROC=4`) in freeldr.ini. The Linux modules now use locked atomics on every CPU (they were built uniprocessor and did not). |
| 6 | Network | Linux drivers behind an NDIS miniport: **r8169** (Realtek RTL8111/8168/8125/8126, most X570/B550 boards), **igc** (Intel I225-V/I226-V), **e1000e**, **igb**. e1000e and igb are verified in QEMU (DHCP, ping, 64 MiB each way over TCP, no corrupted byte); r8169 and igc load but have never seen their hardware. Realtek firmware patches are on the CD. |
| 7 | Display | The 7900 XT is driven by the GOP framebuffer (no acceleration) unless the experimental GPU entry is used (section 5). |
| 8 | amdgpu via LinuxKPI | See section 5: builds and loads (with the display core, DC), never run on an AMD GPU. |

## 3. What to capture if it fails

* A phone photo of the BSOD or the last text on screen.
* **The LinuxKPI log on a USB stick** (works on machines without a serial port): format a second USB stick as FAT32,
  create an empty file named `LKPILOG.TXT` in its root, and leave it plugged in. Everything the Linux drivers print,
  plus the host's own messages, is appended to it every 200 ms and flushed so it survives a crash or power-off. Only a
  volume that already has that file is ever written to.
* Serial output (`/DEBUG /DEBUGPORT=COM1` in freeldr.ini) if the board has a COM port.
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

**Verified end to end in QEMU/OVMF with an NVMe-only VM (no SATA disk, no CD after the install):** text-mode Setup shows the
NVMe disk, creates and formats FAT32, copies the system, writes `\EFI\BOOT\BOOTX64.EFI` + `freeldr.ini`; after the reboot
the firmware starts FreeLoader from the NVMe disk, `linuxkpi` loads as a boot driver, the disk appears, the kernel mounts
`C:\ReactOS` and the GUI second stage (Setup Wizard) runs from the NVMe disk.

Bugs fixed on this path: the arena allocator deadlocked against the NVMe completion interrupt during the large file copy
(interrupts are now masked while its lock is held); Setup's firmware-disk assumptions (BIOS disks, active partition, `scsi()` ARC
paths) are bypassed on UEFI.


## 5. Experimental: amdgpu on the RX 7900 XT (test only)

Boot menu entry **"ReactOS Live (experimental AMD GPU driver, test only)"**. It boots the live system from RAM and,
once the system volume is mounted, loads the GPU group: Linux 6.12 DRM core, TTM, the GPU scheduler and `amdgpu`, with
the Navi 31 firmware from `reactos\system32\firmware\amdgpu` (fetched by `fetch-firmware.sh`, pinned to
linux-firmware 20241110, checksummed). Debug output goes to the screen; put the `LKPILOG.TXT` stick in (section 3)
first: the GPU group waits up to 20 s for it so that the driver's first lines are not lost.

What is in place: PCI probe, BARs, MSI/MSI-X, firmware loading, VBIOS from the ACPI VFCT table (the HAL now hands ACPI
tables to drivers; the log says whether VFCT is present) or the ROM BAR, DRM device nodes for user mode
(`\\.\LKPIDRM0`, size- and pointer-checked ioctls), and mapping GPU buffers into a program (`IOCTL_LKPI_DRM_MMAP`,
VRAM write-combined; the mapping follows the buffer when the driver moves it between VRAM and system memory). The
mapping path is verified with bochs in QEMU, not with amdgpu.

The display core (DC) is built in, with the DCN 3.2 display firmware, so amdgpu can drive the monitor itself, and the
desktop then runs on it: the LinuxKPI display miniport asks amdgpu to set the monitor's preferred mode and the desktop
draws into amdgpu's scanout buffer in VRAM (`LKPI: display: desktop on card 0 (...)` in the log). This is verified
with the bochs DRM driver in QEMU, not with amdgpu. If amdgpu fails, or finds no monitor, the desktop falls back to the
firmware framebuffer; if amdgpu takes the display over and then fails to set a mode, the screen stays dark: the USB
log says how far it got.

**Vulkan on the GPU (RADV).** `tools/vulkan/build-radv-x64.sh` builds Mesa's RADV for ReactOS on top of this driver.
To try it, put `vulkan-1.dll` (from `build-vulkan-x64.sh`), `vulkan_radeon.dll`, `radeon_icd.x86_64.json`,
`amdgpu.ids`, `radvtest.exe` and `libdrmtest.exe` in one folder on a USB stick, boot the experimental GPU entry, and run
`radvtest.exe` from a command prompt (if the screen is dark, type blind, or look at the `LKPILOG.TXT` stick
afterwards; `radvtest` prints to the debugger and the console). What it should say on a working
RX 7900 XT:

    RADVTEST:   device 0: 'AMD Radeon RX 7900 XT (RADV NAVI31)' 1002:744c ...
    RADVTEST: ok   vkCreateDevice ...
    RADVTEST: ok   vkCreateComputePipelines: the shader compiled for the GPU (ACO)
    RADVTEST: ok   the GPU ran the shader: 0 of 16384 values wrong ...
    RADVTEST: PASS

Verified in QEMU only up to "RADV loads, enumerates the DRM devices through libdrm and finds no AMD GPU".

The deferred loading path is verified in QEMU with the bochs DRM driver standing in for amdgpu (QEMU cannot emulate a
Radeon). amdgpu itself has never run on hardware; expect it to stop somewhere during IP block init, and send the log.
