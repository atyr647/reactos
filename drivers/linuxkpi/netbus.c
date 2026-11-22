/*
 * PROJECT:     ReactOS Linux kernel module host (LinuxKPI)
 * PURPOSE:     Linux network devices as PnP children of the LinuxKPI bus (hardware ID LKPI\NET).
 *
 * kpi7_net.c reports every registered net_device (kpi_net_set_hook). Each one becomes a PDO on the bus that
 * lkpidisk.c already runs; lkpinet.sys (an NDIS miniport, installed by netlkpi.inf) is the function driver and talks
 * to the Linux driver through LKPI_NET_INTERFACE, obtained with IRP_MN_QUERY_INTERFACE.
 */

#include "linuxkpi.h"
#include "lkbus.h"
#include <initguid.h>
#include <wdmguid.h>
#include "lkpi_net.h"
#include <ntstrsafe.h>

#define LKNET_TAG 'tNKL'

/* the Linux side (kpi7_net.c) */
struct kpi_net_host_ops
{
    void (LX_ABI *rx)(void *ctx, const void *frame, unsigned int len);
    void (LX_ABI *link)(void *ctx, int up, unsigned int mbps);
    void (LX_ABI *tx_wake)(void *ctx);
};
typedef void (LX_ABI *PFN_NET_SETHOOK)(void (LX_ABI *Hook)(void *Dev, int Added));
typedef int (LX_ABI *PFN_NET_OPEN)(void *Dev, const struct kpi_net_host_ops *Ops, void *Ctx);
typedef void (LX_ABI *PFN_NET_CLOSE)(void *Dev);
typedef int (LX_ABI *PFN_NET_XMIT)(void *Dev, const void *Frame, unsigned int Len);
typedef void (LX_ABI *PFN_NET_INFO)(void *Dev, PLKPI_NET_INFO Info);
typedef void (LX_ABI *PFN_NET_FLAGS)(void *Dev, unsigned int Flags);
typedef void (LX_ABI *PFN_NET_STATS)(void *Dev, unsigned long long *Out);

static PFN_NET_OPEN NetOpen;
static PFN_NET_CLOSE NetClose;
static PFN_NET_XMIT NetXmit;
static PFN_NET_INFO NetInfo;
static PFN_NET_FLAGS NetFlags;
static PFN_NET_STATS NetStats;
static LONG NetCount;

typedef struct _LKNET_PDO
{
    LKDISK_COMMON Common;
    PVOID Netdev;
    ULONG Index;
    CHAR Driver[32];
    LKPI_NET_CALLBACKS Cb;
    BOOLEAN Open;
    KDPC TxWakeDpc;             /* TxWake reaches the miniport from a DPC, never from inside the Linux driver */
} LKNET_PDO, *PLKNET_PDO;

/* ---- Linux -> miniport ---------------------------------------------------------------------------------- */
static void LX_ABI NetRxTramp(void *Ctx, const void *Frame, unsigned int Len)
{
    PLKNET_PDO P = Ctx;

    if (P->Open && P->Cb.Rx)
        P->Cb.Rx(P->Cb.Context, Frame, Len);
}

static void LX_ABI NetLinkTramp(void *Ctx, int Up, unsigned int Mbps)
{
    PLKNET_PDO P = Ctx;

    if (P->Open && P->Cb.Link)
        P->Cb.Link(P->Cb.Context, Up != 0, Mbps);
}

static VOID NTAPI NetTxWakeDpc(PKDPC Dpc, PVOID Ctx, PVOID A1, PVOID A2)
{
    PLKNET_PDO P = Ctx;

    if (P->Open && P->Cb.TxWake)
        P->Cb.TxWake(P->Cb.Context);
}

static void LX_ABI NetTxWakeTramp(void *Ctx)
{
    PLKNET_PDO P = Ctx;

    KeInsertQueueDpc(&P->TxWakeDpc, NULL, NULL);
}

static const struct kpi_net_host_ops NetHostOps = { NetRxTramp, NetLinkTramp, NetTxWakeTramp };

/* ---- LKPI_NET_INTERFACE ------------------------------------------------------------------------------------ */
static VOID NTAPI IfRef(PVOID Ctx) { }
static VOID NTAPI IfDeref(PVOID Ctx) { }

static NTSTATUS NTAPI IfOpen(PVOID Ctx, const LKPI_NET_CALLBACKS *Cb)
{
    PLKNET_PDO P = Ctx;
    int R;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || P->Open)
        return STATUS_INVALID_DEVICE_STATE;
    P->Cb = *Cb;
    P->Open = TRUE;
    R = NetOpen(P->Netdev, &NetHostOps, P);
    if (R)
    {
        P->Open = FALSE;
        RtlZeroMemory(&P->Cb, sizeof(P->Cb));
        DbgPrint("LKPI: net %lu: open failed (errno %d)\n", P->Index, R);
        return STATUS_ADAPTER_HARDWARE_ERROR;
    }
    return STATUS_SUCCESS;
}

static VOID NTAPI IfClose(PVOID Ctx)
{
    PLKNET_PDO P = Ctx;

    if (!P->Open)
        return;
    NetClose(P->Netdev);
    P->Open = FALSE;
    KeRemoveQueueDpc(&P->TxWakeDpc);
    KeFlushQueuedDpcs();
    RtlZeroMemory(&P->Cb, sizeof(P->Cb));
}

static LONG NTAPI IfXmit(PVOID Ctx, const VOID *Frame, ULONG Len)
{
    PLKNET_PDO P = Ctx;

    return P->Open ? NetXmit(P->Netdev, Frame, Len) : -100;
}

static VOID NTAPI IfInfo(PVOID Ctx, PLKPI_NET_INFO Info) { NetInfo(((PLKNET_PDO)Ctx)->Netdev, Info); }
static VOID NTAPI IfFlags(PVOID Ctx, ULONG Flags) { NetFlags(((PLKNET_PDO)Ctx)->Netdev, Flags); }
static VOID NTAPI IfStats(PVOID Ctx, ULONG64 Stats[6]) { NetStats(((PLKNET_PDO)Ctx)->Netdev, Stats); }

/* ---- PDO --------------------------------------------------------------------------------------------------- */
static PWSTR NetSz(const CHAR *A, const CHAR *B, BOOLEAN Multi)
{
    SIZE_T La = strlen(A), Lb = B ? strlen(B) : 0, i;
    PWSTR W = ExAllocatePoolWithTag(PagedPool, (La + Lb + 3) * sizeof(WCHAR), LKNET_TAG), P = W;

    if (!W) return NULL;
    for (i = 0; i < La; i++) *P++ = (WCHAR)(UCHAR)A[i];
    *P++ = 0;
    if (B)
    {
        for (i = 0; i < Lb; i++) *P++ = (WCHAR)(UCHAR)B[i];
        *P++ = 0;
    }
    if (Multi) *P = 0;
    return W;
}

NTSTATUS LkNetPdoPnp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    PLKNET_PDO P = Dev->DeviceExtension;
    NTSTATUS Status = Irp->IoStatus.Status;
    CHAR Id[64];

    switch (Sp->MinorFunction)
    {
        case IRP_MN_START_DEVICE: case IRP_MN_QUERY_REMOVE_DEVICE: case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_STOP_DEVICE: case IRP_MN_CANCEL_REMOVE_DEVICE: case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL: case IRP_MN_REMOVE_DEVICE:
            Status = STATUS_SUCCESS;        /* the PDO lives as long as the Linux device */
            break;
        case IRP_MN_QUERY_CAPABILITIES:
        {
            PDEVICE_CAPABILITIES Caps = Sp->Parameters.DeviceCapabilities.Capabilities;
            Caps->Removable = FALSE;
            Caps->SurpriseRemovalOK = FALSE;
            Caps->UniqueID = FALSE;
            Caps->Address = P->Index;
            Caps->UINumber = P->Index;
            Status = STATUS_SUCCESS;
            break;
        }
        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (Sp->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation)
            {
                PDEVICE_RELATIONS R = ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS), LKNET_TAG);
                if (!R) { Status = STATUS_INSUFFICIENT_RESOURCES; break; }
                R->Count = 1;
                R->Objects[0] = Dev;
                ObReferenceObject(Dev);
                Irp->IoStatus.Information = (ULONG_PTR)R;
                Status = STATUS_SUCCESS;
            }
            break;
        case IRP_MN_QUERY_BUS_INFORMATION:
        {
            PPNP_BUS_INFORMATION Bi = ExAllocatePoolWithTag(PagedPool, sizeof(*Bi), LKNET_TAG);
            if (!Bi) { Status = STATUS_INSUFFICIENT_RESOURCES; break; }
            Bi->BusTypeGuid = GUID_BUS_TYPE_INTERNAL;
            Bi->LegacyBusType = Internal;
            Bi->BusNumber = 0;
            Irp->IoStatus.Information = (ULONG_PTR)Bi;
            Status = STATUS_SUCCESS;
            break;
        }
        case IRP_MN_QUERY_DEVICE_TEXT:
            if (Sp->Parameters.QueryDeviceText.DeviceTextType == DeviceTextDescription)
            {
                CHAR T[80];
                PWSTR W;
                RtlStringCbPrintfA(T, sizeof(T), "Linux %s Ethernet Adapter", P->Driver);
                W = NetSz(T, NULL, FALSE);
                Irp->IoStatus.Information = (ULONG_PTR)W;
                Status = W ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
            }
            break;
        case IRP_MN_QUERY_ID:
        {
            PWSTR W = NULL;
            RtlStringCbPrintfA(Id, sizeof(Id), "LKPI\\NET_%s", P->Driver);
            switch (Sp->Parameters.QueryId.IdType)
            {
                case BusQueryDeviceID: W = NetSz(Id, NULL, FALSE); break;
                case BusQueryHardwareIDs: W = NetSz(Id, "LKPI\\NET", TRUE); break;
                case BusQueryCompatibleIDs: W = NetSz("LKPI\\NET", NULL, TRUE); break;
                case BusQueryInstanceID:
                    RtlStringCbPrintfA(Id, sizeof(Id), "%lu", P->Index);
                    W = NetSz(Id, NULL, FALSE);
                    break;
                default: break;
            }
            if (W) { Irp->IoStatus.Information = (ULONG_PTR)W; Status = STATUS_SUCCESS; }
            break;
        }
        case IRP_MN_QUERY_INTERFACE:
            if (IsEqualGUID(Sp->Parameters.QueryInterface.InterfaceType, &GUID_LKPI_NET_INTERFACE))
            {
                PLKPI_NET_INTERFACE If = (PLKPI_NET_INTERFACE)Sp->Parameters.QueryInterface.Interface;

                if (Sp->Parameters.QueryInterface.Version != LKPI_NET_INTERFACE_VERSION ||
                    Sp->Parameters.QueryInterface.Size < sizeof(*If))
                {
                    Status = STATUS_INVALID_PARAMETER;
                    break;
                }
                If->Header.Size = sizeof(*If);
                If->Header.Version = LKPI_NET_INTERFACE_VERSION;
                If->Header.Context = P;
                If->Header.InterfaceReference = IfRef;
                If->Header.InterfaceDereference = IfDeref;
                If->Open = IfOpen;
                If->Close = IfClose;
                If->Xmit = IfXmit;
                If->GetInfo = IfInfo;
                If->SetRxFlags = IfFlags;
                If->GetStats = IfStats;
                Status = STATUS_SUCCESS;
            }
            break;
        default:
            break;
    }
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/* Bus work thread (PASSIVE_LEVEL) */
PDEVICE_OBJECT LkNetCreatePdo(PDRIVER_OBJECT Driver, PVOID Netdev)
{
    PDEVICE_OBJECT Dev;
    PLKNET_PDO P;
    LKPI_NET_INFO Info;

    if (!NT_SUCCESS(IoCreateDevice(Driver, sizeof(LKNET_PDO), NULL, FILE_DEVICE_NETWORK, FILE_AUTOGENERATED_DEVICE_NAME,
                                   FALSE, &Dev)))
        return NULL;
    P = Dev->DeviceExtension;
    RtlZeroMemory(P, sizeof(*P));
    P->Common.IsFdo = FALSE;
    P->Common.Kind = LKBUS_KIND_NET;
    P->Common.Self = Dev;
    P->Netdev = Netdev;
    P->Index = InterlockedIncrement(&NetCount) - 1;
    KeInitializeDpc(&P->TxWakeDpc, NetTxWakeDpc, P);
    NetInfo(Netdev, &Info);
    RtlCopyMemory(P->Driver, Info.Driver, sizeof(P->Driver));
    P->Driver[sizeof(P->Driver) - 1] = 0;
    Dev->Flags &= ~DO_DEVICE_INITIALIZING;
    DbgPrint("LKPI: net %lu: %s %s %02x:%02x:%02x:%02x:%02x:%02x\n", P->Index, Info.IfName, P->Driver, Info.Mac[0],
             Info.Mac[1], Info.Mac[2], Info.Mac[3], Info.Mac[4], Info.Mac[5]);
    return Dev;
}

/* Linux context (driver probe): queue the device for the bus thread */
static void LX_ABI LkNetHook(void *Netdev, int Added)
{
    if (Added)
        LkBusQueueChild((PVOID)((ULONG_PTR)Netdev | LKBUS_TAG_NET));
}

VOID LkNetTryHook(VOID)
{
    PFN_NET_SETHOOK SetHook;

    if (NetOpen)
        return;
    SetHook = (PFN_NET_SETHOOK)LkpiLookupModuleSymbol("kpi_net_set_hook");
    NetClose = (PFN_NET_CLOSE)LkpiLookupModuleSymbol("kpi_net_close");
    NetXmit = (PFN_NET_XMIT)LkpiLookupModuleSymbol("kpi_net_xmit");
    NetInfo = (PFN_NET_INFO)LkpiLookupModuleSymbol("kpi_net_get_info");
    NetFlags = (PFN_NET_FLAGS)LkpiLookupModuleSymbol("kpi_net_set_rx_flags");
    NetStats = (PFN_NET_STATS)LkpiLookupModuleSymbol("kpi_net_get_stats");
    if (!SetHook || !NetClose || !NetXmit || !NetInfo || !NetFlags || !NetStats)
        return;
    NetOpen = (PFN_NET_OPEN)LkpiLookupModuleSymbol("kpi_net_open");
    if (NetOpen)
        SetHook(LkNetHook);
}
