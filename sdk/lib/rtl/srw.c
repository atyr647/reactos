/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS system libraries
 * PURPOSE:           Slim Reader/Writer (SRW) Routines
 *
 * NOTES:             The lock word is treated as three fields, following the keyed-event based
 *                    design used by Wine (dlls/ntdll/sync.c, LGPL 2.1+):
 *                      bits  0..15  number of threads in (or queued for) shared mode
 *                      bits 16..30  number of threads in (or queued for) exclusive mode
 *                      bit  31      "in exclusive": shared acquirers must queue behind it
 *                    Waiters block on the global keyed event instead of spinning, so a
 *                    waiting thread never burns the CPU or starves the lock owner.
 *                    Applications treat RTL_SRWLOCK as opaque data.
 */

/* INCLUDES *****************************************************************/

#include <rtl_vista.h>

#define NDEBUG
#include <debug.h>

/* DEFINES ******************************************************************/

#define SRWLOCK_MASK_IN_EXCLUSIVE     0x80000000
#define SRWLOCK_MASK_EXCLUSIVE_QUEUE  0x7fff0000
#define SRWLOCK_MASK_SHARED_QUEUE     0x0000ffff
#define SRWLOCK_RES_EXCLUSIVE         0x00010000
#define SRWLOCK_RES_SHARED            0x00000001

/* FUNCTIONS *****************************************************************/

/* The lock state lives in the low 32 bits of the pointer-sized lock (little endian). */
#define SRW_STATE(lock)   ((volatile LONG *)&(lock)->Ptr)

/* Keyed event keys must have bit 0 clear. The lock is at least 4-byte aligned, so
   Ptr+0 and Ptr+2 are never keys of another object. */
static __inline PVOID srwlock_key_exclusive(PRTL_SRWLOCK lock)
{
    return (PVOID)&lock->Ptr;
}

static __inline PVOID srwlock_key_shared(PRTL_SRWLOCK lock)
{
    return (PVOID)((ULONG_PTR)&lock->Ptr + 2);
}

static __inline ULONG
srwlock_lock_exclusive(volatile LONG *dest, LONG incr)
{
    /* Atomically add incr. If the shared queue is empty and threads are waiting for
       exclusive access, set the IN_EXCLUSIVE mark so that new shared acquirers queue. */
    ULONG val, tmp;

    for (val = (ULONG)*dest;; val = tmp)
    {
        tmp = val + incr;
        if ((tmp & SRWLOCK_MASK_EXCLUSIVE_QUEUE) && !(tmp & SRWLOCK_MASK_SHARED_QUEUE))
            tmp |= SRWLOCK_MASK_IN_EXCLUSIVE;
        if ((tmp = (ULONG)InterlockedCompareExchange(dest, (LONG)tmp, (LONG)val)) == val)
            break;
    }
    return val;
}

static __inline ULONG
srwlock_unlock_exclusive(volatile LONG *dest, LONG incr)
{
    /* Atomically add incr. If nobody waits for exclusive access any more, drop the
       IN_EXCLUSIVE mark (only the shared queue counter remains). */
    ULONG val, tmp;

    for (val = (ULONG)*dest;; val = tmp)
    {
        tmp = val + incr;
        if (!(tmp & SRWLOCK_MASK_EXCLUSIVE_QUEUE))
            tmp &= SRWLOCK_MASK_SHARED_QUEUE;
        if ((tmp = (ULONG)InterlockedCompareExchange(dest, (LONG)tmp, (LONG)val)) == val)
            break;
    }
    return val;
}

static __inline VOID
srwlock_leave_exclusive(PRTL_SRWLOCK lock, ULONG val)
{
    /* Other exclusive waiters are served first, then all shared waiters. */
    if (val & SRWLOCK_MASK_EXCLUSIVE_QUEUE)
    {
        NtReleaseKeyedEvent(NULL, srwlock_key_exclusive(lock), FALSE, NULL);
    }
    else
    {
        val &= SRWLOCK_MASK_SHARED_QUEUE; /* remove IN_EXCLUSIVE */
        while (val--)
            NtReleaseKeyedEvent(NULL, srwlock_key_shared(lock), FALSE, NULL);
    }
}

static __inline VOID
srwlock_leave_shared(PRTL_SRWLOCK lock, ULONG val)
{
    /* Wake one exclusive waiter as soon as the last shared owner has left. */
    if ((val & SRWLOCK_MASK_EXCLUSIVE_QUEUE) && !(val & SRWLOCK_MASK_SHARED_QUEUE))
        NtReleaseKeyedEvent(NULL, srwlock_key_exclusive(lock), FALSE, NULL);
}

VOID
NTAPI
RtlInitializeSRWLock(OUT PRTL_SRWLOCK SRWLock)
{
    SRWLock->Ptr = NULL;
}

VOID
NTAPI
RtlAcquireSRWLockExclusive(IN OUT PRTL_SRWLOCK SRWLock)
{
    /* If the lock is already held, queue up and wait for the keyed event. */
    if (srwlock_lock_exclusive(SRW_STATE(SRWLock), SRWLOCK_RES_EXCLUSIVE))
        NtWaitForKeyedEvent(NULL, srwlock_key_exclusive(SRWLock), FALSE, NULL);
}

VOID
NTAPI
RtlAcquireSRWLockShared(IN OUT PRTL_SRWLOCK SRWLock)
{
    ULONG val, tmp;

    /* If shared acquirers can't join the shared queue right now, request exclusive
       access instead and convert it later. */
    for (val = (ULONG)*SRW_STATE(SRWLock);; val = tmp)
    {
        if ((val & SRWLOCK_MASK_EXCLUSIVE_QUEUE) && !(val & SRWLOCK_MASK_IN_EXCLUSIVE))
            tmp = val + SRWLOCK_RES_EXCLUSIVE;
        else
            tmp = val + SRWLOCK_RES_SHARED;
        if ((tmp = (ULONG)InterlockedCompareExchange(SRW_STATE(SRWLock), (LONG)tmp, (LONG)val)) == val)
            break;
    }

    /* Drop the exclusive request again and requeue for shared access. */
    if ((val & SRWLOCK_MASK_EXCLUSIVE_QUEUE) && !(val & SRWLOCK_MASK_IN_EXCLUSIVE))
    {
        NtWaitForKeyedEvent(NULL, srwlock_key_exclusive(SRWLock), FALSE, NULL);
        val = srwlock_unlock_exclusive(SRW_STATE(SRWLock), SRWLOCK_RES_SHARED - SRWLOCK_RES_EXCLUSIVE);
        srwlock_leave_exclusive(SRWLock, val);
    }

    if (val & SRWLOCK_MASK_EXCLUSIVE_QUEUE)
        NtWaitForKeyedEvent(NULL, srwlock_key_shared(SRWLock), FALSE, NULL);
}

VOID
NTAPI
RtlReleaseSRWLockExclusive(IN OUT PRTL_SRWLOCK SRWLock)
{
    ULONG val = srwlock_unlock_exclusive(SRW_STATE(SRWLock), -SRWLOCK_RES_EXCLUSIVE) - SRWLOCK_RES_EXCLUSIVE;
    srwlock_leave_exclusive(SRWLock, val);
}

VOID
NTAPI
RtlReleaseSRWLockShared(IN OUT PRTL_SRWLOCK SRWLock)
{
    ULONG val = srwlock_lock_exclusive(SRW_STATE(SRWLock), -SRWLOCK_RES_SHARED) - SRWLOCK_RES_SHARED;
    srwlock_leave_shared(SRWLock, val);
}

BOOLEAN
NTAPI
RtlTryAcquireSRWLockShared(PRTL_SRWLOCK SRWLock)
{
    ULONG val, tmp;

    for (val = (ULONG)*SRW_STATE(SRWLock);; val = tmp)
    {
        if (val & SRWLOCK_MASK_EXCLUSIVE_QUEUE)
            return FALSE;
        if ((tmp = (ULONG)InterlockedCompareExchange(SRW_STATE(SRWLock), (LONG)(val + SRWLOCK_RES_SHARED), (LONG)val)) == val)
            break;
    }
    return TRUE;
}

BOOLEAN
NTAPI
RtlTryAcquireSRWLockExclusive(PRTL_SRWLOCK SRWLock)
{
    return InterlockedCompareExchange(SRW_STATE(SRWLock),
                                      (LONG)(SRWLOCK_MASK_IN_EXCLUSIVE | SRWLOCK_RES_EXCLUSIVE),
                                      0) == 0;
}
