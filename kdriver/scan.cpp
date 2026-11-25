// SPDX-License-Identifier: GPL-2.0-only
/*
 * WDI_TASK_SCAN. The listener (MAC 0, promiscuous) visits each channel: on
 * channels where transmitting is allowed it sends a probe request and dwells
 * 40 ms, elsewhere it listens 110 ms for beacons (a beacon interval). Beacons
 * and probe responses fill the BSS table; at the end every entry seen in this
 * scan goes up in WDI_INDICATION_BSS_ENTRY_LIST and the task completes with
 * WDI_INDICATION_SCAN_COMPLETE. The listener then returns to its home channel
 * (AWDL's). A firmware (UMAC) background scan can replace the visiting later
 * without changing anything above this file.
 */
#include "precomp.h"

static const UCHAR kDefaultChannels[] = {1,   6,   11,  2,   3,   4,   5,   7,   8,   9,   10,  12,  13,
                                         36,  40,  44,  48,  149, 153, 157, 161, 165, 52,  56,  60,  64,
                                         100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144};
static const ULONG kActiveDwellUs = 40000, kPassiveDwellUs = 110000;
static const ULONG kNvmChannelActive = 1u << 3; /* IWX_NVM_CHANNEL_ACTIVE */

static ULONGLONG NowUs() { return KeQueryInterruptTime() / 10; }
static ULONGLONG g_scanStartUs; /* entries seen since then are this scan's */

const UCHAR *NekoFindIe(const UCHAR *ies, ULONG len, UCHAR id, UCHAR *outLen) {
  for (ULONG i = 0; i + 2 <= len;) {
    const UCHAR eid = ies[i], n = ies[i + 1];
    if (i + 2 + n > len) break;
    if (eid == id) {
      if (outLen) *outLen = n;
      return ies + i + 2;
    }
    i += 2 + n;
  }
  return nullptr;
}

NEKO_BSS *NekoFindBss(NEKO_DEVICE *dev, const UCHAR bssid[6]) {
  for (auto &b : dev->Bss)
    if (b.Valid && RtlEqualMemory(b.Bssid, bssid, 6)) return &b;
  return nullptr;
}

static void SendProbe(NEKO_DEVICE *dev, UCHAR channel) {
  UCHAR f[128] = {};
  ULONG n = 0;
  f[n++] = 0x40; /* probe request */
  f[n++] = 0x00;
  n += 2; /* duration */
  RtlFillMemory(f + n, 6, 0xff);
  n += 6;
  RtlCopyMemory(f + n, dev->Address, 6);
  n += 6;
  RtlFillMemory(f + n, 6, 0xff);
  n += 6;
  n += 2; /* sequence: the firmware fills it */
  f[n++] = 0; /* SSID */
  f[n++] = static_cast<UCHAR>(dev->ScanSsidLength);
  RtlCopyMemory(f + n, dev->ScanSsid, dev->ScanSsidLength);
  n += dev->ScanSsidLength;
  static const UCHAR rates24[] = {1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 50, 4, 0x30, 0x48, 0x60, 0x6c};
  static const UCHAR rates5[] = {1, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
  const UCHAR *r = channel <= 14 ? rates24 : rates5;
  const ULONG rl = channel <= 14 ? sizeof(rates24) : sizeof(rates5);
  RtlCopyMemory(f + n, r, rl);
  n += rl;
  khal_listener_transmit(f, n, channel <= 14 ? 2 : 12);
}

static void Visit(NEKO_DEVICE *dev) {
  const UCHAR ch = dev->ScanChannels[dev->ScanIndex];
  const bool active = khal_set_channel(ch, 20) && (khal_channel_flags(ch) & kNvmChannelActive) != 0;
  if (active) SendProbe(dev, ch);
  dev->ScanDwellEndUs = NowUs() + (active ? kActiveDwellUs : kPassiveDwellUs);
}

void NekoScanStart(NEKO_DEVICE *dev, const WDI_MESSAGE_HEADER *hdr, const WDI_SCAN_PARAMETERS *p) {
  dev->ScanHeader = *hdr;
  dev->ScanCount = 0;
  if (p->Optional.BandChannelList_IsPresent) {
    for (UINT32 b = 0; b < p->BandChannelList.ElementCount; b++) {
      const auto &band = p->BandChannelList.pElements[b];
      for (UINT32 c = 0; c < band.ChannelList.ElementCount && dev->ScanCount < ARRAYSIZE(dev->ScanChannels); c++) {
        const UINT32 ch = band.ChannelList.pElements[c];
        if (ch >= 1 && ch <= 177) dev->ScanChannels[dev->ScanCount++] = static_cast<UCHAR>(ch);
      }
    }
  }
  if (!dev->ScanCount) {
    RtlCopyMemory(dev->ScanChannels, kDefaultChannels, sizeof(kDefaultChannels));
    dev->ScanCount = sizeof(kDefaultChannels);
  }
  dev->ScanSsidLength = 0;
  if (p->SSIDList.ElementCount && p->SSIDList.pElements[0].ElementCount <= 32) {
    dev->ScanSsidLength = p->SSIDList.pElements[0].ElementCount;
    RtlCopyMemory(dev->ScanSsid, p->SSIDList.pElements[0].pElements, dev->ScanSsidLength);
  }
  dev->ScanIndex = 0;
  dev->Scanning = TRUE;
  g_scanStartUs = NowUs();
  /* Firmware scan first: the station and the AWDL listener keep their channels. */
  dev->ScanFirmware = !dev->NoFirmwareScan &&
                      khal_umac_scan(dev->ScanChannels, dev->ScanCount, dev->ScanSsid, dev->ScanSsidLength, dev->Address);
  if (!dev->ScanFirmware) dev->NoFirmwareScan = TRUE;
  NEKO_LOG("scan: %lu channels%s, %s", dev->ScanCount, dev->ScanSsidLength ? " (directed)" : "",
           dev->ScanFirmware ? "firmware" : "visiting");
  if (dev->ScanFirmware) {
    dev->ScanDwellEndUs = NowUs() + 15000000; /* a guard: the firmware reports the end */
    return;
  }
  khal_set_promiscuous(true);
  Visit(dev);
}

static UCHAR Quality(CHAR rssi) {
  const int q = (rssi + 90) * 100 / 60; /* -90 dBm -> 0, -30 dBm -> 100 */
  return static_cast<UCHAR>(q < 0 ? 0 : q > 100 ? 100 : q);
}

static void ReportResults(NEKO_DEVICE *dev, ULONGLONG since) {
  WDI_BSS_ENTRY_CONTAINER entries[16];
  ULONG n = 0;
  auto flush = [&]() {
    if (!n) return;
    WDI_INDICATION_BSS_ENTRY_LIST_PARAMETERS p;
    p.Optional.DeviceDescriptor_IsPresent = 1;
    p.DeviceDescriptor.ElementCount = n;
    p.DeviceDescriptor.pElements = entries;
    p.DeviceDescriptor.MemoryInternallyAllocated = false;
    UINT8 *out = nullptr;
    ULONG outLength = 0;
    if (GenerateWdiIndicationBssEntryList(&p, 0, &dev->Tlv, &outLength, &out) == NDIS_STATUS_SUCCESS) {
      NekoIndicate(dev, &dev->ScanHeader, WDI_INDICATION_BSS_ENTRY_LIST, 0, STATUS_SUCCESS, out, outLength);
      FreeGenerated(out);
    }
    p.DeviceDescriptor.pElements = nullptr;
    p.DeviceDescriptor.ElementCount = 0;
    for (ULONG i = 0; i < n; i++) {
      entries[i].BeaconFrame.pElements = nullptr;
      entries[i].BeaconFrame.ElementCount = 0;
      entries[i].ProbeResponseFrame.pElements = nullptr;
      entries[i].ProbeResponseFrame.ElementCount = 0;
    }
    n = 0;
  };
  ULONG reported = 0;
  for (auto &b : dev->Bss) {
    if (!b.Valid || b.SeenUs < since) continue;
    WDI_BSS_ENTRY_CONTAINER &e = entries[n];
    new (&e) WDI_BSS_ENTRY_CONTAINER();
    RtlCopyMemory(e.BSSID.Address, b.Bssid, 6);
    WDI_BYTE_BLOB &blob = b.Probe ? e.ProbeResponseFrame : e.BeaconFrame;
    if (b.Probe) e.Optional.ProbeResponseFrame_IsPresent = 1;
    else e.Optional.BeaconFrame_IsPresent = 1;
    blob.ElementCount = b.BodyLength;
    blob.pElements = b.Body;
    blob.MemoryInternallyAllocated = false;
    e.SignalInfo.RSSI = b.Rssi;
    e.SignalInfo.LinkQuality = Quality(b.Rssi);
    e.ChannelInfo.ChannelNumber = b.Channel;
    e.ChannelInfo.BandId = b.Channel <= 14 ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000;
    e.Optional.EntryAgeInfo_IsPresent = 1;
    e.EntryAgeInfo.HostTimeStamp = b.SeenUs * 10;
    e.EntryAgeInfo.CachedInformation = FALSE;
    reported++;
    if (++n == ARRAYSIZE(entries)) flush();
  }
  flush();
  NEKO_LOG("scan: %lu networks", reported);
}

static void Finish(NEKO_DEVICE *dev, NTSTATUS status);

void NekoScanTick(NEKO_DEVICE *dev) {
  if (!dev->Scanning) return;
  if (dev->ScanFirmware) {
    if (khal_scan_running() && NowUs() < dev->ScanDwellEndUs) return;
    if (khal_scan_running()) khal_scan_abort();
    Finish(dev, STATUS_SUCCESS);
    return;
  }
  if (NowUs() < dev->ScanDwellEndUs) return;
  if (++dev->ScanIndex < dev->ScanCount) {
    Visit(dev);
    return;
  }
  Finish(dev, STATUS_SUCCESS);
}

static void Finish(NEKO_DEVICE *dev, NTSTATUS status) {
  dev->Scanning = FALSE;
  if (!dev->ScanFirmware) khal_set_channel(dev->HomeChannel, dev->HomeChannel > 14 ? 80 : 20);
  dev->ScanFirmware = FALSE;
  if (status == STATUS_SUCCESS) ReportResults(dev, g_scanStartUs);
  g_scanStartUs = 0;
  NekoIndicate(dev, &dev->ScanHeader, WDI_INDICATION_SCAN_COMPLETE, dev->ScanHeader.TransactionId, status, nullptr, 0);
}

void NekoScanAbort(NEKO_DEVICE *dev) {
  if (!dev->Scanning) return;
  if (dev->ScanFirmware) khal_scan_abort();
  Finish(dev, STATUS_REQUEST_ABORTED);
}

void NekoScanOnFrame(NEKO_DEVICE *dev, const KhalRx *f) {
  const UCHAR sub = f->data[0] & 0xf0;
  if ((sub != 0x80 && sub != 0x50) || f->len < 24 + 12) return; /* beacon, probe response */
  const UCHAR *bssid = f->data + 16;
  const UCHAR *body = f->data + 24;
  const ULONG bodyLength = static_cast<ULONG>(f->len - 24);
  UCHAR channel = f->channel;
  UCHAR l = 0;
  if (const UCHAR *ds = NekoFindIe(body + 12, bodyLength - 12, 3, &l); ds && l >= 1) channel = ds[0];
  else if (const UCHAR *ht = NekoFindIe(body + 12, bodyLength - 12, 61, &l); ht && l >= 1) channel = ht[0];
  NEKO_BSS *b = NekoFindBss(dev, bssid);
  if (!b) {
    NEKO_BSS *oldest = nullptr;
    for (auto &e : dev->Bss) {
      if (!e.Valid) { b = &e; break; }
      if (!oldest || e.SeenUs < oldest->SeenUs) oldest = &e;
    }
    if (!b) b = oldest;
    RtlZeroMemory(b, FIELD_OFFSET(NEKO_BSS, Body));
    RtlCopyMemory(b->Bssid, bssid, 6);
    b->Valid = TRUE;
  }
  /* A probe response carries more than a beacon; keep it unless a newer one comes. */
  if (sub == 0x80 && b->Probe && NowUs() - b->SeenUs < 5000000) {
    b->SeenUs = NowUs();
    b->Rssi = f->dbm;
    return;
  }
  b->Probe = sub == 0x50;
  b->Channel = channel;
  b->Rssi = f->dbm;
  b->SeenUs = NowUs();
  b->BodyLength = static_cast<USHORT>(bodyLength < sizeof(b->Body) ? bodyLength : sizeof(b->Body));
  RtlCopyMemory(b->Body, body, b->BodyLength);
  if (dev->Conn == ConnUp && RtlEqualMemory(bssid, dev->ConnBssid, 6)) dev->LastRssi = f->dbm;
}
