# Airdraft

Airdraft is a native Win32 C++ tray for sending files through the Apple AirDrop sheet. This Windows 11 x64 developer preview targets the internal Intel AX210 PCIe card (8086:2725). The Airdraft Radio Service manages the card and the Wi-Fi connection. See [Credits](CREDITS.md) and [Provenance](PROVENANCE.md).

## Build

From this source directory on Windows 11 x64 with Visual Studio 2022 Build Tools (MSVC x64, CMake, Ninja and Windows SDK), Git, Python 3.10 or newer, and the .NET 8 SDK:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

The build obtains pinned dependencies, builds the Intel backend and native tray, and runs offline service and installer checks. It writes a developer preview package, corresponding source archive and SHA-256 manifest to `build`. Install Inno Setup 6 before building to produce a setup executable. Otherwise the build produces an installable ZIP. Keep the preview and source archive together when sharing the build. See [Provenance](PROVENANCE.md).

Use `-StageOnly` to build the local AX210 setup folder at `build\stage-ax210-local`. This mode skips project tests and release archives. Add `-UseCachedDependencies` to build from dependencies already available on this PC. The build generates an AX210-only register-proxy catalog. Windows signs the catalog and driver during Setup.

## Experimental RTL8852BE development

The local development build targets exactly one RTL8852BE PCIe card with ID `10EC:B852` or `10EC:B85B`. It uses Orchard's RTL8852BE port, the existing register proxy interface and winawdl's radio protocol. The standard developer preview targets the AX210.

Use the vcpkg baseline from winawdl's dependency manifest. On a fresh checkout, run these commands from the source directory with the build tools listed above.

```powershell
git clone https://github.com/microsoft/vcpkg.git third_party/vcpkg
git -C third_party/vcpkg checkout --detach e456309491fd875487bf02b8a0dca76c1b1b00cc
.\third_party\vcpkg\bootstrap-vcpkg.bat -disableMetrics
$env:VCPKG_ROOT = (Resolve-Path .\third_party\vcpkg).Path
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -ExperimentalRtl8852be
```

The command stages a local `Setup.cmd` under `build\stage-rtl8852be`. Use `-IncludeRtl8852be` instead to stage both radios under `build\stage-dual-radio`. Combined Setup detects the single supported adapter and installs its radio and driver. On a fresh PC with both models, use `install.ps1 -Adapter AX210` or `install.ps1 -Adapter RTL8852BE` from an administrator PowerShell. An update keeps the previously selected adapter. Add `-StageOnly` to skip project tests. Use `scripts\build-radio-rtl8852be.ps1` to build just the C++ radio. The Realtek register proxy INF covers both RTL8852BE PCI IDs. The Realtek build includes vendor firmware under review for redistribution. [Provenance](PROVENANCE.md)

On an RTL8852BE development PC, choose **Start AirDrop session…** to give AirDrop the Realtek card temporarily. Windows Wi-Fi reconnects after the session releases the adapter. If another physical adapter already covers every active IPv4 and IPv6 route, choose **Start AirDrop with backup connection** in the tray to keep that connection under observation throughout the session. Airdraft saves the Realtek card's Wi-Fi profile and native IP paths for recovery after release. If the driver fails to return, run the installed `restore.cmd`. Radio bring-up, driver recovery and Apple transfers still require physical RTL8852BE qualification.

## Install and recover

Run Setup or extract the preview ZIP and double-click `Setup.cmd`. Approve the Windows administrator prompt. The ZIP installs to `C:\Program Files\Airdraft` and starts the tray unelevated. For a command-line install, run `install.ps1` from Windows PowerShell as administrator.

Setup checks for a supported adapter, creates a locally trusted signing certificate, signs the UMDF register proxy, and removes the private key. It installs the radio service and link-local firewall rules. The AX210 setup also installs the signed OpenVPN TAP adapter for concurrent Wi-Fi. Device policy must permit the locally signed UMDF package. Setup upgrades an existing Airdraft 0.1.0 installation and starts the radio service as part of the upgrade.

**Receive files** is the saved preference. **Ready to receive** means the engine is listening and Windows accepted its initial AWDL mDNS announcement. On an iPhone, choose **Everyone for 10 Minutes** before a transfer. Incoming requests show the advertised name as unverified. Accepted files go to the selected folder, which defaults to `Downloads\Airdraft`.

Open Airdraft from Start or choose **Open Airdraft** from the tray. Nearby Devices is the main window. Use **Choose files**, select a ready recipient and press **Send**. Dropping files on a device queues them for that recipient. **Review files** lets you remove individual files from the queue. Progress and cancellation appear below the device list. If Windows is protecting its Wi-Fi connection, use **Start AirDrop** to confirm the temporary handoff.

Open **Settings** from the nearby window or the tray menu. Choose the name that appears in AirDrop, where incoming files are saved, whether to show devices still connecting, and whether to show transfer notifications. The **Received files** button opens the selected folder.

Nearby Devices shows AirDrop link and Bluetooth advertising status. The native tray starts the Bluetooth advertisement after the engine sends its first discovery query on AWDL. If Windows rejects advertising, the window shows the reason when available. Nearby discovery ends when the window is closed and any outgoing transfer finishes. [Windows Bluetooth publisher](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementpublisher)

### Wi-Fi handoff

The preview keeps Intel's driver active when the AX210 carries the only connection. Choose **Start AirDrop session…** in the tray to give AirDrop temporary control of the AX210. Wi-Fi disconnects during that session. **Stop AirDrop session**, quitting, locking or suspending returns the card to Windows. Where another adapter covers every active IP path, the preview can also use its register-proxy driver in concurrent mode. It checks the saved Wi-Fi profile and gateway replies before switching and verifies the previous IPv4 and IPv6 paths before showing Ready. If that fails, Airdraft attempts to restore Intel's driver. AirDrop alongside Windows Wi-Fi on one AX210 with no driver switch remains a separate development build. [WiFiCx development build](docs/KERNEL.md)

During the session, Airdraft checks the IPv4 and IPv6 gateways independently through the TAP adapter. Repeated misses count toward recovery after a gateway has answered at least once. [Windows IPv6 echo API](https://learn.microsoft.com/en-us/windows/win32/api/icmpapi/nf-icmpapi-icmp6sendecho2)

On release, the radio gets time to clean up its adapter and exit before the service binds Intel's driver. If the radio hangs, the service tries to terminate it and confirms it exited before rebinding. The service then waits for the AX210's previous IP paths. If the adapter remains disconnected after a short grace period and its previous saved profile is available, Airdraft asks Windows to reconnect once. The tray reports incomplete recovery and prevents another handoff while those paths are missing. [Windows WLAN reconnect API](https://learn.microsoft.com/en-us/windows/win32/api/wlanapi/nf-wlanapi-wlanconnect)

The tray shows when Windows is applying a driver change. A Stop request received during the switch queues recovery after Windows finishes. If Windows reports that takeover requires a restart, Airdraft cancels radio startup and attempts to restore the vendor driver. [Windows driver installation API](https://learn.microsoft.com/en-us/windows/win32/api/newdev/nf-newdev-updatedriverforplugandplaydevicesw)

Quit the tray to release the radio. If Windows Wi-Fi does not recover, run `restore.cmd` in the install directory and check `%ProgramData%\Airdraft\restore.log`. Restore disables the radio service and checks that Windows has a working driver and any previously recorded IP paths on the same card. Rerun Setup to use Airdraft again. Uninstall through Windows Settings when installed by Setup, or run `uninstall.ps1` from the install directory as an administrator for the ZIP preview. If recovery fails during uninstall, the installed copies of `restore.cmd` and `uninstall.ps1` and the saved recovery data are retained. Received files remain in Downloads.
