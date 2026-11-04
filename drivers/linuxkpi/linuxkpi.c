/*
 * PROJECT:     ReactOS Linux kernel module host (LinuxKPI)
 * PURPOSE:     Driver entry. For now it loads \SystemRoot\lkpi\test.ko and runs its init function.
 */

#include "linuxkpi.h"

static PLKPI_MODULE LkpiTestModule;

static VOID
LkpiUnload(PDRIVER_OBJECT DriverObject)
{
    if (LkpiTestModule)
    {
        LkpiRunExit(LkpiTestModule);
        LkpiFreeModule(LkpiTestModule);
        LkpiTestModule = NULL;
    }
    DbgPrint("LKPI: unloaded\n");
}

NTSTATUS NTAPI
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING Path = RTL_CONSTANT_STRING(L"\\SystemRoot\\system32\\drivers\\lkpitest.ko");
    NTSTATUS Status;
    int InitResult = -1;

    DbgPrint("LKPI: Linux kernel module host starting\n");
    DriverObject->DriverUnload = LkpiUnload;

    Status = LkpiLoadModule(&Path, &LkpiTestModule);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("LKPI: no test module loaded (%08lx)\n", Status);
        return STATUS_SUCCESS;  /* the host stays loaded */
    }

    Status = LkpiRunInit(LkpiTestModule, &InitResult);
    DbgPrint("LKPI: init_module returned %d (status %08lx)\n", InitResult, Status);
    return STATUS_SUCCESS;
}
