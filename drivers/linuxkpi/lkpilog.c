/*
 * PROJECT:     ReactOS Linux kernel module host (LinuxKPI)
 * PURPOSE:     Bring-up log on a USB stick: everything the Linux modules print (printk) and the host's own messages
 *              go to a RAM ring; a worker thread appends it to X:\LKPILOG.TXT on the first volume where that file
 *              already exists. Creating the empty file is the opt-in: no volume is written to otherwise.
 *
 * Meant for machines without a serial port, where a driver that fails (or takes the display with it) leaves nothing
 * on screen. Data is written every 200 ms with write-through; what the last interval held before a crash is lost.
 */

#include "linuxkpi.h"
#include <ntstrsafe.h>

#undef DbgPrint     /* this file calls the real one */

#define LKPI_LOG_SIZE (2 * 1024 * 1024)        /* power of two */
#define LKPI_LOG_TAG 'goLL'

static PCHAR LogBuf;
static volatile LONG64 LogHead;                 /* bytes ever written to the ring */
static LONG64 LogFlushed;                       /* bytes ever written to the file (worker only) */
static volatile LONG LogLock;
static HANDLE LogFile;
static KEVENT LogFound;
static KMUTEX LogFlushMutex;
static WCHAR LogPath[] = L"\\DosDevices\\C:\\LKPILOG.TXT";

/* Interrupts off while the ring is updated: printk runs in ISRs too */
static VOID LogAppend(PCSTR S, SIZE_T Len)
{
    ULONG_PTR Flags;
    SIZE_T i;

    if (!LogBuf || !Len)
        return;
    Flags = __readeflags();
    _disable();
    while (InterlockedCompareExchange(&LogLock, 1, 0))
        YieldProcessor();
    for (i = 0; i < Len; i++)
        LogBuf[(LogHead + i) & (LKPI_LOG_SIZE - 1)] = S[i];
    LogHead += Len;
    InterlockedExchange(&LogLock, 0);
    if (Flags & 0x200)      /* IF */
        _enable();
}

/* DbgPrint in linuxkpi.sys is redirected here (linuxkpi.h): host messages and, through nt_dbgprint(), every Linux
 * printk. Any IRQL. The text still reaches the kernel debugger. */
ULONG __cdecl LkpiDbgPrint(PCSTR Format, ...)
{
    CHAR Line[1024];
    va_list Args;
    size_t Len = 0;

    va_start(Args, Format);
    RtlStringCbVPrintfA(Line, sizeof(Line), Format, Args);
    va_end(Args);
    RtlStringCbLengthA(Line, sizeof(Line), &Len);
    LogAppend(Line, Len);
    return DbgPrint("%.511s", Line);      /* the debugger takes 512 characters per call */
}

static BOOLEAN LogTryOpen(VOID)
{
    UNICODE_STRING Us;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Iosb;
    WCHAR L;

    for (L = L'C'; L <= L'Z'; L++)
    {
        LogPath[12] = L;
        RtlInitUnicodeString(&Us, LogPath);
        InitializeObjectAttributes(&Oa, &Us, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        /* FILE_OPEN: only an existing file counts as the user's consent */
        if (NT_SUCCESS(ZwCreateFile(&LogFile, FILE_WRITE_DATA | SYNCHRONIZE, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                    FILE_SHARE_READ, FILE_OPEN,
                                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0)))
        {
            LkpiDbgPrint("LKPI: logging to %c:\\LKPILOG.TXT\n", (CHAR)L);
            return TRUE;
        }
    }
    LogFile = NULL;
    return FALSE;
}

static VOID LogWrite(PVOID Data, ULONG Len)
{
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER EndOfFile;

    EndOfFile.HighPart = -1;
    EndOfFile.LowPart = FILE_WRITE_TO_END_OF_FILE;
    if (!NT_SUCCESS(ZwWriteFile(LogFile, NULL, NULL, NULL, &Iosb, Data, Len, &EndOfFile, NULL)))
    {
        ZwClose(LogFile);           /* stick removed: look for it again */
        LogFile = NULL;
    }
}

/* PASSIVE_LEVEL: copy what is new in the ring to the file (the worker does it every 200 ms; callers about to do
 * something risky can do it themselves) */
VOID LkpiLogFlush(VOID)
{
    static CHAR Chunk[16384];
    LONG64 Head;
    BOOLEAN Wrote = FALSE;

    if (!LogBuf || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    KeWaitForSingleObject(&LogFlushMutex, Executive, KernelMode, FALSE, NULL);
    Head = LogHead;
    if (!LogFile)
    {
        KeReleaseMutex(&LogFlushMutex, FALSE);
        return;
    }
    if (Head - LogFlushed > LKPI_LOG_SIZE)
    {
        static const CHAR Lost[] = "\n[LKPI: log ring overflowed, older lines lost]\n";
        LogWrite((PVOID)Lost, sizeof(Lost) - 1);
        LogFlushed = Head - LKPI_LOG_SIZE;
    }
    while (LogFile && LogFlushed < Head)
    {
        ULONG n = (ULONG)min((LONG64)sizeof(Chunk), Head - LogFlushed), i;

        for (i = 0; i < n; i++)
            Chunk[i] = LogBuf[(LogFlushed + i) & (LKPI_LOG_SIZE - 1)];
        LogWrite(Chunk, n);
        LogFlushed += n;
        Wrote = TRUE;
    }
    /* write-through covers the data; the FAT and the directory entry (file size) need a flush to survive a crash */
    if (Wrote && LogFile)
    {
        IO_STATUS_BLOCK Iosb;
        ZwFlushBuffersFile(LogFile, &Iosb);
    }
    KeReleaseMutex(&LogFlushMutex, FALSE);
}

static VOID NTAPI LogThread(PVOID Context)
{
    LARGE_INTEGER Delay;
    ULONG Tick;

    Delay.QuadPart = -10 * 1000 * 200;          /* 200 ms */
    for (Tick = 0;; Tick++)
    {
        /* look for the file once a second until found (volumes appear late: USB sticks, PnP) */
        if (!LogFile && Tick % 5 == 0)
        {
            KeWaitForSingleObject(&LogFlushMutex, Executive, KernelMode, FALSE, NULL);
            if (LogTryOpen())
            {
                LogWrite("\r\n===== LinuxKPI log =====\r\n", 27);
                KeSetEvent(&LogFound, IO_NO_INCREMENT, FALSE);
            }
            KeReleaseMutex(&LogFlushMutex, FALSE);
        }
        LkpiLogFlush();
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
}

VOID LkpiLogInit(VOID)
{
    HANDLE Th;

    KeInitializeEvent(&LogFound, NotificationEvent, FALSE);
    KeInitializeMutex(&LogFlushMutex, 0);
    LogBuf = ExAllocatePoolWithTag(NonPagedPool, LKPI_LOG_SIZE, LKPI_LOG_TAG);
    if (!LogBuf)
        return;
    if (NT_SUCCESS(PsCreateSystemThread(&Th, THREAD_ALL_ACCESS, NULL, NULL, NULL, LogThread, NULL)))
        ZwClose(Th);
}

/* Wait (up to Ms) until a log file is found. TRUE if logging to a file. */
BOOLEAN LkpiLogWait(ULONG Ms)
{
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = -10 * 1000 * (LONGLONG)Ms;
    return KeWaitForSingleObject(&LogFound, Executive, KernelMode, FALSE, &Timeout) == STATUS_SUCCESS;
}
