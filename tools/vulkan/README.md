# Vulkan on ReactOS x64 (lavapipe, software)

`build-vulkan-x64.sh` cross-builds the Khronos Vulkan loader and Mesa's lavapipe (llvmpipe + LLVM 18 JIT) for ReactOS
x64 with mingw-w64. `vktest.c` loads `vulkan-1.dll`, picks the lavapipe ICD through `VK_DRIVER_FILES`, creates an
instance/device and waits on a queue. `rosimp.py` lists imports a built binary needs that a ReactOS build does not
export (use it on every new DLL before booting).

## Result (QEMU, ReactOS x64 UEFI)
    VKTEST: 1 physical device(s)
    VKTEST: device 'llvmpipe (LLVM 18.1.3, 128 bits)' api 1.4.354 driver 6802006 type 4
    VKTEST: vkCreateDevice -> 0 ; vkQueueWaitIdle -> 0 ; PASS

## What had to change in ReactOS
* `RtlWaitOnAddress` / `RtlWakeAddress*` were stubs returning STATUS_NOT_IMPLEMENTED; now a real keyed-event
  implementation (`sdk/lib/rtl/waitaddress.c`). Mesa's futex layer uses `WaitOnAddress`, so `WaitOnAddress` /
  `WakeByAddress*` are exported from kernel32 (and kernel32_vista) and `Rtl*` from ntdll.
* The Vista+ exports are gated on `DLL_EXPORT_VERSION` (default `0x502`). Binaries built for Windows 10 import
  `api-ms-win-core-*` sets that resolve to kernel32, so they need `-DDLL_EXPORT_VERSION=0xA00` (full rebuild).

## Next gates
vkcube (win32 WSI), DXVK D3D11 sample, vkd3d-proton D3D12 sample, then radv on amdgpu (LinuxKPI).
