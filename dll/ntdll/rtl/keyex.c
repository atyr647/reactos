/*
 * PROJECT:     ReactOS NT User-Mode Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     NtOpenKeyEx
 */

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

/*
 * @implemented
 * The open options (REG_OPTION_*) are not supported by the kernel and are ignored.
 */
NTSTATUS
NTAPI
NtOpenKeyEx(OUT PHANDLE KeyHandle,
            IN ACCESS_MASK DesiredAccess,
            IN POBJECT_ATTRIBUTES ObjectAttributes,
            IN ULONG OpenOptions)
{
    return NtOpenKey(KeyHandle, DesiredAccess, ObjectAttributes);
}
