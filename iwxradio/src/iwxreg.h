// SPDX-License-Identifier: GPL-2.0-only
/* Includes the register header (third_party/iwx/if_iwxreg.h) with the layout
 * the firmware expects under either compiler. With clang its __packed
 * attributes do the work. With MSVC every struct is packed by #pragma pack(1);
 * the six structs there without __packed have no padding to lose (all their
 * fields fall on natural boundaries), so the layout is the same, which
 * tests/selftest checks against the iwlwifi sizes. */
#pragma once

#include "compat.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4200 4201 4505 4214)
#pragma pack(push, 1)
#endif
#include "../third_party/iwx/if_iwxreg.h"
/* The AX210 family's receive transfer descriptor, which itlwm defines in
 * ItlIwx.cpp rather than the header (Linux: struct iwl_rx_transfer_desc). */
struct iwx_rx_transfer_desc {
  uint16_t rbid;
  uint16_t reserved[3];
  uint64_t addr;
} __packed;
#ifdef _MSC_VER
#pragma pack(pop)
#pragma warning(pop)
#endif

/* The Intel command encodes the primary channel's offset from the center. */
constexpr int iwx_phy_ctrl_pos(uint8_t primary, uint8_t center, unsigned width) {
  const int offset = int(primary) - int(center);
  if (center == 0) return -1;
  switch (offset) {
  case -6: return width == 80 ? IWX_PHY_VHT_CTRL_POS_2_BELOW : -1;
  case -2: return width == 40 || width == 80 ? IWX_PHY_VHT_CTRL_POS_1_BELOW : -1;
  case  2: return width == 40 || width == 80 ? IWX_PHY_VHT_CTRL_POS_1_ABOVE : -1;
  case  6: return width == 80 ? IWX_PHY_VHT_CTRL_POS_2_ABOVE : -1;
  default: return -1;
  }
}
