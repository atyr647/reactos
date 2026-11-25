/*
 * PROJECT:     ReactOS Display Driver Model - DxgKrnl
 * PURPOSE:     Minimal host for WDDM display miniports (probe: display-only drivers such as viogpudo).
 *
 * How a miniport reaches us (reverse engineered from the displib code linked into viogpudo.sys):
 *   1. displib does ZwLoadDriver(\Registry\Machine\System\CurrentControlSet\Services\DXGKrnl),
 *   2. opens \Device\DxgKrnl and sends the internal IOCTL 0x230043 (no input, 8 byte output),
 *   3. the output is the address of our DxgkInitialize() which displib then calls with
 *      (DriverObject, RegistryPath, DXGK_INITIALIZATION_DATA *).
 * After that we own the driver object: its PnP/Power dispatch and AddDevice are ours, and we call the
 * miniport through the DDI table it registered, handing it a DXGKRNL_INTERFACE of callbacks.
 */

#include <ntifs.h>
#include <ntddk.h>
#include <ntstrsafe.h>

#define DXGK_TAG 'kgxD'
#define IOCTL_DXGK_GET_INIT 0x00230043

/* DXGK_INITIALIZATION_DATA, WDDM 1.2 (Win8) layout, only the part we need first. All members are pointers
 * after Version, so we can also dump the whole table. */
typedef struct _DXGK_INIT_DATA
{
    ULONG Version;
    ULONG Pad;
    PVOID Ddi[64];
} DXGK_INIT_DATA, *PDXGK_INIT_DATA;

enum { DDI_ADD_DEVICE, DDI_START_DEVICE, DDI_STOP_DEVICE, DDI_REMOVE_DEVICE, DDI_DISPATCH_IO_REQUEST,
       DDI_INTERRUPT_ROUTINE, DDI_DPC_ROUTINE, DDI_QUERY_CHILD_RELATIONS, DDI_QUERY_CHILD_STATUS,
       DDI_QUERY_DEVICE_DESCRIPTOR, DDI_SET_POWER_STATE, DDI_NOTIFY_ACPI_EVENT, DDI_RESET_DEVICE,
       DDI_UNLOAD, DDI_QUERY_INTERFACE };

static const char *DdiNames[] = {
    "AddDevice", "StartDevice", "StopDevice", "RemoveDevice", "DispatchIoRequest", "InterruptRoutine", "DpcRoutine",
    "QueryChildRelations", "QueryChildStatus", "QueryDeviceDescriptor", "SetPowerState", "NotifyAcpiEvent",
    "ResetDevice", "Unload", "QueryInterface", "ControlEtwLogging", "QueryAdapterInfo", "CreateDevice",
    "CreateAllocation", "DestroyAllocation", "DescribeAllocation", "GetStandardAllocationDriverData",
    "AcquireSwizzlingRange", "ReleaseSwizzlingRange", "Patch", "SubmitCommand", "PreemptCommand", "BuildPagingBuffer",
    "SetPalette", "SetPointerPosition", "SetPointerShape", "ResetFromTimeout", "RestartFromTimeout",
    "EscapeFn", "CollectDbgInfo", "QueryCurrentFence", "IsSupportedVidPn", "RecommendFunctionalVidPn",
    "EnumVidPnCofuncModality", "SetVidPnSourceAddress", "SetVidPnSourceVisibility", "CommitVidPn",
    "UpdateActiveVidPnPresentPath", "RecommendMonitorModes", "RecommendVidPnTopology", "GetScanLine",
    "StopCapture", "ControlInterrupt", "CreateOverlay", "DestroyDevice", "OpenAllocation", "CloseAllocation",
    "Render", "Present", "UpdateOverlay", "FlipOverlay", "DestroyOverlay", "CreateContext", "DestroyContext",
    "LinkDevice", "SetDisplayPrivateDriverFormat", "Escape2", "RenderKm", "PresentDisplayOnly?" };

typedef struct _DXGK_ADAPTER
{
    PDEVICE_OBJECT Fdo, Lower, Pdo;
    PVOID MiniportContext;
    DXGK_INIT_DATA Init;
    PVOID Interface;   /* DXGKRNL_INTERFACE handed to the miniport */
    UNICODE_STRING RegPath;
    PCM_RESOURCE_LIST Resources;
} DXGK_ADAPTER, *PDXGK_ADAPTER;

static PDEVICE_OBJECT DxgControlDevice;

/* ---- DXGKRNL_INTERFACE (WDDM 1.2) ----------------------------------------------------------- */
#define NUM_CB 31
static const char *CbNames[NUM_CB] = {
    "EvalAcpiMethod", "GetDeviceInformation", "IndicateChildStatus", "MapMemory", "QueueDpc", "QueryServices",
    "ReadDeviceSpace", "SynchronizeExecution", "UnmapMemory", "WriteDeviceSpace", "IsDevicePresent", "GetHandleData",
    "GetHandleParent", "EnumHandleChildren", "NotifyInterrupt", "NotifyDpc", "QueryVidPnInterface",
    "QueryMonitorInterface", "GetCaptureAddress", "LogEtwEvent", "ExcludeAdapterAccess", "CreateContextAllocation",
    "DestroyContextAllocation", "SetPowerComponentActive", "SetPowerComponentIdle", "AcquirePostDisplayOwnership",
    "PowerRuntimeControlRequest", "SetPowerComponentLatency", "SetPowerComponentResidency",
    "CompleteFStateTransition", "CompletePStateTransition" };

typedef struct _DXGK_RT_INTERFACE
{
    ULONG Size;
    ULONG Version;
    PVOID DeviceHandle;
    PVOID Cb[NUM_CB];
} DXGK_RT_INTERFACE;

/* every callback we have not implemented reports itself and fails */
#define TRAP(n) static NTSTATUS NTAPI CbTrap##n(void) { DbgPrint("DXGK: miniport called unimplemented callback %s\n", CbNames[n]); return STATUS_NOT_SUPPORTED; }
TRAP(0) TRAP(1) TRAP(2) TRAP(3) TRAP(4) TRAP(5) TRAP(6) TRAP(7) TRAP(8) TRAP(9) TRAP(10) TRAP(11) TRAP(12) TRAP(13) TRAP(14)
TRAP(15) TRAP(16) TRAP(17) TRAP(18) TRAP(19) TRAP(20) TRAP(21) TRAP(22) TRAP(23) TRAP(24) TRAP(25) TRAP(26) TRAP(27) TRAP(28)
TRAP(29) TRAP(30)
#define T(n) (PVOID)CbTrap##n
static PVOID CbTraps[NUM_CB] = { T(0),T(1),T(2),T(3),T(4),T(5),T(6),T(7),T(8),T(9),T(10),T(11),T(12),T(13),T(14),T(15),T(16),
    T(17),T(18),T(19),T(20),T(21),T(22),T(23),T(24),T(25),T(26),T(27),T(28),T(29),T(30) };

/* ---- implemented callbacks ------------------------------------------------------------------ */

typedef struct _DXGK_DEVICE_INFO_RT
{
    PVOID MiniportDeviceContext;
    PDEVICE_OBJECT PhysicalDeviceObject;
    UNICODE_STRING DeviceRegistryPath;
    PCM_RESOURCE_LIST TranslatedResourceList;
    LARGE_INTEGER SystemMemorySize;
    PHYSICAL_ADDRESS HighestPhysicalAddress;
    PHYSICAL_ADDRESS AgpApertureBase;
    SIZE_T AgpApertureSize;
    ULONG DockingState;
} DXGK_DEVICE_INFO_RT;

static NTSTATUS NTAPI
CbGetDeviceInformation(PVOID DeviceHandle, DXGK_DEVICE_INFO_RT *Info)
{
    PDXGK_ADAPTER A = DeviceHandle;
    DbgPrint("DXGK: GetDeviceInformation\n");
    RtlZeroMemory(Info, sizeof(*Info));
    Info->MiniportDeviceContext = A->MiniportContext;
    Info->PhysicalDeviceObject = A->Pdo;
    Info->DeviceRegistryPath = A->RegPath;
    Info->TranslatedResourceList = A->Resources;
    Info->SystemMemorySize.QuadPart = 1ull << 30;
    Info->HighestPhysicalAddress.QuadPart = 0xFFFFFFFFFFull;
    return STATUS_SUCCESS;
}

/* DXGK_WHICHSPACE_*: CONFIG = 0 (PCI configuration space) in the WDDM headers */
static NTSTATUS
DxgConfigAccess(PDXGK_ADAPTER A, BOOLEAN Write, PVOID Buffer, ULONG Offset, ULONG Length, PULONG Done)
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP Irp;
    PIO_STACK_LOCATION Sp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, A->Lower, NULL, 0, NULL, &Event, &Iosb);
    if (!Irp) return STATUS_INSUFFICIENT_RESOURCES;
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Sp = IoGetNextIrpStackLocation(Irp);
    Sp->MinorFunction = Write ? IRP_MN_WRITE_CONFIG : IRP_MN_READ_CONFIG;
    Sp->Parameters.ReadWriteConfig.WhichSpace = PCI_WHICHSPACE_CONFIG;
    Sp->Parameters.ReadWriteConfig.Buffer = Buffer;
    Sp->Parameters.ReadWriteConfig.Offset = Offset;
    Sp->Parameters.ReadWriteConfig.Length = Length;
    Status = IoCallDriver(A->Lower, Irp);
    if (Status == STATUS_PENDING) { KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL); Status = Iosb.Status; }
    if (Done) *Done = NT_SUCCESS(Status) ? Length : 0;
    return Status;
}

static NTSTATUS NTAPI
CbReadDeviceSpace(PVOID DeviceHandle, ULONG DataType, PVOID Buffer, ULONG Offset, ULONG Length, PULONG BytesRead)
{
    NTSTATUS S = DxgConfigAccess(DeviceHandle, FALSE, Buffer, Offset, Length, BytesRead);
    DbgPrint("DXGK: ReadDeviceSpace type %lu off %lu len %lu -> %08lx\n", DataType, Offset, Length, S);
    return S;
}

static NTSTATUS NTAPI
CbWriteDeviceSpace(PVOID DeviceHandle, ULONG DataType, PVOID Buffer, ULONG Offset, ULONG Length, PULONG BytesWritten)
{
    NTSTATUS S = DxgConfigAccess(DeviceHandle, TRUE, Buffer, Offset, Length, BytesWritten);
    DbgPrint("DXGK: WriteDeviceSpace type %lu off %lu len %lu -> %08lx\n", DataType, Offset, Length, S);
    return S;
}

static NTSTATUS NTAPI
CbMapMemory(PVOID DeviceHandle, PHYSICAL_ADDRESS Phys, ULONG Length, BOOLEAN InIoSpace, BOOLEAN MapToUser,
            MEMORY_CACHING_TYPE Cache, PVOID *Va)
{
    DbgPrint("DXGK: MapMemory %I64x len %lx\n", Phys.QuadPart, Length);
    if (MapToUser) return STATUS_NOT_SUPPORTED;
    *Va = MmMapIoSpace(Phys, Length, Cache);
    return *Va ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

static NTSTATUS NTAPI
CbUnmapMemory(PVOID DeviceHandle, PVOID Va)
{
    DbgPrint("DXGK: UnmapMemory %p\n", Va);
    /* length unknown to us; the probe leaks the mapping */
    return STATUS_SUCCESS;
}

static BOOLEAN NTAPI CbIsDevicePresent(PVOID DeviceHandle, PVOID Ids, PULONG Hw, PVOID Rev) { return TRUE; }

/* ---- PnP ----------------------------------------------------------------------------------- */

static NTSTATUS NTAPI
DxgIoCompletion(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ctx)
{
    KeSetEvent((PKEVENT)Ctx, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
DxgForwardAndWait(PDXGK_ADAPTER A, PIRP Irp)
{
    KEVENT Event;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, DxgIoCompletion, &Event, TRUE, TRUE, TRUE);
    Status = IoCallDriver(A->Lower, Irp);
    if (Status == STATUS_PENDING) { KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL); Status = Irp->IoStatus.Status; }
    return Status;
}

static NTSTATUS NTAPI
DxgPnp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PDXGK_ADAPTER A = Dev->DeviceExtension;
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    if (Dev == DxgControlDevice) { Irp->IoStatus.Status = STATUS_NOT_SUPPORTED; IoCompleteRequest(Irp, IO_NO_INCREMENT); return STATUS_NOT_SUPPORTED; }

    switch (Sp->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
        {
            DXGK_RT_INTERFACE *Rt;
            struct { ULONG RequiredDmaQueueEntry; GUID AdapterGuid; LUID AdapterLuid; } StartInfo;
            ULONG Sources = 0, Children = 0, i;
            NTSTATUS (NTAPI *StartDevice)(PVOID, PVOID, PVOID, PULONG, PULONG) = A->Init.Ddi[DDI_START_DEVICE];

            Status = DxgForwardAndWait(A, Irp);
            if (!NT_SUCCESS(Status)) { DbgPrint("DXGK: lower driver failed START (%08lx)\n", Status); break; }
            A->Resources = Sp->Parameters.StartDevice.AllocatedResourcesTranslated;
            if (A->Resources)
                for (i = 0; i < A->Resources->List[0].PartialResourceList.Count; i++)
                {
                    PCM_PARTIAL_RESOURCE_DESCRIPTOR D = &A->Resources->List[0].PartialResourceList.PartialDescriptors[i];
                    DbgPrint("DXGK: resource %lu: type %u start %I64x len %lx\n", i, D->Type, D->u.Generic.Start.QuadPart, D->u.Generic.Length);
                }

            Rt = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Rt), DXGK_TAG);
            RtlZeroMemory(Rt, sizeof(*Rt));
            Rt->Size = sizeof(*Rt);
            Rt->Version = A->Init.Version;
            Rt->DeviceHandle = A;
            for (i = 0; i < NUM_CB; i++) Rt->Cb[i] = CbTraps[i];
            Rt->Cb[1] = (PVOID)CbGetDeviceInformation;
            Rt->Cb[3] = (PVOID)CbMapMemory;
            Rt->Cb[6] = (PVOID)CbReadDeviceSpace;
            Rt->Cb[8] = (PVOID)CbUnmapMemory;
            Rt->Cb[9] = (PVOID)CbWriteDeviceSpace;
            Rt->Cb[10] = (PVOID)CbIsDevicePresent;
            A->Interface = Rt;

            RtlZeroMemory(&StartInfo, sizeof(StartInfo));
            StartInfo.RequiredDmaQueueEntry = 0;
            DbgPrint("DXGK: calling miniport DxgkDdiStartDevice %p\n", StartDevice);
            Status = StartDevice(A->MiniportContext, &StartInfo, Rt, &Sources, &Children);
            DbgPrint("DXGK: DxgkDdiStartDevice -> %08lx, %lu video present sources, %lu children\n", Status, Sources, Children);
            break;
        }
        case IRP_MN_REMOVE_DEVICE:
        case IRP_MN_STOP_DEVICE:
        default:
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(A->Lower, Irp);
    }
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS NTAPI
DxgPassDown(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PDXGK_ADAPTER A = Dev->DeviceExtension;

    if (Dev == DxgControlDevice)
    {
        PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
        if (Sp->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL && Sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_DXGK_GET_INIT)
        {
            extern NTSTATUS NTAPI DxgkInitialize(PDRIVER_OBJECT, PUNICODE_STRING, PVOID);
            PVOID *Out = Irp->UserBuffer ? Irp->UserBuffer : Irp->AssociatedIrp.SystemBuffer;
            DbgPrint("DXGK: miniport asked for DxgkInitialize (out buffer %p len %lu)\n", Out, Sp->Parameters.DeviceIoControl.OutputBufferLength);
            if (Out && Sp->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(PVOID))
            {
                *Out = (PVOID)DxgkInitialize;
                Irp->IoStatus.Information = sizeof(PVOID);
                Irp->IoStatus.Status = STATUS_SUCCESS;
            }
            else
                Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        }
        else
            Irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Irp->IoStatus.Status;
    }
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(A->Lower, Irp);
}

static NTSTATUS NTAPI
DxgPower(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PDXGK_ADAPTER A = Dev->DeviceExtension;
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(A->Lower, Irp);
}


/* Make sure the device has a software ("Driver") key so the miniport can store
 * HardwareInformation.* values (normally created by INF installation). */
static VOID
DxgEnsureDriverKey(PDEVICE_OBJECT Pdo)
{
    HANDLE Key = NULL;
    UNICODE_STRING Name, Val;
    OBJECT_ATTRIBUTES Oa;
    NTSTATUS Status;
    static const WCHAR Rel[] = L"{4D36E968-E325-11CE-BFC1-08002BE10318}\\0000";

    Status = IoOpenDeviceRegistryKey(Pdo, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &Key);
    DbgPrint("DXGK: IoOpenDeviceRegistryKey(DRIVER) -> %08lx\n", Status);
    if (NT_SUCCESS(Status)) { ZwClose(Key); return; }

    RtlInitUnicodeString(&Name, L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class\\{4D36E968-E325-11CE-BFC1-08002BE10318}\\0000");
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateKey(&Key, KEY_ALL_ACCESS, &Oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    DbgPrint("DXGK: create class key -> %08lx\n", Status);
    if (!NT_SUCCESS(Status)) return;
    ZwClose(Key);

    /* PLUGPLAY_REGKEY_DEVICE opens "Device Parameters", not the instance key, so walk
     * Enum\\PCI\\VEN_1AF4&DEV_1050* and set the Driver value on every instance (test hack). */
    {
        HANDLE Enum;
        ULONG i, Len;
        UNICODE_STRING Ven;
        PKEY_BASIC_INFORMATION Bi = ExAllocatePool(PagedPool, 512);
        RtlInitUnicodeString(&Name, L"\\Registry\\Machine\\System\\CurrentControlSet\\Enum\\PCI");
        InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (Bi && NT_SUCCESS(ZwOpenKey(&Enum, KEY_READ, &Oa)))
        {
            for (i = 0; NT_SUCCESS(ZwEnumerateKey(Enum, i, KeyBasicInformation, Bi, 512, &Len)); i++)
            {
                HANDLE Hw, Inst; ULONG j;
                Ven.Buffer = Bi->Name; Ven.Length = Ven.MaximumLength = (USHORT)Bi->NameLength;
                if (Ven.Length < 28 || _wcsnicmp(Bi->Name, L"VEN_1AF4&DEV_1050", 17)) continue;
                InitializeObjectAttributes(&Oa, &Ven, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Enum, NULL);
                if (!NT_SUCCESS(ZwOpenKey(&Hw, KEY_READ, &Oa))) continue;
                {
                    PKEY_BASIC_INFORMATION Ib = ExAllocatePool(PagedPool, 512);
                    for (j = 0; Ib && NT_SUCCESS(ZwEnumerateKey(Hw, j, KeyBasicInformation, Ib, 512, &Len)); j++)
                    {
                        UNICODE_STRING In; In.Buffer = Ib->Name; In.Length = In.MaximumLength = (USHORT)Ib->NameLength;
                        InitializeObjectAttributes(&Oa, &In, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Hw, NULL);
                        if (NT_SUCCESS(ZwOpenKey(&Inst, KEY_SET_VALUE, &Oa)))
                        {
                            RtlInitUnicodeString(&Name, L"Driver");
                            Status = ZwSetValueKey(Inst, &Name, 0, REG_SZ, (PVOID)Rel, sizeof(Rel));
                            DbgPrint("DXGK: set Driver value on %wZ -> %08lx\n", &In, Status);
                            ZwClose(Inst);
                        }
                    }
                    if (Ib) ExFreePool(Ib);
                }
                ZwClose(Hw);
            }
            ZwClose(Enum);
        }
        if (Bi) ExFreePool(Bi);
    }
    Status = IoOpenDeviceRegistryKey(Pdo, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &Key);
    DbgPrint("DXGK: IoOpenDeviceRegistryKey(DRIVER) retry -> %08lx\n", Status);
    if (NT_SUCCESS(Status)) ZwClose(Key);
}

static NTSTATUS NTAPI
DxgAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT Pdo)
{
    PDXGK_INIT_DATA Init;
    PDEVICE_OBJECT Fdo;
    PDXGK_ADAPTER A;
    NTSTATUS Status;
    NTSTATUS (NTAPI *AddDevice)(PDEVICE_OBJECT, PVOID *);
    extern PDXGK_INIT_DATA DxgLastInit;

    Init = DxgLastInit;
    Status = IoCreateDevice(DriverObject, sizeof(*A), NULL, FILE_DEVICE_VIDEO, FILE_DEVICE_SECURE_OPEN, FALSE, &Fdo);
    if (!NT_SUCCESS(Status)) return Status;
    A = Fdo->DeviceExtension;
    RtlZeroMemory(A, sizeof(*A));
    A->Fdo = Fdo; A->Pdo = Pdo; A->Init = *Init;
    A->Lower = IoAttachDeviceToDeviceStack(Fdo, Pdo);
    Fdo->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    Fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    AddDevice = A->Init.Ddi[DDI_ADD_DEVICE];
    DxgEnsureDriverKey(Pdo);
    DbgPrint("DXGK: AddDevice for PDO %p: calling miniport DxgkDdiAddDevice %p\n", Pdo, AddDevice);
    Status = AddDevice(Pdo, &A->MiniportContext);
    DbgPrint("DXGK: DxgkDdiAddDevice -> %08lx, context %p\n", Status, A->MiniportContext);
    return Status;
}

PDXGK_INIT_DATA DxgLastInit;

NTSTATUS NTAPI
DxgkInitialize(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath, PVOID InitData)
{
    PDXGK_INIT_DATA Init = InitData;
    ULONG i;
    static DXGK_INIT_DATA Saved;

    DbgPrint("DXGK: DxgkInitialize(driver %wZ) version %08lx\n", &DriverObject->DriverName, Init->Version);
    for (i = 0; i < 64 && i < RTL_NUMBER_OF(DdiNames); i++)
        if (Init->Ddi[i])
            DbgPrint("DXGK:   DDI[%2lu] %-30s = %p\n", i, DdiNames[i], Init->Ddi[i]);

    Saved = *Init;
    DxgLastInit = &Saved;
    DriverObject->DriverExtension->AddDevice = DxgAddDevice;
    DriverObject->MajorFunction[IRP_MJ_PNP] = DxgPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = DxgPower;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = DxgPassDown;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DxgPassDown;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DxgPassDown;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = DxgPassDown;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = DxgPassDown;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\DxgKrnl");
    NTSTATUS Status;
    ULONG i;

    DbgPrint("DXGK: DxgKrnl (ReactOS WDDM host probe) loading\n");
    Status = IoCreateDevice(DriverObject, 0, &Name, FILE_DEVICE_VIDEO, FILE_DEVICE_SECURE_OPEN, FALSE, &DxgControlDevice);
    if (!NT_SUCCESS(Status)) return Status;
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) DriverObject->MajorFunction[i] = DxgPassDown;
    DxgControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
