/*
 * PROJECT:     ReactOS Linux kernel module host (LinuxKPI)
 * PURPOSE:     Loads unmodified, position-independent Linux x86_64 kernel modules (ELF .ko /
 *              relocatable objects) into the ReactOS kernel and provides the Linux kernel API
 *              they call. The module code keeps the Linux (LP64, System V) ABI; the shim
 *              functions are compiled with the System V ABI too, so no thunks are needed.
 */
#pragma once

#include <ntifs.h>
#include <ntddk.h>

#define LKPI_TAG 'ikPL'

/* Linux's "long" is 64-bit; ours is not. */
typedef unsigned long long lx_ulong;
typedef long long lx_long;

#define LX_ABI __attribute__((sysv_abi))

typedef struct _LKPI_EXPORT
{
    const char *Name;
    PVOID Address;
} LKPI_EXPORT;

extern const LKPI_EXPORT LkpiExports[];
extern const ULONG LkpiExportCount;

typedef struct _LKPI_MODULE LKPI_MODULE, *PLKPI_MODULE;

NTSTATUS LkpiLoadModule(_In_ PCUNICODE_STRING Path, _Out_ PLKPI_MODULE *Module);
NTSTATUS LkpiRunInit(_In_ PLKPI_MODULE Module, _Out_ int *InitResult);
VOID LkpiRunExit(_In_ PLKPI_MODULE Module);
VOID LkpiFreeModule(_In_ PLKPI_MODULE Module);

void * LX_ABI LkpiMissing(const char *Name);
int LX_ABI lx_printk(const char *Fmt, lx_ulong a1, lx_ulong a2, lx_ulong a3, lx_ulong a4, lx_ulong a5);
