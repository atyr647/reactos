# Bridging Windows (WDDM) GPU drivers and the Linux graphics stack on ReactOS

Status: design exploration. Everything marked *measured* comes from this repo's probe
(`win32ss/drivers/dxgkrnl`) running the real `viogpudo.sys` on ReactOS x64; everything else is analysis.

## What "talk directly" can mean

Both stacks are layered the same way, so there are only three seams where they can meet:

```
 user mode    D3D / Vulkan app
                 |  (UMD: amdxc64 / radv)
 ----------------+---------------------------------------------------- seam 1: UMD <-> kernel ABI
 kernel      "GPU core": memory mgr, scheduler, fences, mode setting, power
                 |  Windows: dxgkrnl (VidMM, VidSch, VidPN)    Linux: drm core + ttm + drm_sched + kms
 ----------------+---------------------------------------------------- seam 2: core <-> HW driver ABI
             HW driver: Windows miniport (amdkmdag.sys)          Linux (amdgpu.ko)
                 |
 ----------------+---------------------------------------------------- seam 3: HW driver <-> OS services
             Windows kernel API (ntoskrnl)                       Linux kernel API (LinuxKPI)
```

| Combination | Seams used | Meaning |
|---|---|---|
| A. Linux core under a Windows miniport | 2 + 3(NT) | "dxgkrnl built from DRM/TTM/sched": a Windows `.sys` KMD is driven by Linux's memory manager, scheduler and KMS code |
| B. Windows miniport under a Linux DRM node | 2 + 3(NT) | ndiswrapper-for-GPUs: a Windows KMD registers as a `drm_driver` so Linux userland (Mesa, Wayland) can use it |
| C. One kernel ABI, two backends | 1 | a D3DKMT-compatible front end (Windows userland API) over either backend A's core or a Linux driver |
| D. Windows UMD over a Linux KMD | 1 | AMD's closed D3D UMD talking to amdgpu: needs the UMD<->KMD private contract, not available |

## The WDDM contract we must satisfy (A, B)

Counted in the ReactOS headers (`sdk/include/ddk/dispmprt.h`, `d3dkmddi.h`):

* miniport -> OS: `DXGKRNL_INTERFACE` has **44 `DXGKCB_*` callbacks**.
* OS -> miniport: `DRIVER_INITIALIZATION_DATA` has **~149 DDI entry points** (293 distinct `DXGKDDI_*` names
  across WDDM versions).
* *Measured* with `viogpudo` (display-only): registers ~30 DDIs and, through `StartDevice`, uses only 3 callbacks
  (`GetDeviceInformation`, `ReadDeviceSpace`, `MapMemory`) plus the device's Driver registry key. A full render
  driver such as `amdkmdag` will use most of the 44 and nearly all of the DDIs.

### Mapping WDDM services onto Linux machinery

| WDDM piece | Linux equivalent | Notes |
|---|---|---|
| VidMM: `CreateAllocation`, `BuildPagingBuffer` (fill/transfer/map aperture), segments | TTM (`ttm_resource_manager`, `ttm_bo`, eviction) | Closest 1:1 match. Segment descriptors from `QueryAdapterInfo(QUERYSEGMENT)` become TTM placements (VRAM / GTT) |
| GPU VA (WDDM 2.x `MapGpuVirtualAddress`, `UpdateGpuVirtualAddress`) | `amdgpu_vm` / `drm_gpuvm` | Only meaningful with a GPUMMU-model KMD |
| VidSch: `SubmitCommand`, `PreemptCommand`, DMA fences, `DXGKCB_NOTIFY_INTERRUPT(DMA_COMPLETED)` | `drm_sched` + `dma_fence` | `DMA_COMPLETED` -> `dma_fence_signal`; `ResetFromTimeout` -> `drm_sched` timedout_job |
| VidPN: `IsSupportedVidPn`, `RecommendFunctionalVidPn`, `CommitVidPn`, `SetVidPnSourceAddress`, `PresentDisplayOnly` | KMS atomic: `drm_crtc` / `drm_plane` / `drm_connector`, `drm_framebuffer` | Reasonable for display-only; multiplane overlay and HDR are a long tail |
| `QueryChildRelations/Status`, `IndicateChildStatus` | connector hotplug, EDID | |
| `InterruptRoutine`, `DpcRoutine`, `SynchronizeExecution`, `QueueDpc` | `request_irq` + tasklet | Trivial on the NT side; the work is IRQL discipline |
| Power: `SetPowerState`, P-state / F-state callbacks | runtime PM | |
| Firmware / ACPI: `EvalAcpiMethod`, `QueryServices` | ACPI via ReactOS | Needed for real AMD hardware |

Direction A therefore buys an already-debugged memory manager, scheduler and fence machinery. Its cost is that
LinuxKPI must be solid first (269 symbols to go for DRM+TTM; the same dependency as the pure-Linux route, so none
of that work is wasted).

## Why B is mostly not worth it on ReactOS

On a Linux host B would let you run a Windows-only KMD. On ReactOS the Windows KMD already runs natively and no
Linux userland exists. The pieces of DRM we actually want are KMS (mode setting for a Windows-ABI display stack)
and TTM; neither requires exposing a DRM *node*.

## The hard blocker for real AMD hardware (A and C)

The WDDM split is not just kernel <-> miniport. The UMD (`amdxc64.dll`, `amdvlk64.dll`, ...) talks to the KMD
through *private driver data* in `CreateAllocation`, `CreateContext`, `Escape` and the DMA buffer contents. Those
formats are AMD-proprietary and undocumented. Consequences:

* Running AMD's Windows KMD requires AMD's Windows UMD, which requires a working Windows user-mode graphics stack
  (gdi32 `D3DKMT*` thunks: **337 declared in `d3dkmthk.h`**, DXGI, the d3d12 runtime). ReactOS has none of it.
* Replacing the AMD UMD with an open one (Mesa `radv`) would need radv's amdgpu-ioctl winsys rewritten to speak
  WDDM + AMD private data. PM4 packets are the same hardware format, but allocation, VA and sync semantics are
  not. That is a research project, not a driver port.
* Legal: redistributing or reverse engineering `amdkmdag` is not something this repo can ship; only
  user-supplied binaries could be loaded.

## Recommendation (staged)

1. **Finish WDDM display-only (DOD) on `viogpudo`**: VidPN callbacks, interrupt wiring, `PresentDisplayOnly`.
   This builds the `dxgkrnl` skeleton, fence/interrupt plumbing and the missing win32k path, and gives a
   testable framebuffer in QEMU that validates the seam-2 ABI end to end.
2. **Define a small "gpu-core" interface in the clone**: `alloc(handle,size,placement)`, `map(va)`,
   `submit(ctx,cmdbuf)->fence`, `fence_wait`, `set_scanout(fb)`. Back it first with a trivial implementation
   (system memory, no GPU), then with TTM+drm_sched via LinuxKPI. A WDDM KMD plugs in below it (A); `amdgpu.ko`
   plugs in below it too (the pure Linux route). Same front end, two interchangeable backends (option C), the
   only design that keeps both routes alive.
3. **Do not start on `amdkmdag`** until step 2 exists and a user-supplied driver package can be measured (which
   callbacks/DDIs it really uses). Ask for that driver as input rather than guessing.
4. Long-term path to the 7900 XT stays: LinuxKPI -> `amdgpu.ko` -> DRM/TTM -> a D3DKMT front end (step 2) ->
   Vulkan via `radv` for DXVK/vkd3d. The Windows-miniport side of seam 2 is for display-only devices and experiments.

## Prior art worth reading before building step 2

* Microsoft's Linux `dxgkrnl` (WSL2, GPL): the reverse direction (Linux guest -> Windows host over VMBus), but
  the only open definition of the D3DKMT ioctl structures and handle model.
* Mesa's `d3d12` gallium driver and `dzn`: userland that speaks D3DKMT, useful as a client to test a front end.
* ReactOS `win32ss/drivers/videoprt` and the VideoPort miniports: the legacy model win32k uses today; a WDDM-DOD
  path must coexist with it.
