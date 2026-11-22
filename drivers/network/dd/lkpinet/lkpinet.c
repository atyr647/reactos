/*
 * PROJECT:     ReactOS LinuxKPI network miniport
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     NDIS 5 miniport for Ethernet adapters driven by Linux drivers inside linuxkpi.sys
 *              (r8169, igc, e1000e, igb, ...). The hardware belongs to the Linux driver; this miniport moves whole
 *              frames between NDIS and it through LKPI_NET_INTERFACE (drivers/linuxkpi/lkpi_net.h).
 */

#include <ndis.h>
#include <initguid.h>
#include "lkpi_net.h"

#define LKNET_TAG 'tenL'
#define LKNET_MAX_FRAME 1518            /* 1514 + an 802.1Q tag */
#define LKNET_MAX_MCAST 32
#define LKNET_DRIVER_VERSION 0x00010000

typedef struct _LKNET_ADAPTER
{
    NDIS_HANDLE Handle;
    PDEVICE_OBJECT NextDevice;
    LKPI_NET_INTERFACE If;
    BOOLEAN Initialized;
    BOOLEAN Opened;
    volatile LONG TxBlocked;
    ULONG PacketFilter;
    ULONG MediaState;
    ULONG Mbps;
    ULONG Mtu;
    UCHAR PermanentMac[6];
    UCHAR CurrentMac[6];
    UCHAR Mcast[LKNET_MAX_MCAST][6];
    ULONG McastCount;
    CHAR Driver[32];
    UCHAR TxFrame[LKNET_MAX_FRAME];     /* serialized miniport: one send at a time */
} LKNET_ADAPTER, *PLKNET_ADAPTER;

static NDIS_OID SupportedOids[] =
{
    OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED, OID_GEN_MEDIA_IN_USE,
    OID_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_FRAME_SIZE, OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE, OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION, OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD, OID_GEN_DRIVER_VERSION, OID_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAXIMUM_SEND_PACKETS, OID_GEN_PHYSICAL_MEDIUM,
    OID_GEN_XMIT_OK, OID_GEN_RCV_OK, OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR, OID_GEN_RCV_NO_BUFFER,
    OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS, OID_802_3_MULTICAST_LIST, OID_802_3_MAXIMUM_LIST_SIZE,
    OID_802_3_RCV_ERROR_ALIGNMENT, OID_802_3_XMIT_ONE_COLLISION, OID_802_3_XMIT_MORE_COLLISIONS,
};

/* ---- Linux -> NDIS -------------------------------------------------------------------------------------------- */
static BOOLEAN
FrameWanted(PLKNET_ADAPTER A, const UCHAR *Dest)
{
    ULONG F = A->PacketFilter, i;

    if (!F)
        return FALSE;
    if (F & NDIS_PACKET_TYPE_PROMISCUOUS)
        return TRUE;
    if (!(Dest[0] & 1))
        return (F & NDIS_PACKET_TYPE_DIRECTED) && RtlEqualMemory(Dest, A->CurrentMac, 6);
    if (Dest[0] == 0xFF && Dest[1] == 0xFF && Dest[2] == 0xFF && Dest[3] == 0xFF && Dest[4] == 0xFF && Dest[5] == 0xFF)
        return (F & NDIS_PACKET_TYPE_BROADCAST) != 0;
    if (F & NDIS_PACKET_TYPE_ALL_MULTICAST)
        return TRUE;
    if (F & NDIS_PACKET_TYPE_MULTICAST)
        for (i = 0; i < A->McastCount; i++)
            if (RtlEqualMemory(Dest, A->Mcast[i], 6))
                return TRUE;
    return FALSE;
}

static VOID NTAPI
OnRx(PVOID Ctx, const VOID *Frame, ULONG Len)
{
    PLKNET_ADAPTER A = Ctx;
    PUCHAR F = (PUCHAR)Frame;

    if (!A->Initialized || Len < 14 || !FrameWanted(A, F))
        return;
    NdisMEthIndicateReceive(A->Handle, NULL, (PCHAR)F, 14, F + 14, Len - 14, Len - 14);
    NdisMEthIndicateReceiveComplete(A->Handle);
}

static VOID NTAPI
OnLink(PVOID Ctx, BOOLEAN Up, ULONG Mbps)
{
    PLKNET_ADAPTER A = Ctx;
    ULONG State = Up ? NdisMediaStateConnected : NdisMediaStateDisconnected;
    KIRQL Old;

    if (Mbps)
        A->Mbps = Mbps;
    if (State == A->MediaState)
        return;
    A->MediaState = State;
    DbgPrint("LKNET: %s: link %s (%lu Mbps)\n", A->Driver, Up ? "up" : "down", Mbps);
    if (!A->Initialized)
        return;
    KeRaiseIrql(DISPATCH_LEVEL, &Old);
    NdisMIndicateStatus(A->Handle, Up ? NDIS_STATUS_MEDIA_CONNECT : NDIS_STATUS_MEDIA_DISCONNECT, NULL, 0);
    NdisMIndicateStatusComplete(A->Handle);
    KeLowerIrql(Old);
}

static VOID NTAPI
OnTxWake(PVOID Ctx)
{
    PLKNET_ADAPTER A = Ctx;

    if (A->Initialized && InterlockedExchange(&A->TxBlocked, 0))
        NdisMSendResourcesAvailable(A->Handle);
}

/* ---- miniport handlers ---------------------------------------------------------------------------------------- */
static NTSTATUS
QueryLinuxInterface(PLKNET_ADAPTER A)
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP Irp;
    PIO_STACK_LOCATION Sp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, A->NextDevice, NULL, 0, NULL, &Event, &Iosb);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Sp = IoGetNextIrpStackLocation(Irp);
    Sp->MinorFunction = IRP_MN_QUERY_INTERFACE;
    Sp->Parameters.QueryInterface.InterfaceType = &GUID_LKPI_NET_INTERFACE;
    Sp->Parameters.QueryInterface.Size = sizeof(A->If);
    Sp->Parameters.QueryInterface.Version = LKPI_NET_INTERFACE_VERSION;
    Sp->Parameters.QueryInterface.Interface = (PINTERFACE)&A->If;
    Sp->Parameters.QueryInterface.InterfaceSpecificData = NULL;
    Status = IoCallDriver(A->NextDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    return Status;
}

static NDIS_STATUS NTAPI
MiniportInitialize(PNDIS_STATUS OpenErrorStatus, PUINT SelectedMediumIndex, PNDIS_MEDIUM MediumArray,
                   UINT MediumArraySize, NDIS_HANDLE MiniportAdapterHandle, NDIS_HANDLE WrapperConfigurationContext)
{
    PLKNET_ADAPTER A;
    LKPI_NET_CALLBACKS Cb;
    LKPI_NET_INFO Info;
    NTSTATUS Status;
    UINT i;

    for (i = 0; i < MediumArraySize && MediumArray[i] != NdisMedium802_3; i++);
    if (i == MediumArraySize)
        return NDIS_STATUS_UNSUPPORTED_MEDIA;
    *SelectedMediumIndex = i;

    if (NdisAllocateMemoryWithTag((PVOID *)&A, sizeof(*A), LKNET_TAG) != NDIS_STATUS_SUCCESS)
        return NDIS_STATUS_RESOURCES;
    NdisZeroMemory(A, sizeof(*A));
    A->Handle = MiniportAdapterHandle;
    A->MediaState = NdisMediaStateDisconnected;
    NdisMGetDeviceProperty(MiniportAdapterHandle, NULL, NULL, &A->NextDevice, NULL, NULL);

    Status = A->NextDevice ? QueryLinuxInterface(A) : STATUS_NO_SUCH_DEVICE;
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("LKNET: no LinuxKPI network interface (%08lx)\n", Status);
        NdisFreeMemory(A, sizeof(*A), 0);
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }
    A->If.GetInfo(A->If.Header.Context, &Info);
    NdisMoveMemory(A->PermanentMac, Info.Mac, 6);
    NdisMoveMemory(A->CurrentMac, Info.Mac, 6);
    NdisMoveMemory(A->Driver, Info.Driver, sizeof(A->Driver));
    A->Mtu = Info.Mtu ? Info.Mtu : 1500;
    if (A->Mtu > 1500)
        A->Mtu = 1500;

    NdisMSetAttributesEx(MiniportAdapterHandle, A, 0, NDIS_ATTRIBUTE_NO_HALT_ON_SUSPEND, NdisInterfaceInternal);

    Cb.Rx = OnRx;
    Cb.Link = OnLink;
    Cb.TxWake = OnTxWake;
    Cb.Context = A;
    Status = A->If.Open(A->If.Header.Context, &Cb);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("LKNET: %s: open failed (%08lx)\n", A->Driver, Status);
        A->If.Header.InterfaceDereference(A->If.Header.Context);
        NdisFreeMemory(A, sizeof(*A), 0);
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }
    A->Opened = TRUE;
    A->If.GetInfo(A->If.Header.Context, &Info);
    A->MediaState = Info.Link ? NdisMediaStateConnected : NdisMediaStateDisconnected;
    if (Info.Mbps)
        A->Mbps = Info.Mbps;
    A->Initialized = TRUE;
    DbgPrint("LKNET: %s (%s) %02x:%02x:%02x:%02x:%02x:%02x ready\n", Info.IfName, A->Driver, A->PermanentMac[0],
             A->PermanentMac[1], A->PermanentMac[2], A->PermanentMac[3], A->PermanentMac[4], A->PermanentMac[5]);
    *OpenErrorStatus = NDIS_STATUS_SUCCESS;
    return NDIS_STATUS_SUCCESS;
}

static VOID NTAPI
MiniportHalt(NDIS_HANDLE Context)
{
    PLKNET_ADAPTER A = Context;

    A->Initialized = FALSE;
    if (A->Opened)
        A->If.Close(A->If.Header.Context);
    A->If.Header.InterfaceDereference(A->If.Header.Context);
    NdisFreeMemory(A, sizeof(*A), 0);
}

static NDIS_STATUS NTAPI
MiniportReset(PBOOLEAN AddressingReset, NDIS_HANDLE Context)
{
    *AddressingReset = FALSE;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS NTAPI
MiniportSend(NDIS_HANDLE Context, PNDIS_PACKET Packet, UINT Flags)
{
    PLKNET_ADAPTER A = Context;
    PNDIS_BUFFER Buf;
    UINT Total, Len = 0;
    PVOID Va;
    UINT BufLen;
    LONG R;

    NdisQueryPacket(Packet, NULL, NULL, &Buf, &Total);
    if (Total < 14 || Total > 14 + A->Mtu + 4)
        return NDIS_STATUS_INVALID_PACKET;
    while (Buf)
    {
        NdisQueryBufferSafe(Buf, &Va, &BufLen, NormalPagePriority);
        if (!Va)
            return NDIS_STATUS_RESOURCES;
        if (BufLen > Total - Len)
            BufLen = Total - Len;
        NdisMoveMemory(A->TxFrame + Len, Va, BufLen);
        Len += BufLen;
        NdisGetNextBuffer(Buf, &Buf);
    }
    InterlockedExchange(&A->TxBlocked, 1);  /* set first: a TxWake racing with the call below is not lost */
    R = A->If.Xmit(A->If.Header.Context, A->TxFrame, Len);
    if (R == 1)
        return NDIS_STATUS_RESOURCES;       /* NDIS queues it until NdisMSendResourcesAvailable */
    InterlockedExchange(&A->TxBlocked, 0);
    /* the frame was copied into a Linux skb: the packet is done either way */
    return R == 0 ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE;
}

static NDIS_STATUS NTAPI
MiniportQueryInformation(NDIS_HANDLE Context, NDIS_OID Oid, PVOID InfoBuf, ULONG InfoLen, PULONG Written, PULONG Needed)
{
    PLKNET_ADAPTER A = Context;
    union { ULONG Ulong; USHORT Ushort; ULONG64 Ulong64; NDIS_MEDIUM Medium; } G;
    PVOID Src = &G;
    ULONG Len = sizeof(ULONG);
    ULONG64 Stats[6];

    switch (Oid)
    {
        case OID_GEN_SUPPORTED_LIST: Src = SupportedOids; Len = sizeof(SupportedOids); break;
        case OID_GEN_HARDWARE_STATUS: G.Ulong = NdisHardwareStatusReady; break;
        case OID_GEN_MEDIA_SUPPORTED: case OID_GEN_MEDIA_IN_USE: G.Medium = NdisMedium802_3; Len = sizeof(NDIS_MEDIUM); break;
        case OID_GEN_PHYSICAL_MEDIUM: G.Ulong = NdisPhysicalMedium802_3; break;
        case OID_GEN_MAXIMUM_LOOKAHEAD: case OID_GEN_CURRENT_LOOKAHEAD: case OID_GEN_MAXIMUM_FRAME_SIZE:
            G.Ulong = A->Mtu; break;
        case OID_GEN_MAXIMUM_TOTAL_SIZE: case OID_GEN_TRANSMIT_BLOCK_SIZE: case OID_GEN_RECEIVE_BLOCK_SIZE:
            G.Ulong = A->Mtu + 14; break;
        case OID_GEN_TRANSMIT_BUFFER_SPACE: case OID_GEN_RECEIVE_BUFFER_SPACE: G.Ulong = 64 * (A->Mtu + 14); break;
        case OID_GEN_LINK_SPEED: G.Ulong = (A->Mbps ? A->Mbps : 1000) * 10000; break;   /* units of 100 bps */
        case OID_GEN_VENDOR_ID: G.Ulong = (A->PermanentMac[0] << 16) | (A->PermanentMac[1] << 8) | A->PermanentMac[2]; break;
        case OID_GEN_VENDOR_DESCRIPTION: Src = A->Driver; Len = (ULONG)strlen(A->Driver) + 1; break;
        case OID_GEN_VENDOR_DRIVER_VERSION: G.Ulong = LKNET_DRIVER_VERSION; break;
        case OID_GEN_DRIVER_VERSION: G.Ushort = 0x0500; Len = sizeof(USHORT); break;
        case OID_GEN_MAXIMUM_SEND_PACKETS: G.Ulong = 1; break;
        case OID_GEN_MAC_OPTIONS:
            G.Ulong = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                      NDIS_MAC_OPTION_NO_LOOPBACK;
            break;
        case OID_GEN_MEDIA_CONNECT_STATUS: G.Ulong = A->MediaState; break;
        case OID_GEN_CURRENT_PACKET_FILTER: G.Ulong = A->PacketFilter; break;
        case OID_802_3_PERMANENT_ADDRESS: Src = A->PermanentMac; Len = 6; break;
        case OID_802_3_CURRENT_ADDRESS: Src = A->CurrentMac; Len = 6; break;
        case OID_802_3_MULTICAST_LIST: Src = A->Mcast; Len = A->McastCount * 6; break;
        case OID_802_3_MAXIMUM_LIST_SIZE: G.Ulong = LKNET_MAX_MCAST; break;
        case OID_802_3_RCV_ERROR_ALIGNMENT: case OID_802_3_XMIT_ONE_COLLISION: case OID_802_3_XMIT_MORE_COLLISIONS:
            G.Ulong = 0; break;
        case OID_GEN_XMIT_OK: case OID_GEN_RCV_OK: case OID_GEN_XMIT_ERROR: case OID_GEN_RCV_ERROR:
        case OID_GEN_RCV_NO_BUFFER:
            A->If.GetStats(A->If.Header.Context, Stats);
            G.Ulong64 = Oid == OID_GEN_XMIT_OK ? Stats[1] : Oid == OID_GEN_RCV_OK ? Stats[0] :
                        Oid == OID_GEN_XMIT_ERROR ? Stats[5] : Oid == OID_GEN_RCV_ERROR ? Stats[4] : 0;
            /* 32- or 64-bit, whichever the caller has room for */
            Len = InfoLen >= sizeof(ULONG64) ? sizeof(ULONG64) : sizeof(ULONG);
            break;
        default:
            *Written = 0;
            *Needed = 0;
            return NDIS_STATUS_NOT_SUPPORTED;
    }
    if (Len > InfoLen)
    {
        *Written = 0;
        *Needed = Len;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }
    NdisMoveMemory(InfoBuf, Src, Len);
    *Written = Len;
    *Needed = Len;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS NTAPI
MiniportSetInformation(NDIS_HANDLE Context, NDIS_OID Oid, PVOID InfoBuf, ULONG InfoLen, PULONG Read, PULONG Needed)
{
    PLKNET_ADAPTER A = Context;
    ULONG V;

    *Read = 0;
    *Needed = 0;
    switch (Oid)
    {
        case OID_GEN_CURRENT_PACKET_FILTER:
            if (InfoLen < sizeof(ULONG)) { *Needed = sizeof(ULONG); return NDIS_STATUS_INVALID_LENGTH; }
            NdisMoveMemory(&V, InfoBuf, sizeof(ULONG));
            if (V & ~(NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | NDIS_PACKET_TYPE_ALL_MULTICAST |
                      NDIS_PACKET_TYPE_BROADCAST | NDIS_PACKET_TYPE_PROMISCUOUS))
                return NDIS_STATUS_NOT_SUPPORTED;
            A->PacketFilter = V;
            /* the hardware always accepts all multicast (filtered here); promiscuous needs the driver */
            A->If.SetRxFlags(A->If.Header.Context, LKPI_NET_RX_ALL_MULTICAST |
                             ((V & NDIS_PACKET_TYPE_PROMISCUOUS) ? LKPI_NET_RX_PROMISCUOUS : 0));
            *Read = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;
        case OID_GEN_CURRENT_LOOKAHEAD:
            if (InfoLen < sizeof(ULONG)) { *Needed = sizeof(ULONG); return NDIS_STATUS_INVALID_LENGTH; }
            *Read = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;     /* whole frames are always indicated */
        case OID_802_3_MULTICAST_LIST:
            if (InfoLen % 6) return NDIS_STATUS_INVALID_LENGTH;
            if (InfoLen / 6 > LKNET_MAX_MCAST) return NDIS_STATUS_MULTICAST_FULL;
            NdisMoveMemory(A->Mcast, InfoBuf, InfoLen);
            A->McastCount = InfoLen / 6;
            *Read = InfoLen;
            return NDIS_STATUS_SUCCESS;
        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

/* ---- driver entry --------------------------------------------------------------------------------------------- */
NTSTATUS NTAPI
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NDIS_HANDLE Wrapper;
    NDIS_MINIPORT_CHARACTERISTICS C;
    NDIS_STATUS Status;

    NdisMInitializeWrapper(&Wrapper, DriverObject, RegistryPath, NULL);
    if (!Wrapper)
        return NDIS_STATUS_FAILURE;
    NdisZeroMemory(&C, sizeof(C));
    C.MajorNdisVersion = NDIS_MINIPORT_MAJOR_VERSION;
    C.MinorNdisVersion = NDIS_MINIPORT_MINOR_VERSION;
    C.InitializeHandler = MiniportInitialize;
    C.HaltHandler = MiniportHalt;
    C.ResetHandler = MiniportReset;
    C.SendHandler = MiniportSend;
    C.QueryInformationHandler = MiniportQueryInformation;
    C.SetInformationHandler = MiniportSetInformation;
    Status = NdisMRegisterMiniport(Wrapper, &C, sizeof(C));
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NdisTerminateWrapper(Wrapper, NULL);
        return Status;
    }
    return STATUS_SUCCESS;
}
