// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware bring-up and the worker thread.
 *
 * PrepareHardware maps BAR0, reads the PCI IDs, creates the DMA enabler (so
 * every DMA buffer has a bus address that respects the IOMMU), boots the
 * firmware through the shared hardware layer and reports Wi-Fi capabilities.
 * From then on one worker thread owns the card: it runs WDI requests, the
 * scan and connect state machines, transmits what the OS queued and polls
 * the receive ring, as the user-mode radio's device thread did.
 */
#include "precomp.h"

#include <initguid.h>
#include <wdmguid.h>

#include "kplat_glue.h"

static KSTART_ROUTINE NekoWorker;
static void NekoLog(const char *line) { NEKO_LOG("%s", line); }

NEKO_FRAME *NekoFrameAlloc(ULONG length) {
  auto *f = static_cast<NEKO_FRAME *>(
      ExAllocatePool2(POOL_FLAG_NON_PAGED, FIELD_OFFSET(NEKO_FRAME, Data) + length, NEKO_TAG));
  if (f) f->Length = length;
  return f;
}

void NekoFrameFree(NEKO_FRAME *f) {
  if (f) ExFreePoolWithTag(f, NEKO_TAG);
}

static NTSTATUS ReadPciIds(NEKO_DEVICE *dev, USHORT *vendor, USHORT *device, USHORT *subsystem) {
  BUS_INTERFACE_STANDARD bus;
  NTSTATUS st = WdfFdoQueryForInterface(dev->Device, &GUID_BUS_INTERFACE_STANDARD,
                                        reinterpret_cast<PINTERFACE>(&bus), sizeof(bus), 1, nullptr);
  if (!NT_SUCCESS(st)) return st;
  UCHAR cfg[0x30] = {};
  const ULONG got = bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, cfg, 0, sizeof(cfg));
  bus.InterfaceDereference(bus.Context);
  if (got < sizeof(cfg)) return STATUS_DEVICE_DATA_ERROR;
  *vendor = *reinterpret_cast<USHORT *>(cfg + 0x00);
  *device = *reinterpret_cast<USHORT *>(cfg + 0x02);
  *subsystem = *reinterpret_cast<USHORT *>(cfg + 0x2e);
  return STATUS_SUCCESS;
}

static void ReadSettings(NEKO_DEVICE *dev) {
  dev->DataFormat = 0;
  WDFKEY key;
  if (NT_SUCCESS(WdfDeviceOpenRegistryKey(dev->Device, PLUGPLAY_REGKEY_DRIVER, KEY_READ,
                                          WDF_NO_OBJECT_ATTRIBUTES, &key))) {
    DECLARE_CONST_UNICODE_STRING(name, L"DataFormat");
    ULONG v = 0;
    if (NT_SUCCESS(WdfRegistryQueryULong(key, &name, &v))) dev->DataFormat = v;
    WdfRegistryClose(key);
  }
}

/* EvtDeviceAdd: before anything can queue work. */
void NekoInitQueues(NEKO_DEVICE *dev) {
  KeInitializeEvent(&dev->Wake, SynchronizationEvent, FALSE);
  KeInitializeSpinLock(&dev->Lock);
  InitializeListHead(&dev->Jobs);
  InitializeListHead(&dev->TxFrames);
  InitializeListHead(&dev->RxFrames);
  InitializeListHead(&dev->AwdlEvents);
}

NTSTATUS NekoHwPrepare(NEKO_DEVICE *dev, WDFCMRESLIST translated) {
  const ULONG count = WdfCmResourceListGetCount(translated);
  for (ULONG i = 0; i < count && !dev->Bar; i++) {
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(translated, i);
    if (d->Type != CmResourceTypeMemory || d->u.Memory.Length < 0x2000) continue;
    dev->Bar = static_cast<volatile UCHAR *>(
        MmMapIoSpaceEx(d->u.Memory.Start, d->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE));
    if (!dev->Bar) return STATUS_INSUFFICIENT_RESOURCES;
    dev->BarLength = d->u.Memory.Length;
  }
  if (!dev->Bar) return STATUS_DEVICE_CONFIGURATION_ERROR;

  USHORT vendor = 0, device = 0, subsystem = 0;
  NTSTATUS st = ReadPciIds(dev, &vendor, &device, &subsystem);
  if (!NT_SUCCESS(st)) return st;

  WDF_DMA_ENABLER_CONFIG dmaConfig;
  WDF_DMA_ENABLER_CONFIG_INIT(&dmaConfig, WdfDmaProfileScatterGather64Duplex, 64 * 1024);
  st = WdfDmaEnablerCreate(dev->Device, &dmaConfig, WDF_NO_OBJECT_ATTRIBUTES, &dev->Dma);
  if (!NT_SUCCESS(st)) return st;

  NekoGlueSetCard(dev->Bar, static_cast<ULONG>(dev->BarLength), vendor, device, subsystem, dev->Dma);
  ReadSettings(dev);
  dev->Tlv.AllocationContext = 0;
  dev->Tlv.PeerVersion = WifiDeviceGetOsWdiVersion(dev->Device);
  NEKO_LOG("card %04x:%04x subsystem %04x, BAR0 %Iu bytes, OS WDI version 0x%x", vendor, device, subsystem,
           dev->BarLength, dev->Tlv.PeerVersion);

  /* Better timer resolution for the poll loop until interrupts drive it. */
  ExSetTimerResolution(10000, TRUE);

  char error[160] = {};
  dev->HomeChannel = 6;
  if (!khal_open(dev->HomeChannel, "ZZ", NekoLog, error, sizeof(error))) {
    NEKO_LOG("firmware bring-up failed: %s", error);
    return STATUS_DEVICE_NOT_READY;
  }
  dev->HwOpen = TRUE;
  dev->RadioOn = TRUE;
  KhalInfo info;
  khal_info(&info);
  RtlCopyMemory(dev->Address, info.hw_address, 6);
  NEKO_LOG("firmware up, address %02x:%02x:%02x:%02x:%02x:%02x", dev->Address[0], dev->Address[1],
           dev->Address[2], dev->Address[3], dev->Address[4], dev->Address[5]);

  NekoIrqEnable(dev);
  st = NekoSetCapabilities(dev, info);
  if (!NT_SUCCESS(st)) return st;

  dev->Stop = 0;
  HANDLE thread;
  st = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, nullptr, nullptr, nullptr, NekoWorker, dev);
  if (!NT_SUCCESS(st)) return st;
  ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                            reinterpret_cast<PVOID *>(&dev->Thread), nullptr);
  ZwClose(thread);
  /* AirDrop's side door; Wi-Fi works without it. */
  const NTSTATUS awdl = NekoAwdlCreate(dev);
  if (!NT_SUCCESS(awdl)) NEKO_LOG("AWDL control device not created: 0x%08x", awdl);
  return STATUS_SUCCESS;
}

void NekoHwRelease(NEKO_DEVICE *dev) {
  if (dev->Thread) {
    InterlockedExchange(&dev->Stop, 1);
    KeSetEvent(&dev->Wake, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(dev->Thread, Executive, KernelMode, FALSE, nullptr);
    ObDereferenceObject(dev->Thread);
    dev->Thread = nullptr;
  }
  /* Requests the worker never got to: fail them so nobody waits forever. */
  while (!IsListEmpty(&dev->Jobs)) {
    auto *job = CONTAINING_RECORD(RemoveHeadList(&dev->Jobs), NEKO_JOB, Link);
    if (job->IoRequest) WdfRequestComplete(job->IoRequest, STATUS_DEVICE_REMOVED);
    else WifiRequestComplete(job->Request, STATUS_DEVICE_NOT_READY, 0);
    ExFreePoolWithTag(job, NEKO_TAG);
  }
  NekoAwdlDestroy(dev); /* worker stopped: safe to touch the listener here */
  NekoIrqDisable(dev);
  if (dev->HwOpen) {
    khal_close();
    dev->HwOpen = FALSE;
  }
  NekoGlueRunCloseOps();
  NekoGlueSetCard(nullptr, 0, 0, 0, 0, nullptr);
  /* Drop whatever is still queued. */
  LIST_ENTRY *lists[] = {&dev->TxFrames, &dev->RxFrames};
  for (LIST_ENTRY *l : lists) {
    while (!IsListEmpty(l)) NekoFrameFree(CONTAINING_RECORD(RemoveHeadList(l), NEKO_FRAME, Link));
  }
  ExSetTimerResolution(0, FALSE);
  if (dev->Bar) {
    MmUnmapIoSpace(const_cast<UCHAR *>(dev->Bar), dev->BarLength);
    dev->Bar = nullptr;
  }
}

void NekoKick(NEKO_DEVICE *dev) { KeSetEvent(&dev->Wake, IO_NO_INCREMENT, FALSE); }

void NekoQueueTx(NEKO_DEVICE *dev, NEKO_FRAME *f) {
  KIRQL irql;
  KeAcquireSpinLock(&dev->Lock, &irql);
  InsertTailList(&dev->TxFrames, &f->Link);
  KeReleaseSpinLock(&dev->Lock, irql);
  NekoKick(dev);
}

void NekoQueueRx(NEKO_DEVICE *dev, NEKO_FRAME *f) {
  KIRQL irql;
  KeAcquireSpinLock(&dev->Lock, &irql);
  if (dev->RxCount >= 512) { /* the OS stopped taking frames: drop the oldest */
    NekoFrameFree(CONTAINING_RECORD(RemoveHeadList(&dev->RxFrames), NEKO_FRAME, Link));
    dev->RxCount--;
  }
  InsertTailList(&dev->RxFrames, &f->Link);
  dev->RxCount++;
  KeReleaseSpinLock(&dev->Lock, irql);
}

/* Every received frame: scan results, the station's management frames, data. */
static void OnRx(void *ctx, const KhalRx *f) {
  auto *dev = static_cast<NEKO_DEVICE *>(ctx);
  if (f->len < 24) return;
  const UCHAR type = f->data[0] & 0x0c;
  NekoAwdlOnFrame(dev, f); /* the AWDL engine sees everything, as on the radio pipe */
  if (type == 0x00) { /* management */
    NekoScanOnFrame(dev, f);
    NekoConnectOnFrame(dev, f);
  } else if (type == 0x08) {
    NekoDataOnFrame(dev, f);
  }
}

static void NekoWorker(PVOID context) {
  auto *dev = static_cast<NEKO_DEVICE *>(context);
  khal_set_rx(OnRx, dev);
  ULONGLONG lastHealth = 0, lastIrqUs = 0;
  LONG irqSeen = 0;
  LARGE_INTEGER timeout;
  while (!dev->Stop) {
    /* WDI requests */
    for (;;) {
      KIRQL irql;
      KeAcquireSpinLock(&dev->Lock, &irql);
      LIST_ENTRY *e = IsListEmpty(&dev->Jobs) ? nullptr : RemoveHeadList(&dev->Jobs);
      KeReleaseSpinLock(&dev->Lock, irql);
      if (!e) break;
      auto *job = CONTAINING_RECORD(e, NEKO_JOB, Link);
      NekoWdiRun(dev, job);
      ExFreePoolWithTag(job, NEKO_TAG);
    }
    /* Frames from the OS */
    for (unsigned n = 0; n < 64; n++) {
      KIRQL irql;
      KeAcquireSpinLock(&dev->Lock, &irql);
      LIST_ENTRY *e = IsListEmpty(&dev->TxFrames) ? nullptr : RemoveHeadList(&dev->TxFrames);
      KeReleaseSpinLock(&dev->Lock, irql);
      if (!e) break;
      auto *f = CONTAINING_RECORD(e, NEKO_FRAME, Link);
      NekoDataTransmit(dev, f);
      NekoFrameFree(f);
    }
    if (dev->HwOpen) {
      const int got = khal_poll();
      NekoScanTick(dev);
      NekoConnectTick(dev);
      if (dev->RxCount) NekoRxNotify(dev);
      NekoAwdlTick(dev);
      const ULONGLONG now = KeQueryInterruptTime() / 10;
      if (now - lastHealth > 1000000) {
        lastHealth = now;
        khal_check_health();
        if (!khal_healthy()) NEKO_LOG("the firmware failed; the device needs a restart");
      }
      if (got) continue; /* more may be waiting */
    }
    /* 1 ms polls until interrupts prove they arrive, then 10 ms as a safety net. */
    const ULONGLONG nowUs = KeQueryInterruptTime() / 10;
    if (dev->IrqCount != irqSeen) {
      irqSeen = dev->IrqCount;
      lastIrqUs = nowUs;
    }
    timeout.QuadPart = (lastIrqUs && nowUs - lastIrqUs < 2000000) ? -100000 : -10000;
    KeWaitForSingleObject(&dev->Wake, Executive, KernelMode, FALSE, &timeout);
  }
  khal_set_rx(nullptr, nullptr);
  PsTerminateSystemThread(STATUS_SUCCESS);
}
