# Installs Airdraft from the staged folder it sits in. Changes, in order:
#   1. copies the files to %ProgramFiles%\Airdraft
#   2. makes a code-signing certificate on this PC, trusts it, signs the
#      register-proxy driver with it and deletes its private key (nothing else
#      can ever be signed with what this PC now trusts); adds the driver to the
#      driver store, which takes over nothing by itself
#   3. installs TAP-Windows if no tap0901 adapter exists (concurrent mode)
#   4. two inbound firewall rules, link-local addresses only
#   5. the AirdraftRadioService service, started
# The Wi-Fi card is only taken by the service, and given back on uninstall.
# -DryRun says what would happen and changes nothing.
# -SelfTest checks the card classification and the refusal message below
# against made-up devices and exits 0 (all pass) or 1; it needs no
# administrator rights and no card, and changes and writes nothing.
#
# Needs administrator rights. There is no "#Requires -RunAsAdministrator": it
# is checked while the script is parsed, before param(), so -SelfTest could not
# run on a build machine without them. The same check is made below instead,
# before anything is written.
param([switch]$DryRun, [string]$Target = "$env:ProgramFiles\Airdraft", [switch]$SelfTest,
      [ValidateSet('AX210', 'RTL8852BE')][string]$Adapter)
$ErrorActionPreference = 'Stop'

# ------------------------------------------------ Wi-Fi card classification
# Without a supported card the setup said only "no Intel Wi-Fi card Airdraft
# supports", so an RTL8852CE owner did not learn that winawdl runs their card,
# and nobody learned which ID to post (docs/HARDWARE.md, step 4). The setup
# runs this script from its own temporary folder, alone: the tables below match
# installer/report.ps1's, and -SelfTest fails when they drift apart (or from
# restore.ps1's supported IDs).
#
# The supported IDs are iwxradio/src/cards.h's kDeviceIds, the register
# proxy's INF, report.ps1's, restore.ps1's and install.ps1's check: keep them in step.
$KnownIntelIds = @('43F0', 'A0F0', '02F0', '06F0', '34F0', '3DF0', '4DF0', '2723', '2725', '7A70', '7AF0', '51F0', '51F1', '54F0', '7F70')
# Known Wi-Fi chips, 'vendor:device' -> kind, name. Used to name the chip and to
# find a card that has no driver (it is then no network adapter). Never used to
# call a device wireless by its vendor alone: Intel and Realtek make Ethernet
# chips too (10EC:8168, 8086:15xx), which must not show up here.
$WifiChips = @{
  # Intel cards cards.h knows but cannot run: no firmware upstream.
  '8086:2720' = @('intel-fw', 'Intel QnJ')
  '8086:2726' = @('intel-fw', 'Intel SnJ')
  '8086:2729' = @('intel-fw', 'Intel Ma (Meteor Lake CNVi)')
  '8086:7E40' = @('intel-fw', 'Intel Ma (Meteor Lake CNVi)')
  # Intel Wi-Fi 7 (Linux iwlwifi's Bz/Gl/Sc device IDs): no upstream driver.
  '8086:272B' = @('intel-be', 'Intel BE200 (Wi-Fi 7)')
  '8086:A840' = @('intel-be', 'Intel BE201/BE211 (Wi-Fi 7, CNVi)')
  '8086:7740' = @('intel-be', 'Intel BE201/BE211 (Wi-Fi 7, CNVi)')
  '8086:E440' = @('intel-be', 'Intel Wi-Fi 7 (CNVi)')
  '8086:E340' = @('intel-be', 'Intel Wi-Fi 7 (CNVi)')
  '8086:D340' = @('intel-be', 'Intel Wi-Fi 7 (CNVi)')
  # Known Realtek hardware from Linux rtw88/rtw89.
  '10EC:C852' = @('rtl', 'Realtek RTL8852CE')
  '10EC:8852' = @('rtl', 'Realtek RTL8852AE')
  '10EC:A85A' = @('rtl', 'Realtek RTL8852AE')
  '10EC:B852' = @('rtl-be-experimental', 'Realtek RTL8852BE')
  '10EC:B85B' = @('rtl-be-experimental', 'Realtek RTL8852BE')
  '10EC:B851' = @('rtl', 'Realtek RTL8851BE')
  '10EC:8922' = @('rtl', 'Realtek RTL8922AE')
  '10EC:C822' = @('rtl', 'Realtek RTL8822CE')
  '10EC:B822' = @('rtl', 'Realtek RTL8822BE')
  '10EC:C821' = @('rtl', 'Realtek RTL8821CE')
  '10EC:D723' = @('rtl', 'Realtek RTL8723DE')
  '10EC:B723' = @('rtl', 'Realtek RTL8723BE')
  # MediaTek, also sold as AMD RZ6xx/RZ7xx (Linux mt76).
  '14C3:7961' = @('mtk', 'MediaTek MT7921')
  '14C3:0608' = @('mtk', 'MediaTek MT7921 (AMD RZ608)')
  '14C3:7922' = @('mtk', 'MediaTek MT7922')
  '14C3:0616' = @('mtk', 'MediaTek MT7922 (AMD RZ616)')
  '14C3:7925' = @('mtk', 'MediaTek MT7925')
  '14C3:0717' = @('mtk', 'MediaTek MT7925 (AMD RZ717)')
  '14C3:7927' = @('mtk', 'MediaTek MT7927 (Wi-Fi 7)')
  '14C3:7902' = @('mtk', 'MediaTek MT7902')
  '14C3:7663' = @('mtk', 'MediaTek MT7663')
  '14C3:7615' = @('mtk', 'MediaTek MT7615')
  # Qualcomm (Linux ath11k/ath12k) and Qualcomm Atheros (ath10k/ath9k).
  '17CB:1101' = @('qcom', 'Qualcomm QCA6390')
  '17CB:1103' = @('qcom', 'Qualcomm WCN6855 (FastConnect 6800/6900)')
  '17CB:1107' = @('qcom', 'Qualcomm WCN7850 (FastConnect 7800)')
  '168C:003E' = @('qcom', 'Qualcomm Atheros QCA6174')
  '168C:0042' = @('qcom', 'Qualcomm Atheros QCA9377')
  '168C:0036' = @('qcom', 'Qualcomm Atheros QCA9565')
  '168C:0034' = @('qcom', 'Qualcomm Atheros AR9462')
  '168C:0032' = @('qcom', 'Qualcomm Atheros AR9485')
  # Broadcom (Linux brcmfmac/b43/wl).
  '14E4:43A0' = @('bcm', 'Broadcom BCM4360')
  '14E4:43B1' = @('bcm', 'Broadcom BCM4352')
  '14E4:43A3' = @('bcm', 'Broadcom BCM4350')
  '14E4:43BA' = @('bcm', 'Broadcom BCM43602')
  '14E4:4464' = @('bcm', 'Broadcom BCM4364')
  '14E4:4331' = @('bcm', 'Broadcom BCM4331')
  '14E4:4353' = @('bcm', 'Broadcom BCM43224')
  '14E4:4357' = @('bcm', 'Broadcom BCM43225')
}
# kind -> what the report says, and a hint for the reader (may be empty).
$WifiKinds = @{
  'supported'     = @('supported by the AX210 developer preview', '')
  'intel-preview' = @('outside the AX210 developer preview', 'The preview requires Intel AX210 (8086:2725).')
  'intel-fw'      = @('not supported: its firmware is not shipped upstream', 'Airdraft runs the Intel cards whose firmware ForbiddenItlwm ships (API 68); this one has none there.')
  'intel-be'      = @('not supported: Intel Wi-Fi 7, no upstream driver', 'ForbiddenItlwm, which Airdraft ports, has no driver for Wi-Fi 7 cards yet.')
  'intel-other'   = @('not supported: an Intel device ID Airdraft does not know', 'Please post this report in an issue: if upstream ships firmware for this card, its ID can be added.')
  'rtl'           = @('outside the AX210 developer preview', '')
  'rtl-be-experimental' = @('experimental development target', 'The standard preview binds Intel AX210 only. The RTL8852BE radio requires a separate development build and hardware qualification.')
  'mtk'           = @('not supported: MediaTek', 'No user-mode Windows driver exists for MediaTek Wi-Fi yet.')
  'qcom'          = @('not supported: Qualcomm', 'No user-mode Windows driver exists for Qualcomm Wi-Fi yet.')
  'bcm'           = @('not supported: Broadcom', 'There is no open Windows driver path for Broadcom Wi-Fi.')
  'usb'           = @('not supported: USB adapter', 'Airdraft drives PCIe cards only.')
  'other'         = @('not supported: unrecognized wireless adapter', '')
}
$WifiVendors = @{ '8086' = 'Intel'; '10EC' = 'Realtek'; '14C3' = 'MediaTek'; '17CB' = 'Qualcomm'; '168C' = 'Qualcomm Atheros'; '14E4' = 'Broadcom'
                  '0BDA' = 'Realtek'; '0E8D' = 'MediaTek'; '148F' = 'MediaTek (Ralink)'; '0CF3' = 'Qualcomm Atheros'; '8087' = 'Intel' }

function Classify-WifiCard([string]$InstanceId, [string]$FriendlyName = '') {
  $id = "$InstanceId".ToUpper()
  $parts = $id -split '\\'
  $bus = $parts[0]
  $vendor = ''; $device = ''
  if ($id -match '^PCI\\VEN_([0-9A-F]{4})&DEV_([0-9A-F]{4})(?:&|\\|$)') { $vendor = $Matches[1]; $device = $Matches[2] }
  elseif ($id -match '^USB\\VID_([0-9A-F]{4})&PID_([0-9A-F]{4})(?:&|\\|$)') { $vendor = $Matches[1]; $device = $Matches[2] }
  $kind = 'other'; $chip = ''
  if ($bus -eq 'USB') { $kind = 'usb' }
  elseif ($bus -eq 'PCI' -and $vendor) {
    $key = $vendor + ':' + $device
    if ($key -eq '8086:2725') { $kind = 'supported'; $chip = 'Intel AX210' }
    elseif ($vendor -eq '8086' -and $KnownIntelIds -contains $device) { $kind = 'intel-preview'; $chip = 'Intel Wi-Fi 6/6E' }
    elseif ($WifiChips.ContainsKey($key)) { $kind = $WifiChips[$key][0]; $chip = $WifiChips[$key][1] }
    else {
      # Only reached for a device already known to be wireless (by its medium
      # or name), so the vendor is enough to say which family it is.
      $byVendor = @{ '8086' = 'intel-other'; '10EC' = 'rtl'; '14C3' = 'mtk'; '17CB' = 'qcom'; '168C' = 'qcom'; '14E4' = 'bcm' }
      if ($byVendor.ContainsKey($vendor)) { $kind = $byVendor[$vendor] }
    }
  }
  $vendorName = if ($WifiVendors.ContainsKey($vendor)) { $WifiVendors[$vendor] } elseif ($vendor) { "vendor $vendor" } else { 'unknown vendor' }
  if (-not $chip) { $chip = "$vendorName $device (not in Airdraft's table)" }
  return [pscustomobject]@{
    Bus = $bus; Vendor = $vendorName; VendorId = $vendor; DeviceId = $device
    HardwareId = $(if ($parts.Count -gt 1) { $parts[1] } else { $id })
    Chip = $chip; Family = $chip; Kind = $kind; Status = $WifiKinds[$kind][0]; Note = $WifiKinds[$kind][1]
  }
}
# A known Wi-Fi chip by its PCI ID (supported or not).
function Test-KnownWifiId([string]$InstanceId) {
  $c = Classify-WifiCard $InstanceId
  if ($c.Bus -ne 'PCI') { return $false }
  return ($c.Kind -in 'supported', 'intel-preview' -or $WifiChips.ContainsKey($c.VendorId + ':' + $c.DeviceId))
}

# ------------------------------------------------------ the refusal message
$Ax210Pattern = '^PCI\\VEN_8086&DEV_2725(?:&|\\|$)'
$Rtl8852bePattern = '^PCI\\VEN_10EC&DEV_(?:B852|B85B)(?:&|\\|$)'
$NoSupportedCard = 'This developer preview needs exactly one Intel AX210 PCIe card (8086:2725).'
$ReportCommand = 'powershell -ExecutionPolicy Bypass -File report.ps1'

function Resolve-DualRadio([object[]]$Devices, [string]$Requested, [string]$Installed) {
  $choices = @(
    if (@($Devices | Where-Object InstanceId -Match $Ax210Pattern).Count -eq 1) { 'AX210' }
    if (@($Devices | Where-Object InstanceId -Match $Rtl8852bePattern).Count -eq 1) { 'RTL8852BE' }
  )
  if ($Installed -and $Requested -and $Installed -ne $Requested) {
    throw 'Uninstall Airdraft before changing which Wi-Fi adapter it uses.'
  }
  $wanted = if ($Installed) { $Installed } else { $Requested }
  if ($wanted) {
    if ($wanted -notin $choices) { throw "Setup needs exactly one $wanted PCIe adapter." }
    return $wanted
  }
  if ($choices.Count -eq 1) { return $choices[0] }
  if ($choices.Count -eq 0) {
    throw 'Setup needs one supported PCIe adapter. Supported models are Intel AX210 (8086:2725) and Realtek RTL8852BE (10EC:B852 or 10EC:B85B).'
  }
  throw 'Both adapter types are present. Run install.ps1 as administrator with -Adapter AX210 or -Adapter RTL8852BE.'
}
# Adapter names that say wireless. As report.ps1: not bare "Killer" (its
# Ethernet chips E2600/E3100 use it too). The CJK words are \u escapes so this
# file stays ASCII: Windows PowerShell 5.1 reads a file without a BOM in the
# ANSI code page.
$WifiNamePattern = 'Wi-?Fi|Wireless|802\.11|WLAN|RZ[67]\d\d|FastConnect|Killer.*(?:AX|BE|1[4567]\d\d)|\u65E0\u7EBF|\u7121\u7DDA'

# The wireless cards present, up to 5, PCIe first: report.ps1's three sources
# (adapters whose medium is native 802.11, known Wi-Fi chip IDs, adapter
# names), physical devices only. Takes the device and adapter lists instead of
# reading them, so -SelfTest can feed made-up ones.
function Select-WirelessDevices($Pnp, $Adapters) {
  $candidates = @()
  foreach ($a in @($Adapters | Where-Object { $_ })) {
    if ($a.PnPDeviceID -and ($a.NdisPhysicalMedium -eq 9 -or "$($a.PhysicalMediaType)" -match '802\.11|Wireless')) {
      $candidates += @($Pnp | Where-Object { $_.InstanceId -eq $a.PnPDeviceID })
    }
  }
  $candidates += @($Pnp | Where-Object { $_ -and (Test-KnownWifiId $_.InstanceId) })
  $candidates += @($Pnp | Where-Object { $_ -and $_.Class -eq 'Net' -and "$($_.FriendlyName)" -match $WifiNamePattern })
  $seen = @{}
  $found = foreach ($d in $candidates) {
    if (-not $d -or "$($d.InstanceId)" -notmatch '^(PCI|USB)\\') { continue }
    $k = "$($d.InstanceId)".ToUpper()
    if ($seen.ContainsKey($k)) { continue }
    $seen[$k] = $true
    $d
  }
  return @($found | Where-Object { $_ } | Sort-Object { if ($_.InstanceId -match '^PCI\\') { 0 } else { 1 } } | Select-Object -First 5)
}

# Text as is if it is printable ASCII, else ''. The setup reads
# install-error.txt as an ANSI string: a device name in Chinese or with a
# trademark sign would come out garbled, so such a name is left out (the chip
# name from the table says what the card is anyway).
function ConvertTo-AsciiText([string]$Text) {
  if ("$Text" -cmatch '^[\x20-\x7E]*$') { return "$Text" }
  return ''
}

# Show detected cards and their support status without including device instance paths.
function Format-UnsupportedCardMessage($Devices) {
  $lines = @($NoSupportedCard)
  $devs = @($Devices | Where-Object { $_ })
  if ($devs.Count) {
    $lines += 'Wireless card(s) found on this PC:'
    foreach ($d in $devs) {
      $c = Classify-WifiCard $d.InstanceId
      $id = if (-not $c.VendorId) { $c.Bus }
            elseif ($c.Bus -eq 'USB') { "USB\VID_$($c.VendorId)&PID_$($c.DeviceId)" }
            else { "$($c.VendorId):$($c.DeviceId), PCI\VEN_$($c.VendorId)&DEV_$($c.DeviceId)" }
      $id = ConvertTo-AsciiText $id
      $name = (ConvertTo-AsciiText $d.FriendlyName).Trim()
      if (-not $name) { $name = (ConvertTo-AsciiText $d.Name).Trim() }
      $label = if ($name) { "$name ($id)" } else { $id }
      $lines += "- $($label): $($c.Chip), $($c.Status)."
      if ($c.Note) {
        $lines += "  $($c.Note)"
      }
    }
  } else {
    $lines += 'No wireless card was found on this PC (a card turned off in the BIOS or by a hardware switch does not show up).'
  }
  $lines += 'To collect diagnostics, run this command from the extracted preview folder:'
  $lines += "  $ReportCommand"
  # Every line is ASCII by construction; this only guards against a future edit.
  return ($lines | ForEach-Object { ConvertTo-AsciiText $_ }) -join "`r`n"
}

if ($SelfTest) {
  $fail = @()
  function Dev([string]$id, [string]$name = '', [string]$class = 'Net') {
    [pscustomobject]@{ InstanceId = $id; FriendlyName = $name; Name = $name; Class = $class }
  }
  function Nic([string]$pnpId, $medium, [string]$mediaType = '') {
    [pscustomobject]@{ PnPDeviceID = $pnpId; NdisPhysicalMedium = $medium; PhysicalMediaType = $mediaType }
  }
  function Ascii([string]$s) { return ("$s" -cmatch '^[\x09\x0A\x0D\x20-\x7E]*$') }

  foreach ($device in $KnownIntelIds) {
    $id = "PCI\VEN_8086&DEV_$device&SUBSYS_00748086\3&1"
    $ax210 = $device -eq '2725'
    $kind = if ($ax210) { 'supported' } else { 'intel-preview' }
    if (($id -match $Ax210Pattern) -ne $ax210 -or ($id -match $Rtl8852bePattern) -or
        (Classify-WifiCard $id).Kind -ne $kind) { $fail += "8086:$device classified incorrectly" }
  }
  foreach ($suffix in '', '\4&1', '&SUBSYS_B85217AA') {
    if ("pci\ven_10ec&dev_b852$suffix" -notmatch $Rtl8852bePattern -or
        "pci\ven_10ec&dev_b85b$suffix" -notmatch $Rtl8852bePattern -or
        "pci\ven_8086&dev_2725$suffix" -notmatch $Ax210Pattern) { $fail += "valid device ID rejected with suffix '$suffix'" }
  }
  foreach ($id in 'PCI\VEN_8086&DEV_27250', 'PCI\VEN_10EC&DEV_B8520', 'PCI\VEN_10EC&DEV_B85B0', 'PCI\VEN_10EC&DEV_C852', 'PCI\VEN_10EC&DEV_8168') {
    if ($id -match $Ax210Pattern -or $id -match $Rtl8852bePattern) { $fail += "$id accepted as supported" }
  }
  if (Test-KnownWifiId 'PCI\VEN_8086&DEV_43F01&SUBSYS_00748086') { $fail += '5-digit device ID DEV_43F01 matched as known' }
  if (Test-KnownWifiId 'PCI\VEN_10EC&DEV_C8520&SUBSYS_C85210EC') { $fail += '5-digit device ID DEV_C8520 matched as known' }

  $intel = Dev 'PCI\VEN_8086&DEV_2725&SUBSYS_00008086\1'
  $realtek = Dev 'PCI\VEN_10EC&DEV_B85B&SUBSYS_B85B17AA\2'
  if ((Resolve-DualRadio @($intel) '' '') -ne 'AX210' -or
      (Resolve-DualRadio @($realtek) '' '') -ne 'RTL8852BE' -or
      (Resolve-DualRadio @($intel, $realtek) 'RTL8852BE' '') -ne 'RTL8852BE' -or
      (Resolve-DualRadio @($intel, $realtek) '' 'AX210') -ne 'AX210') {
    $fail += 'combined Setup did not select the only, requested or existing radio'
  }
  try { Resolve-DualRadio @($intel, $realtek) '' '' | Out-Null; $fail += 'combined Setup guessed between radio types' }
  catch { if ($_.Exception.Message -notmatch '-Adapter') { $fail += 'combined Setup omitted the adapter choice' } }
  try { Resolve-DualRadio @($intel, $realtek) 'RTL8852BE' 'AX210' | Out-Null; $fail += 'combined Setup switched the existing radio' }
  catch { if ($_.Exception.Message -notmatch 'Uninstall') { $fail += 'combined Setup omitted the existing radio restriction' } }
  try { Resolve-DualRadio @($realtek, (Dev 'PCI\VEN_10EC&DEV_B852\3')) 'RTL8852BE' '' | Out-Null; $fail += 'combined Setup accepted two Realtek radios' }
  catch { if ($_.Exception.Message -notmatch 'exactly one RTL8852BE') { $fail += 'combined Setup omitted the ambiguous Realtek count' } }

  # A laptop with an unsupported Realtek RTL8852BE that has no driver yet (so no
  # adapter and no class), its Realtek Ethernet chip, a Wi-Fi Direct virtual adapter and TAP.
  $rtlPnp = @(
    (Dev 'PCI\VEN_10EC&DEV_B852&SUBSYS_B85217AA&REV_00\4&1&0&00E8' '' ''),
    (Dev 'PCI\VEN_10EC&DEV_8168&SUBSYS_86771043&REV_15\4&2&0&00E5' 'Realtek PCIe GbE Family Controller'),
    (Dev 'SWD\MSRRAS\MS_NDISWANIP' 'WAN Miniport (IP)'),
    (Dev 'ROOT\NET\0000' 'Microsoft Wi-Fi Direct Virtual Adapter'),
    (Dev 'ROOT\NET\0001' 'TAP-Windows Adapter V9')
  )
  $rtlNics = @((Nic 'PCI\VEN_10EC&DEV_8168&SUBSYS_86771043&REV_15\4&2&0&00E5' 14 '802.3'), (Nic 'ROOT\NET\0000' 9 'Native 802.11'))
  $sel = @(Select-WirelessDevices $rtlPnp $rtlNics)
  if ($sel.Count -ne 1 -or $sel[0].InstanceId -notmatch 'DEV_B852') { $fail += "RTL8852BE laptop: selected $(@($sel | ForEach-Object { $_.InstanceId }) -join ', ')" }
  $msg = Format-UnsupportedCardMessage $sel
  foreach ($want in $NoSupportedCard, '10EC:B852', 'PCI\VEN_10EC&DEV_B852', 'Realtek RTL8852BE', 'experimental development target', 'RTL8852BE radio requires a separate development build', $ReportCommand) {
    if (-not $msg.Contains($want)) { $fail += "RTL8852BE message lacks '$want'" }
  }
  if ($msg.Contains('8168') -or $msg.Contains('SUBSYS') -or $msg.Contains('4&1&0')) { $fail += 'RTL8852BE message shows the Ethernet chip or the instance path' }

  # MediaTek RZ616 found by its medium, a USB stick (serial = MAC address) found
  # by name, Intel Meteor Lake and BE200, an Intel ID nobody knows, Qualcomm,
  # Broadcom and an unknown vendor's wireless adapter: PCIe first, at most 5.
  $manyPnp = @(
    (Dev 'USB\VID_0BDA&PID_8812\00E04C000001' 'Realtek 8812AU Wireless LAN 802.11ac USB NIC'),
    (Dev 'PCI\VEN_14C3&DEV_0616&SUBSYS_E0CD1A3B&REV_00\4&1' 'MediaTek Wi-Fi 6E MT7922 (RZ616) 160MHz PCIe Adapter'),
    (Dev 'PCI\VEN_8086&DEV_7E40&SUBSYS_00908086&REV_20\3&1' 'Intel(R) Wi-Fi 6E AX211 160MHz'),
    (Dev 'PCI\VEN_8086&DEV_272B&SUBSYS_00F48086&REV_1A\4&1' 'Intel(R) Wi-Fi 7 BE200 320MHz'),
    (Dev 'PCI\VEN_8086&DEV_1234&SUBSYS_00008086\4&1' 'Intel(R) Wireless Something'),
    (Dev 'PCI\VEN_17CB&DEV_1103&SUBSYS_010817CB\4&1' 'Qualcomm FastConnect 6900 Wi-Fi 6E Dual Band Simultaneous (DBS) WiFi Network Adapter'),
    (Dev 'PCI\VEN_14E4&DEV_43A0&SUBSYS_0000106B\4&1' 'Broadcom 802.11ac Network Adapter')
  )
  $manyNics = @((Nic 'PCI\VEN_14C3&DEV_0616&SUBSYS_E0CD1A3B&REV_00\4&1' 9))
  $sel = @(Select-WirelessDevices $manyPnp $manyNics)
  if ($sel.Count -ne 5) { $fail += "expected 5 wireless devices, got $($sel.Count)" }
  if (@($sel | Where-Object { $_.InstanceId -match '^USB\\' }).Count) { $fail += 'a USB adapter came before 5 PCIe cards' }
  $all = @(Select-WirelessDevices ($manyPnp | Select-Object -First 3) @())
  if ($all.Count -ne 3 -or $all[-1].InstanceId -notmatch '^USB\\') { $fail += 'the USB adapter is not listed after the PCIe cards' }
  $msg = Format-UnsupportedCardMessage $all
  foreach ($want in 'USB\VID_0BDA&PID_8812', 'not supported: USB adapter', 'MediaTek MT7922 (AMD RZ616)', 'not supported: MediaTek', 'Intel Ma (Meteor Lake CNVi)', 'firmware is not shipped upstream') {
    if (-not $msg.Contains($want)) { $fail += "message lacks '$want'" }
  }
  if ($msg.Contains('00E04C000001')) { $fail += 'message shows the USB serial number' }
  if ($msg.Contains('winawdl')) { $fail += 'winawdl offered for a card it does not drive' }
  $msg = Format-UnsupportedCardMessage ($manyPnp | Select-Object -Skip 3)
  foreach ($want in 'Intel BE200 (Wi-Fi 7)', 'an Intel device ID Airdraft does not know', 'Qualcomm WCN6855', 'Broadcom BCM4360') {
    if (-not $msg.Contains($want)) { $fail += "message lacks '$want'" }
  }
  $odd = @(Select-WirelessDevices @((Dev 'PCI\VEN_1AE0&DEV_1234&SUBSYS_00001AE0\4&1' 'Contoso Wireless-AC 9000')) @())
  if ($odd.Count -ne 1 -or -not (Format-UnsupportedCardMessage $odd).Contains('unrecognized wireless adapter')) { $fail += 'unknown vendor wireless adapter not reported' }

  # Ethernet only (Intel and Realtek chips, Killer E3100): no wireless card.
  $ethPnp = @((Dev 'PCI\VEN_8086&DEV_15F3&SUBSYS_00008086\3&1' 'Intel(R) Ethernet Controller I225-V'),
              (Dev 'PCI\VEN_10EC&DEV_8125&SUBSYS_00001043\4&1' 'Killer E3100 2.5 Gigabit Ethernet Controller'),
              (Dev 'PCI\VEN_10EC&DEV_8168&SUBSYS_00001043\4&2' 'Realtek PCIe GbE Family Controller'))
  $ethNics = @((Nic 'PCI\VEN_8086&DEV_15F3&SUBSYS_00008086\3&1' 14 '802.3'))
  if (@(Select-WirelessDevices $ethPnp $ethNics).Count) { $fail += 'an Ethernet chip was taken for Wi-Fi' }
  $msg = Format-UnsupportedCardMessage @()
  if (-not $msg.StartsWith($NoSupportedCard) -or -not $msg.Contains('No wireless card was found') -or -not $msg.Contains($ReportCommand)) { $fail += 'no-card message incomplete' }
  if (-not (Format-UnsupportedCardMessage $null).Contains('No wireless card was found')) { $fail += 'null device list not handled' }

  # A localized name (Chinese) and a trademark sign: left out, chip still named,
  # and the whole message stays ASCII for the setup's ANSI error box.
  $cjk = Dev 'PCI\VEN_14C3&DEV_7922&SUBSYS_7922103C\4&1' ('MediaTek ' + [char]0x65E0 + [char]0x7EBF + ' Wi-Fi')
  $tm = Dev 'PCI\VEN_10EC&DEV_B852&SUBSYS_1\4&1' ('Realtek' + [char]0x00AE + ' 8852BE WiFi 6 PCI-E NIC')
  if (@(Select-WirelessDevices @((Dev 'PCI\VEN_1AE0&DEV_0001\4&1' ('Contoso ' + [char]0x65E0 + [char]0x7EBF))) @()).Count -ne 1) { $fail += 'a Chinese wireless adapter name was not recognized' }
  $msg = Format-UnsupportedCardMessage @($cjk, $tm)
  if (-not $msg.Contains('MediaTek MT7922') -or -not $msg.Contains('Realtek RTL8852BE') -or -not $msg.Contains('PCI\VEN_10EC&DEV_B852')) { $fail += 'a card with a non-ASCII name lost its chip or ID' }
  foreach ($m in $msg, (Format-UnsupportedCardMessage $manyPnp), (Format-UnsupportedCardMessage $rtlPnp)) {
    if (-not (Ascii $m)) { $fail += 'message is not plain ASCII' }
    foreach ($ln in ($m -split "`r?`n")) {
      if ($ln -match 'https?://' -and $ln.Length -gt 79) { $fail += "URL line exceeds 79 chars: $ln" }
    }
  }
  foreach ($k in $WifiKinds.Keys) {
    if (-not (Ascii ($WifiKinds[$k] -join ' '))) { $fail += "kind $k text is not ASCII" }
  }

  # The tables and functions are a copy of report.ps1's, and the supported IDs
  # are restore.ps1's too: compare them when those files sit beside this one
  # (the build's stage folder and the repository's installer folder).
  function Get-ScriptAst([string]$path) {
    [System.Management.Automation.Language.Token[]]$tokens = $null
    [System.Management.Automation.Language.ParseError[]]$errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
    if ($errors -and $errors.Count) { return $null }
    return $ast
  }
  # A top-level variable's value, only if it is pure literals (strings, arrays,
  # hashtables): evaluating it then runs no command.
  function Get-LiteralValue($ast, [string]$name) {
    $a = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq "`$$name" }, $false)
    if (-not $a) { return $null }
    $code = $a.Right.Find({ param($n) $n -is [System.Management.Automation.Language.CommandAst] -or
                                      $n -is [System.Management.Automation.Language.VariableExpressionAst] -or
                                      $n -is [System.Management.Automation.Language.InvokeMemberExpressionAst] -or
                                      $n -is [System.Management.Automation.Language.ExpandableStringExpressionAst] }, $true)
    if ($code) { return $null }
    return (& ([scriptblock]::Create($a.Right.Extent.Text)))
  }
  function ConvertTo-Comparable($v) {
    if ($v -is [hashtable]) { return (($v.Keys | Sort-Object | ForEach-Object { "$_=" + (@($v[$_]) -join '|') }) -join "`n") }
    return (@($v) -join '|')
  }
  $self = Get-ScriptAst $PSCommandPath
  if (-not $self) { $fail += 'install.ps1 does not parse' }
  $report = Join-Path $PSScriptRoot 'report.ps1'
  if ($self -and (Test-Path -LiteralPath $report)) {
    $other = Get-ScriptAst $report
    if (-not $other) { $fail += 'report.ps1 does not parse' }
    else {
      foreach ($v in 'KnownIntelIds', 'WifiChips', 'WifiKinds', 'WifiVendors') {
        $mine = Get-LiteralValue $self $v; $theirs = Get-LiteralValue $other $v
        if ($null -eq $theirs -or (ConvertTo-Comparable $mine) -ne (ConvertTo-Comparable $theirs)) { $fail += "`$$v differs from report.ps1's" }
      }
    }
  } else { Write-Host 'install.ps1 self-test: report.ps1 not beside it, table comparison skipped' }
  $restore = Join-Path $PSScriptRoot 'restore.ps1'
  if ($self -and (Test-Path -LiteralPath $restore)) {
    $other = Get-ScriptAst $restore
    $theirs = if ($other) { Get-LiteralValue $other 'SupportedIntelIds' } else { $null }
    if ($null -eq $theirs -or (ConvertTo-Comparable $KnownIntelIds) -ne (ConvertTo-Comparable $theirs)) { $fail += "restore.ps1's `$SupportedIntelIds differ" }
  } else { Write-Host 'install.ps1 self-test: restore.ps1 not beside it, ID comparison skipped' }

  if ($fail) { $fail | ForEach-Object { Write-Host "FAIL $_" }; exit 1 }
  Write-Host 'install.ps1 self-test: card check, wireless selection, refusal message and card tables passed'
  exit 0
}

# --------------------------------------------------------------- elevation
# Preflight can inspect the package and adapter without changing Windows.
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
  [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and !$DryRun) {
  Write-Host 'Run Airdraft Setup or run this script from an administrator PowerShell.'
  exit 1
}

$Source = $PSScriptRoot
$variantFile = 'RTL8852BE.experimental'
$bundleFile = 'Airdraft.DualRadio'
$combined = Test-Path -LiteralPath (Join-Path $Source $bundleFile) -PathType Leaf
$experimental = Test-Path -LiteralPath (Join-Path $Source $variantFile) -PathType Leaf
if ($combined -and $experimental) { throw 'Setup has conflicting radio package markers.' }
if (([int](Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuild -lt 22000) -or
    $env:PROCESSOR_ARCHITECTURE -ne 'AMD64') { throw 'Airdraft requires Windows 11 x64.' }
$pnpAll = @(Get-PnpDevice -PresentOnly)
$Target = [IO.Path]::GetFullPath($Target)
if (!$DryRun) {
  $setupMutex = [Threading.Mutex]::new($false, 'Global\Airdraft.Setup')
  try { $ownsSetupMutex = $setupMutex.WaitOne(0) }
  catch [Threading.AbandonedMutexException] { $ownsSetupMutex = $true }
  if (!$ownsSetupMutex) { throw 'Another Airdraft Setup is running.' }
}
$Service = 'AirdraftRadioService'
# Version 0.1.0 registered the radio service with its upstream name.
$legacyService = 'NekoDropService'
$legacyExe = Join-Path $Target "$legacyService.exe"
$serviceRoot = 'HKLM:\SYSTEM\CurrentControlSet\Services'
$existing = Get-ItemProperty -LiteralPath (Join-Path $serviceRoot $Service) -ErrorAction SilentlyContinue
$legacy = Get-ItemProperty -LiteralPath (Join-Path $serviceRoot $legacyService) -ErrorAction SilentlyContinue
if ($existing -and $existing.ImagePath.Trim('"') -ine (Join-Path $Target "$Service.exe")) {
  throw 'Airdraft is already installed in another directory. Run Setup for that installation first.'
}
$installedExperiment = Test-Path -LiteralPath (Join-Path $Target $variantFile) -PathType Leaf
if ($combined) {
  $installedRadio = if ($installedExperiment) { 'RTL8852BE' } elseif ($existing -or $legacy) { 'AX210' }
  $experimental = (Resolve-DualRadio $pnpAll $Adapter $installedRadio) -eq 'RTL8852BE'
} else {
  $availableRadio = if ($experimental) { 'RTL8852BE' } else { 'AX210' }
  if ($Adapter -and $Adapter -ne $availableRadio) { throw "This Setup package supports $availableRadio." }
  if ($experimental) {
    if (@($pnpAll | Where-Object InstanceId -Match $Rtl8852bePattern).Count -ne 1) {
      throw 'The Realtek build needs exactly one RTL8852BE PCIe card (10EC:B852 or 10EC:B85B).'
    }
  } elseif (@($pnpAll | Where-Object InstanceId -Match $Ax210Pattern).Count -ne 1) {
    throw (Format-UnsupportedCardMessage (Select-WirelessDevices $pnpAll @()))
  }
}
if (($installedExperiment -and !$experimental) -or ($existing -and $installedExperiment -ne $experimental) -or
    ($legacy -and $experimental)) {
  throw 'Uninstall the current Airdraft preview before switching between AX210 and RTL8852BE.'
}
if ($legacy -and ($legacy.ImagePath.Trim('"') -ine $legacyExe -or $legacy.DisplayName -ne 'Airdraft AirDrop')) {
  throw 'Another application has registered a radio service for this adapter. Uninstall it before installing Airdraft.'
}
$sourceDriver = if ($combined -and $experimental) { 'driver-rtl8852be' } else { 'driver' }
$driverFiles = 'winawdl_regproxy.inf', 'winawdl_regproxy.cat', 'winawdl_regproxy.dll'
$radioFiles = if ($experimental) {
  @('airdraft-radio-rtl8852be.exe')
} else {
  @('airdraft-radio.exe', 'firmware\iwlwifi-ty-a0-gf-a0-68.ucode', 'firmware\iwlwifi-ty-a0-gf-a0.pnvm')
}
$payload = @('Airdraft.exe', 'AirdraftRadioService.exe', 'awdl\awdld.exe', 'awdl\wintun.dll', 'python\python.exe',
             'airdrop\engine.py', 'scripts\airdrop-beacon.ps1', 'restore.cmd', 'restore.ps1', 'report.cmd', 'report.ps1', 'uninstall.ps1')
$payload += @($driverFiles | ForEach-Object { Join-Path $sourceDriver $_ })
$payload += @($radioFiles | ForEach-Object { Join-Path 'radio' $_ })
if (!$experimental) { $payload += 'setup-tap.ps1', 'tap-windows.exe' }
$missing = @($payload | Where-Object { !(Test-Path -LiteralPath (Join-Path $Source $_) -PathType Leaf) })
if ($missing.Count) { throw "Airdraft Setup is incomplete. Missing files: $($missing -join ', ')." }
if ($DryRun) {
  'Airdraft preflight passed.'
  if ($experimental) { 'Selected radio: RTL8852BE' } else { 'Selected radio: AX210' }
  return
}

# The setup runs this hidden: keep a log, and say what failed through the exit
# code and install-error.txt, which the setup shows.
$LogDir = Join-Path $env:ProgramData 'Airdraft'
New-Item -ItemType Directory -Force $LogDir | Out-Null
if ((Get-Item -LiteralPath $LogDir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
  throw "Refusing reparse point: $LogDir"
}
& icacls.exe $LogDir /inheritance:r /grant:r "*S-1-5-18:(OI)(CI)F" "*S-1-5-32-544:(OI)(CI)F" | Out-Null
Remove-Item (Join-Path $LogDir 'install-error.txt') -ErrorAction SilentlyContinue
Start-Transcript (Join-Path $LogDir 'install.log') -Force | Out-Null
$loggingSetup = $true
trap {
  $failure = $_
  $rollbackFailure = $null
  if ($setupWorkspace) {
    try {
      if (!$setupCommitted) { Undo-SetupUpgrade }
      Remove-SetupWorkspace $setupWorkspace $setupParent
    } catch {
      $rollbackFailure = "Setup recovery could not finish. The working files are at $setupWorkspace. $($_.Exception.Message)"
    }
  }
  if ($loggingSetup) {
    @($failure | Out-String; if ($rollbackFailure) { $rollbackFailure }) |
      Set-Content (Join-Path $LogDir 'install-error.txt') -Encoding utf8 -ErrorAction Continue
    Stop-Transcript -ErrorAction SilentlyContinue | Out-Null
  }
  if ($setupMutex) {
    if ($ownsSetupMutex) { $setupMutex.ReleaseMutex() }
    $setupMutex.Dispose()
  }
  Write-Error -ErrorRecord $failure -ErrorAction Continue
  if ($rollbackFailure) { Write-Error $rollbackFailure -ErrorAction Continue }
  exit 1
}
$CertSubject = 'CN=Airdraft driver (signed on this PC)'
$TrustStores = 'Root', 'TrustedPublisher'
$FirewallGroup = 'Airdraft AirDrop'

function Step($what, [scriptblock]$do) {
  "  $what"
  if (-not $DryRun) { & $do }
}

function Assert-SetupDirectory([string]$Path) {
  $item = Get-Item -LiteralPath $Path -Force
  if (!$item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw "Setup expected a regular directory at $Path."
  }
}

function Add-SetupDirectory([string]$Path, $Created) {
  if (Test-Path -LiteralPath $Path) {
    Assert-SetupDirectory $Path
  } else {
    New-Item -ItemType Directory -Path $Path | Out-Null
    $Created.Add($Path)
  }
}

function Publish-SetupFiles([string]$Staged, [string]$Destination, [string]$Backup, $Changes, $Created) {
  $root = (Resolve-Path -LiteralPath $Staged).ProviderPath.TrimEnd('\') + '\'
  $entries = @(Get-ChildItem -LiteralPath $Staged -Force -Recurse)
  if ($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } | Select-Object -First 1) {
    throw 'Setup files cannot contain reparse points.'
  }
  Add-SetupDirectory $Destination $Created
  foreach ($directory in $entries | Where-Object PSIsContainer | Sort-Object FullName) {
    Add-SetupDirectory (Join-Path $Destination $directory.FullName.Substring($root.Length)) $Created
  }
  foreach ($file in $entries | Where-Object { !$_.PSIsContainer } | Sort-Object FullName) {
    $relative = $file.FullName.Substring($root.Length)
    $path = Join-Path $Destination $relative
    $saved = Join-Path $Backup $relative
    $previous = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    if ($previous -and ($previous.PSIsContainer -or ($previous.Attributes -band [IO.FileAttributes]::ReparsePoint))) {
      throw "Setup expected a regular file at $path."
    }
    $change = [pscustomobject]@{ Path = $path; Saved = $saved; New = !$previous; Published = $false }
    $Changes.Add($change)
    if ($previous) {
      [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($saved))
      [IO.File]::Replace($file.FullName, $path, $saved)
    } else {
      [IO.File]::Move($file.FullName, $path)
    }
    $change.Published = $true
  }
}

function Undo-SetupFiles($Changes, $Created) {
  $errors = [Collections.Generic.List[string]]::new()
  for ($position = $Changes.Count - 1; $position -ge 0; $position--) {
    $change = $Changes[$position]
    try {
      if ([IO.File]::Exists($change.Saved)) {
        if ([IO.File]::Exists($change.Path)) {
          [IO.File]::Replace($change.Saved, $change.Path, [NullString]::Value)
        } else {
          [IO.File]::Move($change.Saved, $change.Path)
        }
      } elseif ($change.Published -and $change.New) {
        [IO.File]::Delete($change.Path)
      }
    } catch {
      $errors.Add("Could not restore $($change.Path). $($_.Exception.Message)")
    }
  }
  for ($position = $Created.Count - 1; $position -ge 0; $position--) {
    $directory = $Created[$position]
    try {
      if ((Test-Path -LiteralPath $directory -PathType Container) -and
          !(Get-ChildItem -LiteralPath $directory -Force | Select-Object -First 1)) {
        [IO.Directory]::Delete($directory)
      }
    } catch {
      $errors.Add("Could not remove the directory $directory. $($_.Exception.Message)")
    }
  }
  if ($errors.Count) { throw ($errors -join [Environment]::NewLine) }
}

function Remove-SetupWorkspace([string]$Path, [string]$Parent) {
  if (!$Path -or !(Test-Path -LiteralPath $Path -PathType Container)) { return }
  $checked = (Resolve-Path -LiteralPath $Path).ProviderPath
  if ([IO.Path]::GetDirectoryName($checked) -ine $Parent -or
      [IO.Path]::GetFileName($checked) -notmatch '^\.AirdraftSetup-[0-9a-f]{32}$') {
    throw 'Setup refused to remove a working directory outside its install parent.'
  }
  Assert-SetupDirectory $checked
  if (Get-ChildItem -LiteralPath $checked -Force -Recurse |
      Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } | Select-Object -First 1) {
    throw 'Setup refused to remove a working directory containing reparse points.'
  }
  Remove-Item -LiteralPath $checked -Recurse -Force
}

function Undo-SetupUpgrade {
  if ($startedNewPayload) {
    $newService = Get-Service -Name $Service -ErrorAction SilentlyContinue
    if ($newService -and $newService.Status -ne 'Stopped') {
      Stop-SetupService $Service
      & (Join-Path $Target "$Service.exe") --give-back
      if ($LASTEXITCODE -ne 0) { throw 'The new radio service could not give the adapter back to Windows.' }
    }
    if ($createdRadioService) {
      sc.exe delete $Service | Out-Null
      if ($LASTEXITCODE -ne 0) { throw 'Setup could not remove its new service registration.' }
    }
  }
  Undo-SetupFiles $fileChanges $createdDirectories
  if ($startedNewPayload -and $existing) {
    $startupType = @{ '2' = 'Automatic'; '3' = 'Manual'; '4' = 'Disabled' }[[string]$existing.Start]
    if ($startupType) { Set-Service -Name $Service -StartupType $startupType }
    if ($startupType -eq 'Automatic' -and $existing.DelayedAutoStart -eq 1) {
      sc.exe config $Service start= delayed-auto | Out-Null
      if ($LASTEXITCODE -ne 0) { throw 'Setup could not restore the previous delayed start setting.' }
    }
  }
  if ($nativeVerified) {
    foreach ($name in $previousRunning) { Start-Service -Name $name }
  }
}

$fileChanges = [Collections.Generic.List[object]]::new()
$createdDirectories = [Collections.Generic.List[string]]::new()
$setupParent = [IO.Path]::GetDirectoryName($Target.TrimEnd('\'))
if (!$setupParent) { throw 'Choose an application folder beneath a drive root.' }
New-Item -ItemType Directory -Path $setupParent -Force | Out-Null
$setupParent = (Resolve-Path -LiteralPath $setupParent).ProviderPath
Assert-SetupDirectory $setupParent
if (Test-Path -LiteralPath $Target) { Assert-SetupDirectory $Target }
$setupWorkspace = Join-Path $setupParent ('.AirdraftSetup-' + [Guid]::NewGuid().ToString('N'))
$setupStage = Join-Path $setupWorkspace 'incoming'
$setupBackup = Join-Path $setupWorkspace 'previous'
$previousRunning = @()
$nativeVerified = $false
$startedNewPayload = $false
$createdRadioService = $false
$setupCommitted = $false

'Files'
# Every process running from the install folder, not only ours by name: the
# AirDrop engine is the bundled python.exe started by Airdraft.exe, and killing
# Airdraft.exe leaves it running with python\libcrypto-3.dll open, so the copy
# below failed with an IOException on that file and left a half-copied folder (#22).
function Stop-FromTarget {
  $root = [IO.Path]::GetFullPath($Target).TrimEnd('\') + '\'
  # Never by path under a drive root or a folder that is not Airdraft's.
  $byPath = $root.Length -gt 4 -and ((Test-Path (Join-Path $root 'AirdraftRadioService.exe')) -or (Test-Path $legacyExe))
  $ours = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
              try { $byPath -and $_.Path -and $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) } catch { $false } })
  # The standalone beacon runs in Windows PowerShell outside the install folder.
  $beacon = [regex]::Escape((Join-Path $root 'scripts\airdrop-beacon.ps1'))
  $beaconCommand = '(?i)(?:^|\s)-File\s+(?:"' + $beacon + '"|' + $beacon + ')(?:\s|$)'
  $ours += @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
      $byPath -and $_.ProcessId -ne $PID -and $_.Name -eq 'powershell.exe' -and $_.CommandLine -match $beaconCommand } |
    ForEach-Object { Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue })
  $ours = @($ours | Sort-Object Id -Unique)
  $ours | Stop-Process -Force -ErrorAction SilentlyContinue
  $ours | ForEach-Object { try { $_.WaitForExit(10000) | Out-Null } catch { } }
  $ours
}
function Stop-SetupService([string]$Name) {
  $running = Get-Service -Name $Name -ErrorAction SilentlyContinue
  if (!$running -or $running.Status -eq 'Stopped') { return }
  Stop-Service -Name $Name -Force
  $running.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(25))
}
function Stop-EarlierSetupService {
  foreach ($name in @($Service; if ($legacy) { $legacyService })) {
    $previous = Get-Service -Name $name -ErrorAction SilentlyContinue
    if ($previous -and $previous.Status -in 'Running', 'StartPending') { $script:previousRunning += $name }
    Stop-SetupService $name
  }
  Stop-FromTarget | Out-Null
}
function Confirm-EarlierSetupWifi {
  $previousExe = if ($existing) { Join-Path $Target "$Service.exe" } elseif ($legacy) { $legacyExe }
  if ($previousExe) {
    if (!(Test-Path -LiteralPath $previousExe -PathType Leaf)) {
      throw 'The previous radio service is missing. Run Airdraft - restore Windows Wi-Fi before retrying Setup.'
    }
    & $previousExe --give-back
    if ($LASTEXITCODE -ne 0) {
      throw 'The previous radio service could not return the adapter to Windows. Run Airdraft - restore Windows Wi-Fi before retrying Setup.'
    }
  }
}
Step 'prepare the application files' {
  New-Item -ItemType Directory -Path $setupStage -Force | Out-Null
  $exclude = 'Setup.cmd', 'install.ps1', 'tap-windows.exe'
  if ($combined) { $exclude += $bundleFile, 'driver', 'driver-rtl8852be', 'radio' }
  Copy-Item "$Source\*" $setupStage -Recurse -Force -Exclude $exclude
  if ($combined) {
    $driverTarget = Join-Path $setupStage 'driver'
    New-Item -ItemType Directory -Force $driverTarget | Out-Null
    foreach ($file in $driverFiles) {
      Copy-Item -LiteralPath (Join-Path (Join-Path $Source $sourceDriver) $file) -Destination $driverTarget -Force
    }
    $radioTarget = Join-Path $setupStage 'radio'
    New-Item -ItemType Directory -Force $radioTarget | Out-Null
    if ($experimental) {
      Copy-Item -LiteralPath (Join-Path $Source 'radio\airdraft-radio-rtl8852be.exe') -Destination $radioTarget -Force
      Set-Content -LiteralPath (Join-Path $setupStage $variantFile) -Value '10EC:B852', '10EC:B85B' -Encoding ASCII
    } else {
      Copy-Item -LiteralPath (Join-Path $Source 'radio\airdraft-radio.exe'), (Join-Path $Source 'radio\firmware') -Destination $radioTarget -Recurse -Force
    }
  }
}

'Driver'
$driver = Join-Path $setupStage 'driver'
Step 'sign and stage the register-proxy driver on this PC' {
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject `
            -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable `
            -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
  $public = Join-Path $driver "Airdraft-$($cert.Thumbprint).cer"
  $registered = $false
  try {
    Export-Certificate -Cert $cert -FilePath $public | Out-Null
    foreach ($store in $TrustStores) { Import-Certificate -FilePath $public -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null }
    foreach ($file in 'winawdl_regproxy.cat', 'winawdl_regproxy.dll') {
      $r = Set-AuthenticodeSignature -FilePath (Join-Path $driver $file) -Certificate $cert -HashAlgorithm SHA256
      if ($r.Status -ne 'Valid') { throw "$file could not be signed: $($r.StatusMessage)" }
    }
    # Add the package without binding it to a physical device. Earlier packages
    # may still be in the driver store, so their public signers stay trusted.
    pnputil /add-driver (Join-Path $driver 'winawdl_regproxy.inf') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "pnputil could not add the driver ($LASTEXITCODE)" }
    $registered = $true
    Move-Item -LiteralPath $public -Destination (Join-Path $driver 'Airdraft-signing.cer') -Force
  } finally {
    if (!$registered) {
      foreach ($store in $TrustStores) {
        Remove-Item -LiteralPath "Cert:\LocalMachine\$store\$($cert.Thumbprint)" -ErrorAction SilentlyContinue
      }
    }
    Remove-Item -LiteralPath $public -ErrorAction SilentlyContinue
    Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey
  }
}

Step 'stop an earlier service' { Stop-EarlierSetupService }
Step 'verify Windows has its Wi-Fi adapter before replacing files' { Confirm-EarlierSetupWifi }
$nativeVerified = $true

'Network'
if (!$experimental) {
  Step 'Airdraft TAP adapter for concurrent Wi-Fi' {
    if ($legacy -and !(Get-NetAdapter -Name 'Airdraft' -IncludeHidden -ErrorAction SilentlyContinue)) {
      $legacyTap = Get-NetAdapter -Name 'nekodrop' -IncludeHidden -ErrorAction SilentlyContinue |
        Where-Object { $_.InterfaceDescription -like 'TAP-Windows Adapter V9*' -and $_.Status -ne 'Not Present' }
      if ($legacyTap) { $legacyTap | Rename-NetAdapter -NewName 'Airdraft' }
    }
    & "$Source\setup-tap.ps1" -TapSetup "$Source\tap-windows.exe" | Out-Host
  }
}
Step 'firewall: AirDrop receiver and discovery, from link-local addresses only' {
  $common = @{ Group = $FirewallGroup; Direction = 'Inbound'; Action = 'Allow'; Profile = 'Any'; RemoteAddress = 'fe80::/64' }
  if (!(Get-NetFirewallRule -Name 'Airdraft.Receiver' -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule @common -Name 'Airdraft.Receiver' -DisplayName 'Airdraft AirDrop receiver' -Protocol TCP -LocalPort 8771-8779 | Out-Null
  }
  if (!(Get-NetFirewallRule -Name 'Airdraft.Discovery' -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule @common -Name 'Airdraft.Discovery' -DisplayName 'Airdraft AirDrop discovery' -Protocol UDP -LocalPort 5353 | Out-Null
  }
}

Step 'publish the prepared files' {
  Publish-SetupFiles $setupStage $Target $setupBackup $fileChanges $createdDirectories
}
$driver = Join-Path $Target 'driver'

'Service'
Step 'install and start the Airdraft radio service' {
  $script:startedNewPayload = $true
  $exe = Join-Path $Target 'AirdraftRadioService.exe'
  if (-not (Get-Service $Service -ErrorAction SilentlyContinue)) {
    New-Service -Name $Service -BinaryPathName "`"$exe`"" -DisplayName 'Airdraft Radio Service' `
      -Description 'AirDrop on the built-in Wi-Fi card: owns the card while AirDrop is on.' -StartupType Automatic | Out-Null
    $script:createdRadioService = $true
    sc.exe failure $Service reset= 86400 actions= restart/5000/restart/30000/""/0 | Out-Null
  }
  # Wi-Fi recovery deliberately disables the service. Setup must undo that.
  Set-Service -Name $Service -StartupType Automatic
  Start-Service -Name $Service
  (Get-Service -Name $Service).WaitForStatus('Running', [TimeSpan]::FromSeconds(20))
  $probe = Start-Process -FilePath (Join-Path $Target 'Airdraft.exe') -ArgumentList '--check-service' -WindowStyle Hidden -PassThru
  try {
    if (!$probe.WaitForExit(20000)) {
      if (!$probe.HasExited) { $probe.Kill(); $null = $probe.WaitForExit(2000) }
      throw 'The Airdraft radio service check timed out.'
    }
    if ($probe.ExitCode -ne 0) { throw 'The installed radio service did not respond to Airdraft. See the radio service log.' }
  } finally {
    $probe.Dispose()
  }
}
Step 'retire the 0.1.0 service registration' {
  if ($legacy) {
    sc.exe delete $legacyService | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not retire the previous radio service ($LASTEXITCODE)." }
  }
}
$setupCommitted = $true
Remove-SetupWorkspace $setupWorkspace $setupParent

Step 'remove older Airdraft support files' {
  Get-NetFirewallRule -Group $FirewallGroup -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notin 'Airdraft.Receiver', 'Airdraft.Discovery' -and
                   $_.DisplayName -in 'Airdraft AirDrop receiver', 'Airdraft AirDrop discovery' } |
    Remove-NetFirewallRule -ErrorAction Continue
  if ($legacy) {
    Get-NetFirewallRule -Group 'nekodrop AirDrop' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    foreach ($oldFile in $legacyExe, (Join-Path $Target 'radio\nekodrop-radio.exe'), (Join-Path $driver 'nekodrop-signing.cer')) {
      if (Test-Path -LiteralPath $oldFile) { Remove-Item -LiteralPath $oldFile }
    }
  }
}
Step 'start the unelevated Airdraft tray at sign-in' {
  $startup = [Environment]::GetFolderPath('CommonStartup')
  $shell = New-Object -ComObject WScript.Shell
  $shortcut = $shell.CreateShortcut((Join-Path $startup 'Airdraft.lnk'))
  $shortcut.TargetPath = Join-Path $Target 'Airdraft.exe'
  $shortcut.Arguments = '--background'
  $shortcut.Save()
}
Step 'add Airdraft to Start' {
  $programs = [Environment]::GetFolderPath('CommonPrograms')
  $shell = New-Object -ComObject WScript.Shell
  foreach ($entry in @(
    @('Airdraft', 'Airdraft.exe'),
    @('Airdraft - restore Windows Wi-Fi', 'restore.cmd'),
    @('Airdraft - diagnostic report', 'report.cmd')
  )) {
    $targetFile = Join-Path $Target $entry[1]
    $shortcut = $shell.CreateShortcut((Join-Path $programs ($entry[0] + '.lnk')))
    if ($shortcut.TargetPath -ieq $targetFile -and !$shortcut.Arguments) { continue }
    $shortcut.TargetPath = $targetFile
    $shortcut.Arguments = ''
    $shortcut.WorkingDirectory = $Target
    $shortcut.Save()
  }
}
'Airdraft is installed.'
Stop-Transcript | Out-Null
$setupMutex.ReleaseMutex()
$setupMutex.Dispose()
exit 0
