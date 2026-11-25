/*
 * PROJECT:     ReactOS Linux kernel module host (LinuxKPI)
 * PURPOSE:     The LinuxKPI bus (lkpidisk.c): its children are Linux disks and Linux network devices (netbus.c).
 */
#pragma once

typedef struct _LKDISK_COMMON
{
    BOOLEAN IsFdo;
    UCHAR Kind;                 /* LKBUS_KIND_*, PDOs only */
    PDEVICE_OBJECT Self;
} LKDISK_COMMON;

#define LKBUS_KIND_DISK 0
#define LKBUS_KIND_NET  1

/* new children are queued as tagged pointers: bit 0 = Linux scsi_device, bit 1 = Linux net_device */
#define LKBUS_TAG_SCSI 1
#define LKBUS_TAG_NET  2
#define LKBUS_TAG_MASK 3

VOID LkBusQueueChild(PVOID Tagged);

/* netbus.c */
PDEVICE_OBJECT LkNetCreatePdo(PDRIVER_OBJECT Driver, PVOID Netdev);
NTSTATUS LkNetPdoPnp(PDEVICE_OBJECT Dev, PIRP Irp);
VOID LkNetTryHook(VOID);
