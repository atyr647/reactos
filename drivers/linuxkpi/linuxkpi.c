/*
 * PROJECT:     ReactOS Linux kernel module host (LinuxKPI)
 * PURPOSE:     Driver entry. For now it loads \SystemRoot\lkpi\test.ko and runs its init function.
 */

#include "linuxkpi.h"
#include <ntstrsafe.h>

#ifndef LKPI_MODULE_COUNT
#define LKPI_MODULE_COUNT 0
#endif
#ifndef LKPI_GPU_MODULE_COUNT
#define LKPI_GPU_MODULE_COUNT 0
#endif
#define LKPI_MAX_MODULES 48

static PLKPI_MODULE LkpiModules[LKPI_MAX_MODULES];
static ULONG LkpiModuleCount;

/*
 * There is no DriverUnload: once a Linux module has run, its worker threads, timers, interrupt handlers and the disk
 * devices built on it reference module code, and Linux drivers' exit paths assume a kernel that can stop all of that.
 * Like other boot storage drivers, the host stays loaded until shutdown.
 */

/* CREATE/CLOSE/CLEANUP/DEVICE_CONTROL are shared by the DRM nodes and the disk devices */
static NTSTATUS NTAPI
LkpiDispatch(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);

    if (LkpiDisplayOwns(Dev))
        return LkpiDisplayDispatch(Dev, Irp);
    if (LkDiskOwns(Dev))
    {
        if (Sp->MajorFunction == IRP_MJ_DEVICE_CONTROL)
            return LkDiskControl(Dev, Irp);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
    switch (Sp->MajorFunction)
    {
        case IRP_MJ_CREATE: return DrmCreate(Dev, Irp);
        case IRP_MJ_DEVICE_CONTROL: return DrmControl(Dev, Irp);
        default: return DrmClean(Dev, Irp);
    }
}

/* internal device control: the disks' SCSI requests, or the display device's */
static NTSTATUS NTAPI
LkpiInternalDispatch(PDEVICE_OBJECT Dev, PIRP Irp)
{
    if (LkpiDisplayOwns(Dev))
        return LkpiDisplayDispatch(Dev, Irp);
    return LkDiskScsi(Dev, Irp);
}

/* TRUE if Option appears in the boot options (HKLM\SYSTEM\CurrentControlSet\Control\SystemStartOptions):
 * /NOLINUXKPI disables the host, /LINUXKPI_GPU loads the GPU module group. */
static BOOLEAN
LkpiBootOption(PCWSTR Option)
{
    UNICODE_STRING KeyName = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control");
    UNICODE_STRING ValName = RTL_CONSTANT_STRING(L"SystemStartOptions");
    OBJECT_ATTRIBUTES Oa;
    HANDLE Key;
    UCHAR Buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 512 * sizeof(WCHAR)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buf;
    ULONG Len, i, N, L = (ULONG)wcslen(Option);
    PWCHAR S;
    BOOLEAN Found = FALSE;

    InitializeObjectAttributes(&Oa, &KeyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&Key, KEY_QUERY_VALUE, &Oa)))
        return FALSE;
    if (NT_SUCCESS(ZwQueryValueKey(Key, &ValName, KeyValuePartialInformation, Buf, sizeof(Buf) - sizeof(WCHAR), &Len)) &&
        Info->Type == REG_SZ)
    {
        S = (PWCHAR)Info->Data;
        N = Info->DataLength / sizeof(WCHAR);
        S[N] = 0;
        /* whole words only: NOLINUXKPI must not match inside another option */
        for (i = 0; i + L <= N; i++)
            if (!_wcsnicmp(S + i, Option, L) && (i == 0 || S[i - 1] == L' ' || S[i - 1] == L'/') &&
                (S[i + L] == 0 || S[i + L] == L' ' || S[i + L] == L'='))
            {
                Found = TRUE;
                break;
            }
    }
    ZwClose(Key);
    return Found;
}

#if defined(LKPI_EMBEDDED) || LKPI_GPU_MODULE_COUNT > 0
/* Load and initialise one embedded module group. Returns FALSE if a module failed (the rest is not loaded). */
static BOOLEAN
LkpiLoadGroup(const ULONG_PTR *Table, ULONG Count, PCSTR Group)
{
    ULONG i;

    for (i = 0; i < Count; i++)
    {
        UNICODE_STRING Path = RTL_CONSTANT_STRING(L"embedded");
        PLKPI_MODULE M;
        NTSTATUS Status;
        int InitResult = -1;

        if (LkpiModuleCount >= LKPI_MAX_MODULES)
        {
            DbgPrint("LKPI: %s module %lu: too many modules\n", Group, i);
            return FALSE;
        }
        Status = LkpiLoadModuleEx(&Path, (const UCHAR *)Table[2 * i], (ULONG)(Table[2 * i + 1] - Table[2 * i]), &M);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint("LKPI: %s module %lu failed to load (%08lx)\n", Group, i, Status);
            return FALSE;
        }
        LkpiModules[LkpiModuleCount++] = M;
        Status = LkpiRunInit(M, &InitResult);
        if (Status == STATUS_ENTRYPOINT_NOT_FOUND)
        {
            /* library modules (drm_kms_helper, ...) have no init function */
            DbgPrint("LKPI: %s module %lu has no init_module\n", Group, i);
            continue;
        }
        DbgPrint("LKPI: %s module %lu init_module returned %d (status %08lx)\n", Group, i, InitResult, Status);
        LkpiLogFlush();
        LkpiDiskTryHook();
        if (InitResult != 0)
            return FALSE;
    }
    return TRUE;
}
#endif

#if LKPI_GPU_MODULE_COUNT > 0
/*
 * GPU drivers (DRM core, amdgpu, ...) are not boot drivers: they read their firmware from
 * \SystemRoot\system32\firmware, which exists only once the system volume is mounted. A worker thread waits for it,
 * then loads the group. Only with the boot option /LINUXKPI_GPU.
 */
static VOID NTAPI
LkpiGpuThread(PVOID Context)
{
    PDRIVER_OBJECT DriverObject = Context;
    UNICODE_STRING Dir = RTL_CONSTANT_STRING(L"\\SystemRoot\\system32");
    UNICODE_STRING Fw = RTL_CONSTANT_STRING(L"\\SystemRoot\\system32\\firmware");
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER Delay;
    HANDLE H;
    ULONG Waited;

    Delay.QuadPart = -10 * 1000 * 500;          /* 500 ms */
    for (Waited = 0; Waited < 240; Waited++)    /* up to 2 minutes */
    {
        InitializeObjectAttributes(&Oa, &Dir, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(ZwOpenFile(&H, FILE_LIST_DIRECTORY | SYNCHRONIZE, &Oa, &Iosb, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT)))
        {
            ZwClose(H);
            break;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    InitializeObjectAttributes(&Oa, &Fw, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenFile(&H, FILE_LIST_DIRECTORY | SYNCHRONIZE, &Oa, &Iosb, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT)))
    {
        ZwClose(H);
        DbgPrint("LKPI: GPU group: system volume ready after %lu ms, firmware directory present\n", Waited * 500);
    }
    else
    {
        DbgPrint("LKPI: GPU group: system volume %s after %lu ms, NO firmware directory\n",
                 Waited < 240 ? "ready" : "not ready", Waited * 500);
    }
    /* a GPU driver may hang or crash the machine: give the USB-stick log a chance to start first */
    if (!LkpiLogWait(20000))
        DbgPrint("LKPI: GPU group: no LKPILOG.TXT found on any volume, not logging to a file\n");
    LkpiLogFlush();
    if (LkpiLoadGroup(LkpiEmbeddedGpuTable, LKPI_GPU_MODULE_COUNT, "GPU"))
        DbgPrint("LKPI: GPU group loaded (%u modules)\n", (ULONG)LKPI_GPU_MODULE_COUNT);
    LkpiCreateDrmNodes(DriverObject);
    LkpiDisplayGpuLoaded();
    PsTerminateSystemThread(STATUS_SUCCESS);
}
#endif

NTSTATUS NTAPI
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    if (LkpiBootOption(L"NOLINUXKPI"))
    {
        DbgPrint("LKPI: disabled by /NOLINUXKPI\n");
        return STATUS_CANCELLED;
    }
    LkpiLogInit();
    DbgPrint("LKPI: Linux kernel module host starting (%u modules, %u GPU modules)\n", (ULONG)LKPI_MODULE_COUNT,
             (ULONG)LKPI_GPU_MODULE_COUNT);
    {
        /* hardware diagnostics for bring-up logs: is the GPU's VBIOS published by the firmware (ACPI VFCT)? */
        PULONG Facp = nt_acpi_get_table("FACP"), Vfct = nt_acpi_get_table("VFCT");
        DbgPrint("LKPI: ACPI tables %s; VFCT (GPU VBIOS) %s, %lu bytes\n", Facp ? "readable" : "NOT readable",
                 Vfct ? "present" : "absent", Vfct ? Vfct[1] : 0);
    }
    LkpiInitAllocator();
    LkpiInitInput();
    LkpiDiskInit(DriverObject);
    DriverObject->MajorFunction[IRP_MJ_CREATE] = LkpiDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = LkpiDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = LkpiDispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = LkpiDispatch;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = LkpiInternalDispatch;
    DriverObject->MajorFunction[IRP_MJ_PNP] = LkDiskPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = LkDiskPower;

#ifdef LKPI_EMBEDDED
    LkpiLoadGroup(LkpiEmbeddedTable, LKPI_MODULE_COUNT, "boot");
#endif
    LkpiCreateDrmNodes(DriverObject);
    LkpiDiskWaitIdle();
#if LKPI_GPU_MODULE_COUNT > 0
    if (LkpiBootOption(L"LINUXKPI_GPU"))
    {
        HANDLE Th;

        LkpiDisplayInit(DriverObject, TRUE);
        if (NT_SUCCESS(PsCreateSystemThread(&Th, THREAD_ALL_ACCESS, NULL, NULL, NULL, LkpiGpuThread, DriverObject)))
            ZwClose(Th);
        else
            LkpiDisplayGpuLoaded();     /* nothing to wait for */
    }
    else
    {
        DbgPrint("LKPI: GPU modules not loaded (boot option /LINUXKPI_GPU)\n");
    }
#endif
    return STATUS_SUCCESS;
}
