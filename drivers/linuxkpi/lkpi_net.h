/*
 * PROJECT:     ReactOS Linux kernel module host (LinuxKPI)
 * PURPOSE:     Interface between linuxkpi.sys (Linux network drivers) and lkpinet.sys (the NDIS miniport).
 *
 * linuxkpi.sys reports one PDO per Linux net_device on its bus (hardware ID LKPI\NET). lkpinet.sys is the function
 * driver for those PDOs; it gets this interface with IRP_MN_QUERY_INTERFACE (GUID_LKPI_NET_INTERFACE) sent to the PDO.
 */
#pragma once

/* {6D3C3A9E-5B1F-4C47-9A51-2E8C7F0B1342} */
DEFINE_GUID(GUID_LKPI_NET_INTERFACE, 0x6d3c3a9e, 0x5b1f, 0x4c47, 0x9a, 0x51, 0x2e, 0x8c, 0x7f, 0x0b, 0x13, 0x42);
#define LKPI_NET_INTERFACE_VERSION 1

/* Same layout as struct kpi_net_info in kpi7_net.c */
typedef struct _LKPI_NET_INFO
{
    UCHAR Mac[8];
    ULONG Mtu;
    ULONG Link;             /* carrier up */
    ULONG Mbps;             /* 0 = unknown */
    CHAR Driver[32];        /* Linux driver name, e.g. "r8169" */
    CHAR IfName[16];        /* e.g. "eth0" */
} LKPI_NET_INFO, *PLKPI_NET_INFO;

#define LKPI_NET_RX_PROMISCUOUS   0x1
#define LKPI_NET_RX_ALL_MULTICAST 0x2

/* Callbacks into the miniport. Rx: DISPATCH_LEVEL, the whole Ethernet frame (no FCS), valid only during the call.
 * Link: PASSIVE_LEVEL. TxWake: DISPATCH_LEVEL, the transmit queue has room again after Xmit returned 1. */
typedef VOID (NTAPI *PLKPI_NET_RX)(PVOID Context, const VOID *Frame, ULONG Length);
typedef VOID (NTAPI *PLKPI_NET_LINK)(PVOID Context, BOOLEAN Up, ULONG Mbps);
typedef VOID (NTAPI *PLKPI_NET_TX_WAKE)(PVOID Context);

typedef struct _LKPI_NET_CALLBACKS
{
    PLKPI_NET_RX Rx;
    PLKPI_NET_LINK Link;
    PLKPI_NET_TX_WAKE TxWake;
    PVOID Context;
} LKPI_NET_CALLBACKS, *PLKPI_NET_CALLBACKS;

typedef struct _LKPI_NET_INTERFACE
{
    INTERFACE Header;
    /* PASSIVE_LEVEL: bring the interface up and start the callbacks / stop them and bring it down */
    NTSTATUS (NTAPI *Open)(PVOID Context, const LKPI_NET_CALLBACKS *Callbacks);
    VOID (NTAPI *Close)(PVOID Context);
    /* <= DISPATCH_LEVEL. Returns 0 (sent; the frame was copied), 1 (queue full: retry after TxWake), < 0 (dropped) */
    LONG (NTAPI *Xmit)(PVOID Context, const VOID *Frame, ULONG Length);
    /* <= DISPATCH_LEVEL */
    VOID (NTAPI *GetInfo)(PVOID Context, PLKPI_NET_INFO Info);
    VOID (NTAPI *SetRxFlags)(PVOID Context, ULONG Flags);
    /* rx packets, tx packets, rx bytes, tx bytes, rx errors, tx errors */
    VOID (NTAPI *GetStats)(PVOID Context, ULONG64 Stats[6]);
} LKPI_NET_INTERFACE, *PLKPI_NET_INTERFACE;
