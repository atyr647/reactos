# Trying this build on real hardware (Ryzen 9 5950X + RX 7900 XT)

Status: **untested on real hardware.** Everything below is a checklist, not a promise. Work through it in
order; stop at the first step that fails and capture the evidence (photo of the screen, serial log).

## 0. Get the ISO

* GitHub: Actions tab on the fork -> "x64 live ISO" -> Run workflow. The artifact
  `reactos-x64-live-release-<sha>` holds `reactos-x64-live-release.iso` and a `.sha256`.
  (Verify with `sha256sum -c`.) Tag `x64-live-<anything>` to publish it as a release asset instead.
* Local: `cmake -DARCH=amd64 -DCMAKE_BUILD_TYPE=Release -DDLL_EXPORT_VERSION=0xA00 ...; ninja bootcd`.

The ISO built by CI does **not** contain the LinuxKPI `.ko` modules (they need the Linux 6.12 tree built
separately; see `README.md`). So CI ISOs boot ReactOS only; the GPU work is exercised in the local build.

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
| 1 | Release build | No debug prompts/asserts. Done in CI. |
| 2 | UEFI boot of the ISO | Disable Secure Boot in firmware (ReactOS loaders are unsigned). Boot the USB in UEFI mode. Write the ISO with `dd` (hybrid) or Rufus in **DD mode**. |
| 3 | Boot from USB | Needs a USB mass-storage path that works on the board's xHCI. ReactOS xHCI support is weak; if the stick is not seen, try a USB 2.0 port / BIOS "xHCI hand-off". |
| 4 | Installing to disk | **No NVMe driver in ReactOS.** An NVMe-only machine cannot install; use a SATA disk or stay on the live ISO. |
| 5 | SMP | 5950X = 32 threads. Untested; if it hangs early, set `/ONECPU` (or `/NUMPROC=4`) in freeldr.ini. |
| 6 | Network | Realtek 2.5GbE / Intel I225 boards: no driver known to be present. Assume no network. |
| 7 | Display | Without the amdgpu path the 7900 XT is driven by GOP framebuffer only (no acceleration). |
| 8 | amdgpu via LinuxKPI | Needs MSI interrupts, firmware blobs, VBIOS/ACPI. Not yet implemented; legacy INTx only today. |

## 3. What to capture if it fails

* Serial/debug output (`/DEBUG /DEBUGPORT=COM1` in freeldr.ini) or a phone photo of the BSOD / last text.
* Motherboard model + BIOS version; whether CSM/Secure Boot were on.
