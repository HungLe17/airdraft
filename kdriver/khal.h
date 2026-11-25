// SPDX-License-Identifier: GPL-2.0-only
/*
 * The hardware layer as the driver glue sees it: plain C types, no C++
 * library, so the WDF/WiFiCx files never include it. Implemented in
 * khal.cpp over IwxDevice (iwxradio/src, shared with the user-mode radio).
 * Every call runs on the driver's worker thread (PASSIVE_LEVEL).
 */
#pragma once

#ifdef _NTDDK_
/* The WDF glue: kernel headers only, no C library. */
#ifndef _STDINT
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
#endif
#else
#include <stddef.h>
#include <stdint.h>
#endif

struct KhalRx {
  const uint8_t *data; /* 802.11 frame, no FCS */
  size_t len;
  int8_t dbm;
  uint8_t channel;
  uint16_t status;   /* IWX_RX_MPDU_RES_STATUS_* */
  bool mic_stripped; /* CCMP MIC already removed */
};
typedef void (*KhalRxCallback)(void *ctx, const KhalRx *frame);
typedef void (*KhalLogCallback)(const char *line);

struct KhalInfo {
  uint8_t hw_address[6];
  char firmware[64];
  uint8_t tx_chains, rx_chains;
};

/* Boots the firmware and starts the listener (MAC 0) on `channel`. */
bool khal_open(uint8_t channel, const char *country, KhalLogCallback log, char *error, size_t error_len);
void khal_close();
bool khal_is_open();
void khal_info(KhalInfo *info);
void khal_set_rx(KhalRxCallback cb, void *ctx);
/* Handles what the card delivered; returns the number of buffers. */
int khal_poll();
void khal_check_health();
bool khal_healthy();
bool khal_listener_healthy();

/* Listener (scan, AWDL) */
bool khal_set_channel(uint8_t channel, unsigned width_mhz);
bool khal_retune_listener(uint8_t channel, unsigned width_mhz);
uint8_t khal_channel();
bool khal_set_promiscuous(bool on);
bool khal_set_listener_address(const uint8_t address[6]);
/* Raw frame on the listener at a legacy rate (mbps_x2 = 12 is 6 Mb/s). */
bool khal_listener_transmit(const uint8_t *frame, size_t len, unsigned mbps_x2);
/* Any rate (kind: 0 legacy, 1 HT, 2 VHT, 3 firmware rate control); returns
 * the ring slot for the TX-status callback, -1 on failure. */
int khal_listener_transmit_rate(const uint8_t *frame, size_t len, unsigned kind, unsigned mbps_x2, unsigned mcs,
                                unsigned nss, bool wide80);
typedef void (*KhalTxStatusCallback)(void *ctx, unsigned slot, bool acked);
void khal_set_tx_status(KhalTxStatusCallback cb, void *ctx);
bool khal_add_rx_ba(const uint8_t peer[6], unsigned tid, uint16_t ssn, uint16_t window, uint8_t token);
bool khal_remove_rx_ba(unsigned tid);
uint8_t khal_rx_ba_tids();
bool khal_promiscuous();
unsigned khal_width();
struct KhalTxStats {
  uint64_t submitted, completed, acked, failed, retries, dropped_full;
};
void khal_tx_stats(KhalTxStats *out);
/* IWX_NVM_CHANNEL_* flags from the last regulatory update. */
uint32_t khal_channel_flags(uint8_t channel);
bool khal_set_country(const char alpha2[3]);

/* Firmware scan: the firmware visits the channels between our slots. */
bool khal_umac_scan(const uint8_t *channels, size_t count, const uint8_t *ssid, size_t ssid_len, const uint8_t address[6]);
bool khal_scan_running();
void khal_scan_abort();

/* Station (MAC 1) */
struct KhalSta {
  uint8_t bssid[6];
  uint8_t addr[6];
  uint8_t channel;
  uint8_t center_channel;
  unsigned width;
  uint16_t beacon_int;
  bool ht, vht, mimo;
};
bool khal_sta_join(const KhalSta *sta);
bool khal_sta_associated(const KhalSta *agreed, uint16_t aid, uint8_t dtim);
bool khal_sta_rate_control();
bool khal_sta_set_key(bool group, unsigned index, const uint8_t key[16]);
bool khal_sta_set_mfp(bool on);
bool khal_sta_set_igtk(unsigned key_id, const uint8_t key[16], uint64_t rsc);
/* `mgmt`: management/EAPOL at a basic rate; otherwise firmware rate control. */
bool khal_sta_transmit(const uint8_t *frame, size_t len, bool encrypt, bool mgmt);
bool khal_sta_add_rx_ba(unsigned tid, uint16_t ssn, uint16_t window);
void khal_sta_leave();
bool khal_sta_active();
unsigned khal_sta_tx_acked();
unsigned khal_sta_tx_failed();
