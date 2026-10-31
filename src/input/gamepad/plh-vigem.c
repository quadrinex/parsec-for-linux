/* plh-vigem.sys — minimal WDM driver run by Wine's winedevice in the dedicated prefix only.
 * It exposes the public ViGEmBus device interface so that Parsec's built-in ViGEm gamepad
 * support finds a "bus", and turns the Xbox 360 target reports it submits into plh_pad_evt
 * events that plh-pad-relay.exe collects and forwards to plh-bridge.
 *
 * The bus protocol is the public one of the open-source ViGEmBus/ViGEmClient projects, written
 * here from its documented behaviour: IOCTLs are device type 0x2A, function 0x801.. (plugin,
 * unplug, check version, wait ready) and 0xA00.. (XUSB). Anything unknown is logged to
 * C:\plh-vigem.log and, if it looks like an XUSB report (20 bytes, Size field == 20), is
 * treated as one. Only Xbox 360 wired targets are supported. */
#include <ntddk.h>
#include <ntstrsafe.h>
#include "plh_pad_drv.h"

#define VIGEM_DEVTYPE      0x2A
#define VIGEM_FN_PLUGIN    0x801
#define VIGEM_FN_UNPLUG    0x802
#define VIGEM_FN_VERSION   0x803
#define VIGEM_FN_READY     0x804
#define VIGEM_FN_XUSB_BASE 0xA00   /* notification / submit / user index live from here on */
#define VIGEM_X360_WIRED   0
#define XUSB_SUBMIT_LEN    20      /* {Size, SerialNo, XUSB_REPORT(12)} */

#define LINK_BUS  L"\\??\\ROOT#PLHVIGEM#0001#{96E42B22-F5E9-42F8-B043-ED0F932F014F}"
#define LINK_PRIV L"\\??\\PlhPad"
#define EVQ 128
#define MAXPEND 8

struct target { int used; ULONG serial; PFILE_OBJECT owner; };

static DEVICE_OBJECT *g_dev;
static KSPIN_LOCK g_lock;
static struct target g_t[PLH_PAD_SLOTS];
static struct plh_pad_evt g_q[EVQ];
static ULONG g_qh, g_qn;
static IRP *g_pend[MAXPEND];
static LONG g_logn;

static void logf_(const char *fmt, ULONG a, ULONG b, ULONG c, ULONG d)
{
    char buf[160]; UNICODE_STRING name; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb; HANDLE h; LARGE_INTEGER off;
    if (InterlockedIncrement(&g_logn) > 400) return;
    RtlInitUnicodeString(&name, L"\\??\\C:\\plh-vigem.log");
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (ZwCreateFile(&h, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                     FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0) < 0) return;
    RtlStringCbPrintfA(buf, sizeof buf, fmt, a, b, c, d);
    off.QuadPart = -1;
    ZwWriteFile(h, NULL, NULL, NULL, &iosb, buf, (ULONG)strlen(buf), &off, NULL);
    ZwClose(h);
}

/* Caller holds g_lock. */
static void push(const struct plh_pad_evt *e)
{
    if (g_qn == EVQ) { g_qh = (g_qh + 1) % EVQ; g_qn--; }   /* drop oldest */
    g_q[(g_qh + g_qn++) % EVQ] = *e;
}

static int find(ULONG serial)
{
    for (int i = 0; i < PLH_PAD_SLOTS; i++) if (g_t[i].used && g_t[i].serial == serial) return i;
    return -1;
}

static void unplug_slot(int i)   /* caller holds g_lock */
{
    struct plh_pad_evt e = { PLH_PAD_EVT_UNPLUG, (ULONG)i, 0, 0, 0, 0, 0, 0, 0 };
    g_t[i].used = 0;
    push(&e);
}

static void cancel_pend(DEVICE_OBJECT *d, IRP *irp)
{
    KIRQL irql;
    (void)d;
    IoReleaseCancelSpinLock(irp->CancelIrql);
    KeAcquireSpinLock(&g_lock, &irql);
    for (int i = 0; i < MAXPEND; i++) if (g_pend[i] == irp) g_pend[i] = NULL;
    KeReleaseSpinLock(&g_lock, irql);
    irp->IoStatus.Status = STATUS_CANCELLED; irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static void complete(IRP *irp, NTSTATUS st, ULONG_PTR info)
{
    irp->IoStatus.Status = st; irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static NTSTATUS bus_ioctl(IRP *irp, IO_STACK_LOCATION *s, ULONG code)
{
    ULONG fn = (code >> 2) & 0xFFF, inl = s->Parameters.DeviceIoControl.InputBufferLength;
    ULONG *in = irp->AssociatedIrp.SystemBuffer;
    KIRQL irql;
    NTSTATUS st = STATUS_SUCCESS;

    if (fn == VIGEM_FN_VERSION) { logf_("version in=%lu v=%lx %lu %lu\n", inl, inl >= 8 ? in[1] : 0, 0, 0); return st; }
    if (fn == VIGEM_FN_READY) return st;
    if (fn == VIGEM_FN_PLUGIN || fn == VIGEM_FN_UNPLUG) {
        int i;
        if (inl < 8) return STATUS_INVALID_PARAMETER;
        KeAcquireSpinLock(&g_lock, &irql);
        i = find(in[1]);
        if (fn == VIGEM_FN_PLUGIN) {
            if (inl < 16 || in[2] != VIGEM_X360_WIRED) { st = STATUS_NOT_SUPPORTED; logf_("plugin type=%lu\n", inl >= 16 ? in[2] : 99, 0, 0, 0); }
            else if (i >= 0) st = STATUS_INVALID_PARAMETER;
            else {
                for (i = 0; i < PLH_PAD_SLOTS && g_t[i].used; i++) ;
                if (i == PLH_PAD_SLOTS) st = STATUS_INSUFFICIENT_RESOURCES;
                else {
                    struct plh_pad_evt e = { PLH_PAD_EVT_PLUG, (ULONG)i, 0, 0, 0, 0, 0, 0, 0 };
                    g_t[i].used = 1; g_t[i].serial = in[1]; g_t[i].owner = s->FileObject;
                    push(&e);
                }
            }
        } else if (i >= 0) unplug_slot(i);
        else st = STATUS_DEVICE_DOES_NOT_EXIST;
        KeReleaseSpinLock(&g_lock, irql);
        return st;
    }
    /* XUSB range or unknown: XUSB report submit (20 bytes, Size == 20) ... */
    if (inl == XUSB_SUBMIT_LEN && in[0] == XUSB_SUBMIT_LEN) {
        struct plh_pad_evt e; int i;
        const uint8_t *r = (const uint8_t *)&in[2];
        KeAcquireSpinLock(&g_lock, &irql);
        i = find(in[1]);
        if (i < 0) st = STATUS_DEVICE_DOES_NOT_EXIST;
        else {
            e.type = PLH_PAD_EVT_REPORT; e.slot = (ULONG)i;
            e.buttons = *(const uint16_t *)r; e.lt = r[2]; e.rt = r[3];
            e.lx = *(const int16_t *)(r + 4); e.ly = *(const int16_t *)(r + 6);
            e.rx = *(const int16_t *)(r + 8); e.ry = *(const int16_t *)(r + 10);
            push(&e);
        }
        KeReleaseSpinLock(&g_lock, irql);
        return st;
    }
    /* ... otherwise a request that waits for host-to-device data (rumble / LED): keep it pending. */
    if (fn >= VIGEM_FN_XUSB_BASE && s->Parameters.DeviceIoControl.OutputBufferLength > 0) {
        int slot = -1;
        logf_("pending ioctl %lx in=%lu out=%lu\n", code, inl, s->Parameters.DeviceIoControl.OutputBufferLength, 0);
        KeAcquireSpinLock(&g_lock, &irql);
        for (int k = 0; k < MAXPEND; k++) if (!g_pend[k]) { slot = k; break; }
        if (slot >= 0) {
            g_pend[slot] = irp;
            IoMarkIrpPending(irp);
            IoSetCancelRoutine(irp, cancel_pend);
            if (irp->Cancel && IoSetCancelRoutine(irp, NULL)) { g_pend[slot] = NULL; KeReleaseSpinLock(&g_lock, irql); return STATUS_CANCELLED; }
        }
        KeReleaseSpinLock(&g_lock, irql);
        return slot >= 0 ? STATUS_PENDING : STATUS_INSUFFICIENT_RESOURCES;
    }
    logf_("unhandled ioctl %lx in=%lu out=%lu\n", code, inl, s->Parameters.DeviceIoControl.OutputBufferLength, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS dispatch(DEVICE_OBJECT *dev, IRP *irp)
{
    IO_STACK_LOCATION *s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st = STATUS_SUCCESS;
    ULONG_PTR info = 0;
    KIRQL irql;
    (void)dev;

    switch (s->MajorFunction) {
    case IRP_MJ_DEVICE_CONTROL: {
        ULONG code = s->Parameters.DeviceIoControl.IoControlCode;
        if (code == IOCTL_PLH_PAD_GET_EVENTS) {
            ULONG max = s->Parameters.DeviceIoControl.OutputBufferLength / sizeof(struct plh_pad_evt), n = 0;
            struct plh_pad_evt *out = irp->AssociatedIrp.SystemBuffer;
            KeAcquireSpinLock(&g_lock, &irql);
            while (n < max && g_qn) { out[n++] = g_q[g_qh]; g_qh = (g_qh + 1) % EVQ; g_qn--; }
            KeReleaseSpinLock(&g_lock, irql);
            info = n * sizeof(struct plh_pad_evt);
        } else if (((code >> 16) & 0xFFFF) == VIGEM_DEVTYPE) {
            st = bus_ioctl(irp, s, code);
            if (st == STATUS_PENDING) return st;
        } else st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    case IRP_MJ_CLEANUP:     /* handle closed (Wine may only send CLOSE): free this client's targets and cancel its waits */
    case IRP_MJ_CLOSE: {
        IRP *c[MAXPEND]; int nc = 0;
        KeAcquireSpinLock(&g_lock, &irql);
        for (int i = 0; i < PLH_PAD_SLOTS; i++) if (g_t[i].used && g_t[i].owner == s->FileObject) unplug_slot(i);
        for (int k = 0; k < MAXPEND; k++)
            if (g_pend[k] && IoGetCurrentIrpStackLocation(g_pend[k])->FileObject == s->FileObject &&
                IoSetCancelRoutine(g_pend[k], NULL)) { c[nc++] = g_pend[k]; g_pend[k] = NULL; }
        KeReleaseSpinLock(&g_lock, irql);
        while (nc) complete(c[--nc], STATUS_CANCELLED, 0);
        break;
    }
    default: break;   /* CREATE / CLOSE: succeed */
    }
    complete(irp, st, info);
    return st;
}

static void unload(DRIVER_OBJECT *drv)
{
    UNICODE_STRING l1, l2;
    RtlInitUnicodeString(&l1, LINK_BUS); RtlInitUnicodeString(&l2, LINK_PRIV);
    IoDeleteSymbolicLink(&l1); IoDeleteSymbolicLink(&l2);
    IoDeleteDevice(drv->DeviceObject);
}

NTSTATUS WINAPI DriverEntry(DRIVER_OBJECT *drv, UNICODE_STRING *reg)
{
    UNICODE_STRING dev, l1, l2;
    NTSTATUS st;
    (void)reg;
    KeInitializeSpinLock(&g_lock);
    RtlInitUnicodeString(&dev, L"\\Device\\PlhVigem");
    RtlInitUnicodeString(&l1, LINK_BUS); RtlInitUnicodeString(&l2, LINK_PRIV);
    st = IoCreateDevice(drv, 0, &dev, FILE_DEVICE_BUS_EXTENDER, FILE_DEVICE_SECURE_OPEN, FALSE, &g_dev);
    if (st < 0) return st;
    if ((st = IoCreateSymbolicLink(&l1, &dev)) < 0 || (st = IoCreateSymbolicLink(&l2, &dev)) < 0) { IoDeleteDevice(g_dev); return st; }
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) drv->MajorFunction[i] = dispatch;
    drv->DriverUnload = unload;
    g_dev->Flags |= DO_BUFFERED_IO;
    g_dev->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
