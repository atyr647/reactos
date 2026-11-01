/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Windows Vista to Windows 10 process/thread helper functions
 *
 * Most of these are hints or feature probes. Where ReactOS has no equivalent
 * (mitigation policies, power requests, memory priority) the function validates
 * its arguments and reports success without enforcing anything; this is noted
 * on each function. Callers such as Chromium only probe these.
 */

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* Win32 values that the older headers used here do not define */
#define K32_LOAD_LIBRARY_SEARCH_MASK 0x00001F00

/*
 * @implemented
 * The attribute list (ideal processor, group affinity, ...) is ignored.
 */
HANDLE
WINAPI
CreateRemoteThreadEx(IN HANDLE hProcess,
                     IN LPSECURITY_ATTRIBUTES lpThreadAttributes,
                     IN SIZE_T dwStackSize,
                     IN LPTHREAD_START_ROUTINE lpStartAddress,
                     IN LPVOID lpParameter,
                     IN DWORD dwCreationFlags,
                     IN PVOID lpAttributeList,
                     OUT LPDWORD lpThreadId)
{
    return CreateRemoteThread(hProcess,
                              lpThreadAttributes,
                              dwStackSize,
                              lpStartAddress,
                              lpParameter,
                              dwCreationFlags,
                              lpThreadId);
}

/*
 * @implemented
 * Only a hint that the contents are no longer needed; nothing is discarded.
 */
DWORD
WINAPI
DiscardVirtualMemory(IN PVOID VirtualAddress,
                     IN SIZE_T Size)
{
    if (!VirtualAddress || !Size) return ERROR_INVALID_PARAMETER;
    return ERROR_SUCCESS;
}

/*
 * @implemented
 */
WORD
WINAPI
GetMaximumProcessorGroupCount(VOID)
{
    /* Processor groups are not supported: a single group */
    return 1;
}

/*
 * @implemented
 * No mitigation policy is ever enabled, so the policy structure is zeroed.
 */
BOOL
WINAPI
GetProcessMitigationPolicy(IN HANDLE hProcess,
                           IN INT MitigationPolicy,
                           OUT PVOID lpBuffer,
                           IN SIZE_T dwLength)
{
    if (!lpBuffer || dwLength < sizeof(DWORD) || dwLength > 64 ||
        MitigationPolicy < 0 || MitigationPolicy > 20)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    RtlZeroMemory(lpBuffer, dwLength);
    return TRUE;
}

/*
 * @implemented
 * The request is validated and accepted, but the policy is NOT enforced.
 */
BOOL
WINAPI
SetProcessMitigationPolicy(IN INT MitigationPolicy,
                           IN PVOID lpBuffer,
                           IN SIZE_T dwLength)
{
    if (!lpBuffer || dwLength < sizeof(DWORD) || dwLength > 64 ||
        MitigationPolicy < 0 || MitigationPolicy > 20)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    DPRINT1("SetProcessMitigationPolicy(%d): accepted but not enforced\n", MitigationPolicy);
    return TRUE;
}

/*
 * @implemented
 * Power requests are not acted upon; the handle can be passed to the other
 * PowerXxxRequest functions and closed with CloseHandle.
 */
HANDLE
WINAPI
PowerCreateRequest(IN PVOID Context)
{
    if (!Context)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    return CreateEventW(NULL, FALSE, FALSE, NULL);
}

/*
 * @implemented
 */
BOOL
WINAPI
PowerSetRequest(IN HANDLE PowerRequest,
                IN INT RequestType)
{
    if (!PowerRequest || PowerRequest == INVALID_HANDLE_VALUE || RequestType < 0 || RequestType > 3)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
PowerClearRequest(IN HANDLE PowerRequest,
                  IN INT RequestType)
{
    return PowerSetRequest(PowerRequest, RequestType);
}

/*
 * @implemented
 * Prefetching is only a hint; nothing is prefetched.
 */
BOOL
WINAPI
PrefetchVirtualMemory(IN HANDLE hProcess,
                      IN ULONG_PTR NumberOfEntries,
                      IN PVOID VirtualAddresses,
                      IN ULONG Flags)
{
    if (!hProcess || !NumberOfEntries || !VirtualAddresses || Flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

/*
 * @implemented
 * Locale names are returned unchanged (a name resolves to itself).
 */
INT
WINAPI
ResolveLocaleName(IN LPCWSTR lpNameToResolve,
                  OUT LPWSTR lpLocaleName,
                  IN INT cchLocaleName)
{
    INT Length;

    if (!lpNameToResolve || cchLocaleName < 0 || (cchLocaleName && !lpLocaleName))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    Length = lstrlenW(lpNameToResolve) + 1;
    if (cchLocaleName == 0) return Length;
    if (cchLocaleName < Length)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    RtlCopyMemory(lpLocaleName, lpNameToResolve, Length * sizeof(WCHAR));
    return Length;
}

/*
 * @implemented
 * The flags are validated and accepted; the loader's search order is unchanged.
 */
BOOL
WINAPI
SetDefaultDllDirectories(IN DWORD DirectoryFlags)
{
    if (!DirectoryFlags || (DirectoryFlags & ~K32_LOAD_LIBRARY_SEARCH_MASK))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

/*
 * @implemented
 * ProcessMemoryPriority(0), ProcessPowerThrottling(4), ProcessProtectionLevelInfo(7),
 * ProcessLeapSecondInfo(8), ProcessMachineTypeInfo(9) are accepted without effect.
 */
BOOL
WINAPI
SetProcessInformation(IN HANDLE hProcess,
                      IN INT ProcessInformationClass,
                      IN PVOID ProcessInformation,
                      IN DWORD ProcessInformationSize)
{
    if (!hProcess || !ProcessInformation || ProcessInformationSize < sizeof(DWORD))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    switch (ProcessInformationClass)
    {
        case 0: case 4: case 7: case 8: case 9:
            return TRUE;
        default:
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
    }
}

/*
 * @implemented
 * ThreadMemoryPriority(0), ThreadAbsoluteCpuPriority(1), ThreadDynamicCodePolicy(2),
 * ThreadPowerThrottling(3) are accepted without effect.
 */
BOOL
WINAPI
SetThreadInformation(IN HANDLE hThread,
                     IN INT ThreadInformationClass,
                     IN PVOID ThreadInformation,
                     IN DWORD ThreadInformationSize)
{
    if (!hThread || !ThreadInformation || ThreadInformationSize < sizeof(DWORD))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    switch (ThreadInformationClass)
    {
        case 0: case 1: case 2: case 3:
            return TRUE;
        default:
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
    }
}

/*
 * @implemented
 * There is no Windows Error Reporting, so nothing is registered.
 */
HRESULT
WINAPI
WerRegisterRuntimeExceptionModule(IN PCWSTR pwszOutOfProcessCallbackDll,
                                  IN PVOID pContext)
{
    if (!pwszOutOfProcessCallbackDll) return E_INVALIDARG;
    return S_OK;
}

/*
 * @implemented
 */
VOID
WINAPI
QueryUnbiasedInterruptTimePrecise(OUT PULONGLONG lpUnbiasedInterruptTimePrecise)
{
    LARGE_INTEGER Time;

    do
    {
        Time.HighPart = SharedUserData->InterruptTime.High1Time;
        Time.LowPart = SharedUserData->InterruptTime.LowPart;
    } while (Time.HighPart != SharedUserData->InterruptTime.High2Time);

    *lpUnbiasedInterruptTimePrecise = Time.QuadPart;
}

/*
 * @implemented
 * Two handles refer to the same kernel object if the system handle table
 * reports the same object pointer for both.
 */
BOOL
WINAPI
CompareObjectHandles(IN HANDLE hFirstObjectHandle,
                     IN HANDLE hSecondObjectHandle)
{
    NTSTATUS Status;
    PSYSTEM_HANDLE_INFORMATION Info = NULL;
    ULONG Size = 0x10000, Needed, Attempt, i;
    PVOID First = NULL, Second = NULL;
    ULONG_PTR Pid = (ULONG_PTR)NtCurrentTeb()->ClientId.UniqueProcess;
    BOOL Result = FALSE;

    if (hFirstObjectHandle == hSecondObjectHandle) return TRUE;

    for (Attempt = 0; Attempt < 6; Attempt++)
    {
        Info = RtlAllocateHeap(RtlGetProcessHeap(), 0, Size);
        if (!Info)
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
        Status = NtQuerySystemInformation(SystemHandleInformation, Info, Size, &Needed);
        if (Status != STATUS_INFO_LENGTH_MISMATCH) break;
        RtlFreeHeap(RtlGetProcessHeap(), 0, Info);
        Info = NULL;
        Size = Needed + 0x4000;
    }

    if (!Info || !NT_SUCCESS(Status))
    {
        if (Info) RtlFreeHeap(RtlGetProcessHeap(), 0, Info);
        BaseSetLastNTError(NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status);
        return FALSE;
    }

    for (i = 0; i < Info->NumberOfHandles; i++)
    {
        if (Info->Handles[i].UniqueProcessId != Pid) continue;
        if (Info->Handles[i].HandleValue == (USHORT)(ULONG_PTR)hFirstObjectHandle) First = Info->Handles[i].Object;
        if (Info->Handles[i].HandleValue == (USHORT)(ULONG_PTR)hSecondObjectHandle) Second = Info->Handles[i].Object;
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, Info);

    if (!First || !Second)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    Result = (First == Second);
    return Result;
}
