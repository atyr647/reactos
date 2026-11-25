/*
 * PROJECT:     ReactOS Runtime Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     WaitOnAddress / WakeByAddress implementation
 *
 * Waiters register an on-stack entry in a hash bucket and sleep on a keyed event whose key is the
 * entry itself. A waker removes matching entries under the bucket lock and then releases the
 * keyed event of each removed entry; the waiter either is sleeping already or will consume the
 * release in its timeout path.
 */

/* INCLUDES *****************************************************************/

#include <rtl_vista.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

#define WAIT_BUCKETS 64

typedef struct _ADDR_WAITER
{
    LIST_ENTRY Link;
    const volatile VOID *Address;
} ADDR_WAITER;

typedef struct _ADDR_BUCKET
{
    volatile LONG Lock;
    LIST_ENTRY Waiters;
} ADDR_BUCKET;

static ADDR_BUCKET RtlpAddrBuckets[WAIT_BUCKETS];
static volatile LONG RtlpAddrInit; /* 0 = no, 1 = in progress, 2 = done */

/* PRIVATE FUNCTIONS ********************************************************/

static
VOID
RtlpAddrEnsureInit(VOID)
{
    ULONG i;

    if (RtlpAddrInit == 2)
        return;
    if (InterlockedCompareExchange(&RtlpAddrInit, 1, 0) == 0)
    {
        for (i = 0; i < WAIT_BUCKETS; i++)
            InitializeListHead(&RtlpAddrBuckets[i].Waiters);
        InterlockedExchange(&RtlpAddrInit, 2);
    }
    else
    {
        while (RtlpAddrInit != 2)
            YieldProcessor();
    }
}

static
ADDR_BUCKET *
RtlpAddrBucket(const volatile VOID *Address)
{
    ULONG_PTR p = (ULONG_PTR)Address >> 3;
    return &RtlpAddrBuckets[(p ^ (p >> 6)) % WAIT_BUCKETS];
}

static
VOID
RtlpAddrLock(ADDR_BUCKET *B)
{
    while (InterlockedCompareExchange(&B->Lock, 1, 0) != 0)
        YieldProcessor();
}

static
VOID
RtlpAddrUnlock(ADDR_BUCKET *B)
{
    InterlockedExchange(&B->Lock, 0);
}

static
BOOLEAN
RtlpAddrEqual(const volatile VOID *Address, const VOID *Compare, SIZE_T Size)
{
    switch (Size)
    {
        case 1: return *(const volatile UCHAR *)Address == *(const UCHAR *)Compare;
        case 2: return *(const volatile USHORT *)Address == *(const USHORT *)Compare;
        case 4: return *(const volatile ULONG *)Address == *(const ULONG *)Compare;
        case 8: return *(const volatile ULONGLONG *)Address == *(const ULONGLONG *)Compare;
    }
    return FALSE;
}

static
VOID
RtlpWakeAddress(const volatile VOID *Address, BOOLEAN All)
{
    ADDR_BUCKET *B;
    PLIST_ENTRY Entry, Next;
    ADDR_WAITER *Woken[64];
    ULONG Count, i;

    RtlpAddrEnsureInit();
    B = RtlpAddrBucket(Address);

    do
    {
        Count = 0;
        RtlpAddrLock(B);
        for (Entry = B->Waiters.Flink; Entry != &B->Waiters && Count < RTL_NUMBER_OF(Woken); Entry = Next)
        {
            ADDR_WAITER *W = CONTAINING_RECORD(Entry, ADDR_WAITER, Link);
            Next = Entry->Flink;
            if (W->Address != Address)
                continue;
            RemoveEntryList(&W->Link);
            InitializeListHead(&W->Link); /* waiter's timeout path tests for this */
            Woken[Count++] = W;
            if (!All)
                break;
        }
        RtlpAddrUnlock(B);

        for (i = 0; i < Count; i++)
            NtReleaseKeyedEvent(NULL, Woken[i], FALSE, NULL);
    } while (All && Count == RTL_NUMBER_OF(Woken));
}

/* EXPORTED FUNCTIONS ********************************************************/

NTSTATUS
NTAPI
RtlWaitOnAddress(
    _In_ const volatile VOID *Address,
    _In_ PVOID CompareAddress,
    _In_ SIZE_T AddressSize,
    _In_opt_ PLARGE_INTEGER Timeout)
{
    ADDR_BUCKET *B;
    ADDR_WAITER Waiter;
    NTSTATUS Status;

    if (AddressSize != 1 && AddressSize != 2 && AddressSize != 4 && AddressSize != 8)
        return STATUS_INVALID_PARAMETER;

    RtlpAddrEnsureInit();
    B = RtlpAddrBucket(Address);
    Waiter.Address = Address;

    RtlpAddrLock(B);
    if (!RtlpAddrEqual(Address, CompareAddress, AddressSize))
    {
        RtlpAddrUnlock(B);
        return STATUS_SUCCESS;
    }
    InsertTailList(&B->Waiters, &Waiter.Link);
    RtlpAddrUnlock(B);

    Status = NtWaitForKeyedEvent(NULL, &Waiter, FALSE, Timeout);
    if (Status == STATUS_TIMEOUT)
    {
        RtlpAddrLock(B);
        if (!IsListEmpty(&Waiter.Link))
        {
            /* Still queued: nobody woke us. */
            RemoveEntryList(&Waiter.Link);
            RtlpAddrUnlock(B);
            return STATUS_TIMEOUT;
        }
        RtlpAddrUnlock(B);
        /* A waker removed us and is about to release the keyed event; consume it. */
        NtWaitForKeyedEvent(NULL, &Waiter, FALSE, NULL);
        return STATUS_SUCCESS;
    }

    return NT_SUCCESS(Status) ? STATUS_SUCCESS : Status;
}

VOID
NTAPI
RtlWakeAddressAll(
    _In_ const volatile VOID *Address)
{
    RtlpWakeAddress(Address, TRUE);
}

VOID
NTAPI
RtlWakeAddressSingle(
    _In_ const volatile VOID *Address)
{
    RtlpWakeAddress(Address, FALSE);
}

/* EOF */
