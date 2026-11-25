/*
 * PROJECT:     ReactOS LinuxKPI host
 * PURPOSE:     Forward events from Linux input drivers (usbhid, ...) to the ReactOS keyboard and mouse
 *              class drivers, so USB input devices drive the desktop.
 *
 * input_event() runs in whatever context the Linux driver completed its URB in (BH worker with interrupts
 * masked), so nt_input_event() only queues; a system thread translates and talks to kbdclass/mouclass.
 * Translation: Linux KEY_* -> PS/2 set 1 make codes (what the class drivers and win32k expect), REL_X/Y/WHEEL
 * and BTN_LEFT/RIGHT/MIDDLE -> MOUSE_INPUT_DATA, batched until the driver's SYN_REPORT.
 */
#include <ntddk.h>
#include <ntddkbd.h>
#include <ntddmou.h>
#include <reactos/drivers/lkpiinput.h>
#include "linuxkpi.h"

#define LKPI_INPUT_TAG 'pnIL'
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define REL_X 0x00
#define REL_Y 0x01
#define REL_HWHEEL 0x06
#define REL_WHEEL 0x08
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define RING 512
#define WHEEL_DELTA 120

typedef struct { USHORT Type, Code; LONG Value; } LKPI_INEV;

static LKPI_INEV Ring[RING];
static ULONG RingHead, RingTail;
static KSPIN_LOCK RingLock;
static KEVENT RingEvent;
static BOOLEAN InputStarted;
static PDEVICE_OBJECT KbdDevice, MouDevice;
static PFILE_OBJECT KbdFile, MouFile;

/* Linux key code -> set 1 make code. Returns 0 if unmapped; *Ext set for E0-prefixed keys. */
static USHORT LkpiKeyToScan(USHORT Key, BOOLEAN *Ext)
{
    *Ext = FALSE;
    if (Key >= 1 && Key <= 88) return Key;               /* ESC..F12: identical */
    switch (Key)
    {
        case 96:  *Ext = TRUE; return 0x1C;  /* KPENTER */
        case 97:  *Ext = TRUE; return 0x1D;  /* RIGHTCTRL */
        case 98:  *Ext = TRUE; return 0x35;  /* KPSLASH */
        case 99:  *Ext = TRUE; return 0x37;  /* SYSRQ / PrtSc */
        case 100: *Ext = TRUE; return 0x38;  /* RIGHTALT */
        case 102: *Ext = TRUE; return 0x47;  /* HOME */
        case 103: *Ext = TRUE; return 0x48;  /* UP */
        case 104: *Ext = TRUE; return 0x49;  /* PAGEUP */
        case 105: *Ext = TRUE; return 0x4B;  /* LEFT */
        case 106: *Ext = TRUE; return 0x4D;  /* RIGHT */
        case 107: *Ext = TRUE; return 0x4F;  /* END */
        case 108: *Ext = TRUE; return 0x50;  /* DOWN */
        case 109: *Ext = TRUE; return 0x51;  /* PAGEDOWN */
        case 110: *Ext = TRUE; return 0x52;  /* INSERT */
        case 111: *Ext = TRUE; return 0x53;  /* DELETE */
        case 125: *Ext = TRUE; return 0x5B;  /* LEFTMETA */
        case 126: *Ext = TRUE; return 0x5C;  /* RIGHTMETA */
        case 127: *Ext = TRUE; return 0x5D;  /* COMPOSE (menu) */
        case 86:  return 0x56;               /* 102ND */
        case 183: return 0x64; case 184: return 0x65; case 185: return 0x66; case 186: return 0x67;
        case 187: return 0x68; case 188: return 0x69; case 189: return 0x6A; case 190: return 0x6B;
        case 191: return 0x6C; case 192: return 0x6D; case 193: return 0x6E; /* F13..F23 */
    }
    return 0;
}

static NTSTATUS LkpiOpenClass(PCWSTR Name, PDEVICE_OBJECT *Dev, PFILE_OBJECT *File)
{
    UNICODE_STRING U;
    RtlInitUnicodeString(&U, Name);
    return IoGetDeviceObjectPointer(&U, FILE_READ_ATTRIBUTES, File, Dev);
}

static VOID LkpiInject(PDEVICE_OBJECT Dev, ULONG Code, PVOID Data, ULONG Len)
{
    KEVENT Done;
    IO_STATUS_BLOCK Iosb;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Done, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(Code, Dev, Data, Len, NULL, 0, FALSE, &Done, &Iosb);
    if (!Irp) return;
    Status = IoCallDriver(Dev, Irp);
    if (Status == STATUS_PENDING) KeWaitForSingleObject(&Done, Executive, KernelMode, FALSE, NULL);
}

static VOID LkpiSendKey(USHORT Key, LONG Value)
{
    KEYBOARD_INPUT_DATA D;
    BOOLEAN Ext;
    USHORT Scan = LkpiKeyToScan(Key, &Ext);

    if (!Scan || Value == 2) return;                     /* unmapped, or autorepeat: win32k repeats itself */
    if (!KbdDevice && !NT_SUCCESS(LkpiOpenClass(L"\\Device\\KeyboardClass0", &KbdDevice, &KbdFile))) return;
    RtlZeroMemory(&D, sizeof(D));
    D.MakeCode = Scan;
    D.Flags = (Value ? KEY_MAKE : KEY_BREAK) | (Ext ? KEY_E0 : 0);
    LkpiInject(KbdDevice, IOCTL_LKPI_KEYBOARD_INJECT, &D, sizeof(D));
}

static VOID LkpiSendMouse(LONG Dx, LONG Dy, LONG Wheel, USHORT Buttons)
{
    MOUSE_INPUT_DATA D;

    if (!Dx && !Dy && !Wheel && !Buttons) return;
    if (!MouDevice && !NT_SUCCESS(LkpiOpenClass(L"\\Device\\PointerClass0", &MouDevice, &MouFile))) return;
    RtlZeroMemory(&D, sizeof(D));
    D.Flags = MOUSE_MOVE_RELATIVE;
    D.LastX = Dx;
    D.LastY = Dy;
    D.ButtonFlags = Buttons;
    if (Wheel)
    {
        D.ButtonFlags |= MOUSE_WHEEL;
        D.ButtonData = (USHORT)(SHORT)(Wheel * WHEEL_DELTA);
    }
    LkpiInject(MouDevice, IOCTL_LKPI_MOUSE_INJECT, &D, sizeof(D));
}

static VOID NTAPI LkpiInputThread(PVOID Ctx)
{
    LONG Dx = 0, Dy = 0, Wheel = 0;
    USHORT Buttons = 0;

    for (;;)
    {
        LKPI_INEV E;
        KIRQL Old;
        BOOLEAN Have = FALSE;

        KeAcquireSpinLock(&RingLock, &Old);
        if (RingTail != RingHead)
        {
            E = Ring[RingTail];
            RingTail = (RingTail + 1) % RING;
            Have = TRUE;
        }
        KeReleaseSpinLock(&RingLock, Old);
        if (!Have)
        {
            KeWaitForSingleObject(&RingEvent, Executive, KernelMode, FALSE, NULL);
            continue;
        }

        if (E.Type == EV_KEY)
        {
            if (E.Code >= BTN_LEFT && E.Code <= BTN_MIDDLE)
            {
                static const USHORT Down[] = { MOUSE_LEFT_BUTTON_DOWN, MOUSE_RIGHT_BUTTON_DOWN, MOUSE_MIDDLE_BUTTON_DOWN };
                static const USHORT Up[]   = { MOUSE_LEFT_BUTTON_UP,   MOUSE_RIGHT_BUTTON_UP,   MOUSE_MIDDLE_BUTTON_UP };
                Buttons |= E.Value ? Down[E.Code - BTN_LEFT] : Up[E.Code - BTN_LEFT];
            }
            else if (E.Code < 0x100)
            {
                LkpiSendKey(E.Code, E.Value);
            }
        }
        else if (E.Type == EV_REL)
        {
            if (E.Code == REL_X) Dx += E.Value;
            else if (E.Code == REL_Y) Dy += E.Value;
            else if (E.Code == REL_WHEEL) Wheel += E.Value;
        }
        else if (E.Type == EV_SYN)
        {
            LkpiSendMouse(Dx, Dy, Wheel, Buttons);
            Dx = Dy = Wheel = 0;
            Buttons = 0;
        }
    }
}

/* Called from Linux input_event() (any context, interrupts possibly masked). */
VOID LX_ABI nt_input_event(unsigned int Type, unsigned int Code, int Value)
{
    KIRQL Old;
    ULONG Next;

    if (!InputStarted || (Type != EV_KEY && Type != EV_REL && Type != EV_SYN)) return;
    KeAcquireSpinLock(&RingLock, &Old);
    Next = (RingHead + 1) % RING;
    if (Next != RingTail)
    {
        Ring[RingHead].Type = (USHORT)Type;
        Ring[RingHead].Code = (USHORT)Code;
        Ring[RingHead].Value = Value;
        RingHead = Next;
    }
    KeReleaseSpinLock(&RingLock, Old);
    KeSetEvent(&RingEvent, IO_NO_INCREMENT, FALSE);
}

VOID LkpiInitInput(VOID)
{
    HANDLE Th;

    KeInitializeSpinLock(&RingLock);
    KeInitializeEvent(&RingEvent, SynchronizationEvent, FALSE);
    if (NT_SUCCESS(PsCreateSystemThread(&Th, THREAD_ALL_ACCESS, NULL, NULL, NULL, LkpiInputThread, NULL)))
    {
        ZwClose(Th);
        InputStarted = TRUE;
    }
}
