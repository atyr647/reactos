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

NTSTATUS NTAPI
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    ULONG i;

    DbgPrint("LKPI: Linux kernel module host starting (%u modules)\n", (ULONG)LKPI_MODULE_COUNT);
    LkpiInitAllocator();
    LkpiInitInput();
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
        if (InitResult != 0) break;
    }
    LkpiCreateDrmNodes(DriverObject);
    return STATUS_SUCCESS;
}
