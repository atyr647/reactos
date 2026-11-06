/*
 * PROJECT:     ReactOS Linux kernel module host (LinuxKPI)
 * PURPOSE:     Driver entry. For now it loads \SystemRoot\lkpi\test.ko and runs its init function.
 */

#include "linuxkpi.h"
#include <ntstrsafe.h>

#ifndef LKPI_MODULE_COUNT
#define LKPI_MODULE_COUNT 0
#endif
#define LKPI_MAX_MODULES 16

static PLKPI_MODULE LkpiModules[LKPI_MAX_MODULES];
static ULONG LkpiModuleCount;

static VOID
LkpiUnload(PDRIVER_OBJECT DriverObject)
{
    while (LkpiModuleCount)
    {
        PLKPI_MODULE M = LkpiModules[--LkpiModuleCount];
        LkpiRunExit(M);
        LkpiFreeModule(M);
    }
    DbgPrint("LKPI: unloaded\n");
}

/* CREATE/CLOSE/CLEANUP/DEVICE_CONTROL are shared by the DRM nodes and the disk devices */
static NTSTATUS NTAPI
LkpiDispatch(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);

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

NTSTATUS NTAPI
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    ULONG i;

    DbgPrint("LKPI: Linux kernel module host starting (%u modules)\n", (ULONG)LKPI_MODULE_COUNT);
    LkpiInitAllocator();
    LkpiInitInput();
    LkpiDiskInit(DriverObject);
    DriverObject->MajorFunction[IRP_MJ_CREATE] = LkpiDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = LkpiDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = LkpiDispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = LkpiDispatch;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = LkDiskScsi;
    DriverObject->MajorFunction[IRP_MJ_PNP] = LkDiskPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = LkDiskPower;
    DriverObject->DriverUnload = LkpiUnload;

    for (i = 0; i < LKPI_MODULE_COUNT && i < LKPI_MAX_MODULES; i++)
    {
        WCHAR PathBuf[96];
        UNICODE_STRING Path;
        PLKPI_MODULE M;
        NTSTATUS Status;
        int InitResult = -1;

        RtlStringCbPrintfW(PathBuf, sizeof(PathBuf), L"\\SystemRoot\\system32\\drivers\\lkpimod%u.ko", i);
        RtlInitUnicodeString(&Path, PathBuf);
        Status = LkpiLoadModule(&Path, &M);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint("LKPI: module %u failed to load (%08lx)\n", i, Status);
            break;
        }
        LkpiModules[LkpiModuleCount++] = M;
        Status = LkpiRunInit(M, &InitResult);
        if (Status == STATUS_ENTRYPOINT_NOT_FOUND)
        {
            /* library modules (drm_kms_helper, ...) have no init function */
            DbgPrint("LKPI: module %u has no init_module\n", i);
            InitResult = 0;
            continue;
        }
        DbgPrint("LKPI: module %u init_module returned %d (status %08lx)\n", i, InitResult, Status);
        LkpiDiskTryHook();
        if (InitResult != 0) break;
    }
    LkpiCreateDrmNodes(DriverObject);
    return STATUS_SUCCESS;
}
