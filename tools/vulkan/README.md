# Vulkan on ReactOS x64 (lavapipe, software)

`build-vulkan-x64.sh` cross-builds the Khronos Vulkan loader and Mesa's lavapipe (llvmpipe + LLVM 18 JIT) for ReactOS
x64 with mingw-w64. `vktest.c` loads `vulkan-1.dll`, picks the lavapipe ICD through `VK_DRIVER_FILES`, creates an
instance/device and waits on a queue. `rosimp.py` lists imports a built binary needs that a ReactOS build does not
export (use it on every new DLL before booting).

## Result (QEMU, ReactOS x64 UEFI)
    VKTEST: 1 physical device(s)
    VKTEST: device 'llvmpipe (LLVM 18.1.3, 128 bits)' api 1.4.354 driver 6802006 type 4
    VKTEST: vkCreateDevice -> 0 ; vkQueueWaitIdle -> 0 ; PASS

`vkpresent.c` creates a Win32 window, a `VK_KHR_win32_surface` swapchain and clears/presents 200 frames (lavapipe
renders into the window through GDI): all steps return VK_SUCCESS and the window shows the animated clear colour.
`loader-win64.patch` lets the loader skip `dxgi.dll` (`VK_LOADER_NO_DXGI=1`): ReactOS's wined3d otherwise shows a
blocking "needs OpenGL 2.0" box.

## What had to change in ReactOS
* `RtlWaitOnAddress` / `RtlWakeAddress*` were stubs returning STATUS_NOT_IMPLEMENTED; now a real keyed-event
  implementation (`sdk/lib/rtl/waitaddress.c`). Mesa's futex layer uses `WaitOnAddress`, so `WaitOnAddress` /
  `WakeByAddress*` are exported from kernel32 (and kernel32_vista) and `Rtl*` from ntdll.
* The Vista+ exports are gated on `DLL_EXPORT_VERSION` (default `0x502`). Binaries built for Windows 10 import
  `api-ms-win-core-*` sets that resolve to kernel32, so they need `-DDLL_EXPORT_VERSION=0xA00` (full rebuild).

## DXVK (D3D11) on lavapipe on ReactOS
`d3d11test.c` loads DXVK 3.1.1's `d3d11.dll`/`dxgi.dll` (release build, mingw + UCRT) from the exe directory, creates a
hardware device (feature level 11.0) + swapchain, compiles HLSL with ReactOS's `d3dcompiler_47`, and draws a shaded
triangle every frame. DXVK finds `llvmpipe (LLVM 18.1.3)` through the Vulkan loader. DXVK needed 9 more gdi32 D3DKMT
entry points (shared resources / keyed mutex); they are stubs returning STATUS_NOT_SUPPORTED
(`win32ss/gdi/gdi32_vista/d3dkmt_share.c`).

## vkd3d-proton (D3D12)
`d3d12test.c` loads vkd3d-proton 3.1's `d3d12.dll` + DXVK's `dxgi.dll`, creates a D3D12 device (FL 11_0), direct queue,
fence, flip-model swapchain, RTV heap and records/executes clear + barriers each frame with fence sync. All calls
succeed on lavapipe and the window shows the animated clear colour. vkd3d-proton needed `PathCchCombineEx` /
`PathCchRemoveFileSpec` (`api-ms-win-core-path-l1-1-0`); minimal PathCch* helpers were added to kernel32 (exported
`-private` so shell32, which links its own copy, still builds).

## Pitfalls found while bringing this up (all fixed in the patch / tests)
* The loader's GAS trampolines for unknown device/physical-device extension functions assumed the System V x64 ABI
  (`rdi`); on Windows x64 the first argument is `rcx`. DXVK calls such functions, so it crashed in the loader.
  `loader-win64.patch` adds the Win64 variant.
* The loader calls `CreateDXGIFactory1` unconditionally; the opt-out patch (`VK_LOADER_NO_DXGI=1`) now installs a stub
  instead of a NULL pointer (a NULL call looked like a silent process death for a long time).
* `SetEnvironmentVariable` does not update the C runtime's `environ`; tests also call `_putenv`.
* `OutputDebugString` raises an exception that vectored handlers see; a handler that logs with `OutputDebugString`
  recurses until the stack is gone. (An earlier note blaming `AddVectoredExceptionHandler` was wrong.)

## Godot 4.7.2 (real engine) notes
* Godot needs SSE4.2: run QEMU with `-cpu Nehalem` (or a KVM/real CPU). With `-cpu max` (AVX2) llvmpipe emits AVX2
  gathers and the process dies in JIT code; `gathertest.c` shows QEMU's TCG returning zeros for an unmasked
  `vpgatherdd`, while `avxtest.c` shows ReactOS preserves YMM state across preemption, so this is QEMU emulation, not
  ReactOS (re-verify on real hardware).
* llvmpipe's JIT needs all sections within 2 GiB of each other (COFF RIP-relative relocations): `patch-mesa.py`
  switches to the Large code model and an arena memory manager.

* Open: with `-cpu Nehalem` Godot 4.7.2 (mobile renderer, 400x300) gets as far as drawing, then llvmpipe-generated
  code calls a NULL function pointer (`UM-AV ... rip 0`, return address inside the JIT arena; the kernel log shows
  registers and a stack dump). `EnumProcessModulesEx` is implemented now but is not the cause. Next step: log
  unresolved/NULL symbols in the arena memory manager (`getSymbolAddress`) and capture the shader that triggers it
  (`LP_DEBUG`/`GALLIVM_DEBUG`).

## Next gates
vkcube (win32 WSI), DXVK D3D11 sample, vkd3d-proton D3D12 sample, then radv on amdgpu (LinuxKPI).
