# Adding a Wi-Fi card

Airdraft runs AirDrop on a card through three layers. Each has a small
interface, so a new card family touches each layer once and nothing else.


```
AirdraftRadioService (C#)                user-mode radio daemon (C++)          kernel
┌──────────────────────────┐   spawn    ┌──────────────────────────┐  IOCTL  ┌────────────────────┐
│ CardCatalog  (which card)│ ─────────▶ │ radio daemon             │ ──────▶ │ driver/regproxy    │
│ IRadioBackend (how start)│            │  speaks winawdl's radio  │         │ (UMDF register     │
│ Card / Supervisor        │            │  pipe to awdld           │         │  proxy, largest BAR)│
└──────────────────────────┘            └──────────────────────────┘         └────────────────────┘
```

Everything above the daemon (awdld, the AirDrop engine, the client) is the
same for every card: the daemon speaks winawdl's radio pipe
(`\\.\pipe\winawdl-radio`, `radio_link.h`) and nothing else.


## Today

| Backend (`RadioBackendKind`) | Daemon | Cards | Station (Wi-Fi beside AirDrop) |
|---|---|---|---|
| `Iwx` | `airdraft-radio.exe` (`iwxradio/`, ported from ForbiddenItlwm) | Intel 22000 / AX210 families | yes |
| `Devourer` | `airdraft-radio-rtl.exe` (winawdl `radio/` on raph559/devourer's Windows PCIe backend) | Realtek RTL8852CE | on demand |
| `Devourer8852be` | `airdraft-radio-rtl8852be.exe` (`rtlradio/`, using Orchard's PCIe backend) | Realtek RTL8852BE | on demand |

## Checklist

### 1. A radio daemon

Either an existing project that already speaks the radio pipe (winawdl's
radio did, so RTL8852CE needed no C++ in this repo), or a new backend inside
`iwxradio/`:

- implement the device calls and declare what it can do in
  `static constexpr RadioCapabilities kCapabilities` (`iwxradio/src/radio_backend.h`);
  every `supports_*` bit defaults to "cannot";
- `radio::check_backend_contract<Dev>()` (a `static_assert` in `radio_main.cpp`)
  fails the build if a declared capability lacks its calls;
- offline tests go in `iwxradio/tests/` (run by `iwx_selftest` in CI).

Unknown options make winawdl's daemon exit with status 2: the service passes a
backend only what its `IRadioBackend.BuildArgs` returns.


### 2. The service

- `app/service/CardCatalog.cs`: add a `RadioBackendKind` value (new daemon only)
  and an `AdapterProfile` per PCI ID with its `AdapterCapabilities`
  (`SupportsConcurrentWifi=false` makes Concurrent run on demand), its vendor's
  driver provider names (to give the card back), and `Active: true`.
- `app/service/RadioBackends.cs`: implement `IRadioBackend` (daemon file name,
  command line from `RadioStart`) and register it in `RadioBackends.All`.
- `CardCatalog.ManualModels`: add the model so users whose card is not
  recognized by its ID can pick it by hand (Settings > Wi-Fi card).
- `CardCatalog.SelfTest()` (`AirdraftRadioService --selftest`, run by the build)
  checks that every profile and model has a registered backend.


### 3. The register proxy

- `driver/regproxy/winawdl_regproxy.inf`: add `PCI\VEN_xxxx&DEV_yyyy`. Generic
  IDs rank below the vendor's driver, so this takes over nothing by itself.
  Windows binds a driver only to an ID its INF lists, so a card picked by hand
  also needs its ID here.
- `regproxy.c` maps the largest memory BAR of at least 8 KiB; raise
  `AdapterCapabilities.MinBarBytes` if the card needs more.


### 4. Installer and diagnostics

- `installer/install.ps1`: `$SupportedCardPattern` and `$WifiChips`
  (its `-SelfTest` runs in the build);
- `installer/report.ps1` and `installer/restore.ps1`: the same tables
  (their self-tests fail when they drift apart);
- `installer/install.ps1`, `uninstall.ps1`, `restore.ps1`: the daemon's process
  name in the lists of processes they stop;
- `scripts/build-installer.ps1`: build the daemon into `build\stage\radio\`,
  and copy its licence to `licenses\`.

### 5. Docs

Update the adapter notes in `README.md` and the table in `docs/COMPATIBILITY.md`.

## Candidates

From the 2026-11-22 survey. Only a card with a working Windows user-mode
driver ("a wheel") is a one-step addition; the rest are driver ports.

| Card | Status |
|---|---|
| Realtek RTL8852CE `10EC:C852` | done (winawdl + devourer) |
| Realtek RTL8852BE `10EC:B852`/`B85B` | Experimental PCIe backend using Orchard. Hardware verification is pending. |
| Realtek RTL8851BE `10EC:B851` | No PCIe backend in Airdraft. |
| Realtek USB adapters (RTL8812AU/8821AU/8812BU/8812CU/8852BU/8852CU…) | devourer drives them over libusb; winawdl's radio only opens PCIe, and they need a WinUSB driver |
| Intel Meteor Lake `7E40`/`2729`, SnJ `2726` | `cards.h` knows them; needs the `ma`/`SnJ` firmware and API 72–83 command versions |
| MediaTek MT7921/MT7922 `14C3:7961`/`0608`/`7922`/`0616` | no Windows user-mode driver yet: a port of Linux mt76 |
| Intel BE200/BE201 | needs the MLD command set (API 86+) |
| Atheros AR9xxx (ath9k) | no firmware at all (the driver does everything), fully open: Linux `ath9k`, OpenBSD `athn`; old 802.11n cards |
| Qualcomm, Broadcom | not planned (MHI/QMI firmware stack; closed AWDL offload) |

Where the driver to port comes from: the Intel radio is ForbiddenItlwm's,
itself OpenBSD's iwx ported to macOS. No other card family has an open
Hackintosh driver (Broadcom and old Atheros cards there run Apple's own closed
drivers with patches), so the source to port is the Linux/BSD driver, as
itlwm did: `rtw89` (Realtek 8852AE/BE/CE, GPL-2.0 OR BSD-3-Clause),
`mt76` (MediaTek, ISC), `ath9k`/`athn`. A port is a new backend under
`iwxradio/` (or a separate daemon speaking the radio pipe) on the same
register proxy and user-mode DMA.
