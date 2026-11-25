# Builds kdriver\ into build\kdriver\nekodrop-wlan.sys (KMDF 1.33, NetAdapterCx 2.4,
# WiFiCx 1.2) with MSVC and the WDK NuGet package. Not signed here.
param([switch]$ExperimentalAx210)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"
$repo = Split-Path $PSScriptRoot -Parent
$tools = Initialize-Toolchain
$kit = 'C:\Program Files (x86)\Windows Kits\10'
$wdk = Join-Path $repo 'third_party\wdk-nuget\wdk\c'
$ver = Get-ChildItem "$wdk\Include" -Directory | Where-Object { Test-Path "$($_.FullName)\km\wificx" } |
       Sort-Object Name | Select-Object -Last 1 -ExpandProperty Name
$msvc = Split-Path (Split-Path (Split-Path (Split-Path $tools.Cl)))
$inc = @(
  (Join-Path $wdk "Include\$ver\km"),
  (Join-Path $wdk "Include\$ver\km\crt"),
  (Join-Path $wdk "Include\$ver\shared"),
  (Join-Path $kit "Include\$ver\shared"),
  (Join-Path $kit "Include\$ver\shared\netcx\shared\1.0"),
  (Join-Path $wdk 'Include\wdf\kmdf\1.33'),
  (Join-Path $wdk "Include\$ver\km\netcx\kmdf\adapter\2.4"),
  (Join-Path $wdk "Include\$ver\km\wificx\1.2"),
  (Join-Path $wdk "Include\$ver\km\wlan\2.0"),
  (Join-Path $msvc 'include')
)
$lib = @(
  (Join-Path $wdk "Lib\$ver\km\x64"),
  (Join-Path $wdk 'Lib\wdf\kmdf\x64\1.33'),
  (Join-Path $wdk "Lib\$ver\km\x64\netcx\kmdf\adapter\2.4"),
  (Join-Path $wdk "Lib\$ver\km\x64\wificx\1.2"),
  (Join-Path $wdk "Lib\$ver\km\x64\wlan\2.0"),
  (Join-Path $msvc 'lib\x64')
)
$env:INCLUDE = $inc -join ';'
$env:LIB = $lib -join ';'
$build = [IO.Path]::GetFullPath((Join-Path $repo 'build'))
$out = Join-Path $build $(if ($ExperimentalAx210) { 'kdriver-ax210' } else { 'kdriver' })
foreach ($directory in $build, $out, (Join-Path $out 'package')) {
  if ((Test-Path -LiteralPath $directory) -and
      ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw "Cannot build the WiFiCx package through a reparse point: $directory"
  }
}
New-Item -ItemType Directory -Force $out | Out-Null
$defs = '/D_AMD64_', '/D_WIN64', '/DAMD64', '/DNTDDI_VERSION=0x0A000010', '/D_WIN32_WINNT=0x0A00',
        '/DKMDF_VERSION_MAJOR=1', '/DKMDF_VERSION_MINOR=33',
        '/DNET_ADAPTER_CX_2_4', '/DNETADAPTER_VERSION_MAJOR=2', '/DNETADAPTER_VERSION_MINOR=4', '/DNET_VERSION_MAJOR=2', '/DNET_VERSION_MINOR=4',
        '/DWIFI_VERSION_MAJOR=1', '/DWIFI_VERSION_MINOR=2', '/DPOOL_NX_OPTIN=1', '/DPOOL_ZERO_DOWN_LEVEL_SUPPORT'
Get-ChildItem $out -Filter *.obj | Remove-Item
$kdir = Join-Path $repo 'kdriver'
$shared = Join-Path $repo 'iwxradio\src'
$common = '/nologo', '/c', '/kernel', '/GS-', '/Gy', '/Zp8', '/W4', '/WX', '/wd4324', '/O2', '/std:c++17', '/EHs-c-',
          '/GR-', '/utf-8', '/Zc:threadSafeInit-'
if ($ExperimentalAx210) { $common += '/Z7' }
# The WDF/WiFiCx glue: kernel headers only.
$src = Get-ChildItem $kdir -Filter *.cpp | Where-Object Name -notmatch '^k(plat|proxy|dma|hal)\.cpp$' | ForEach-Object FullName
& $tools.Cl @common /Zc:wchar_t- @defs /FIprecomp.h "/I$kdir" "/Fo$out\" @src
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }
# The hardware layer shared with the user-mode radio, with the C++ library
# in kernel mode (no exceptions, no RTTI, non-paged allocation in kplat.cpp).
$ucrt = Join-Path $kit "Include\$ver\ucrt"
$env:INCLUDE = (@((Join-Path $msvc 'include'), $ucrt) + $inc) -join ';'
$stl = '/DNEKO_KERNEL', '/D_HAS_EXCEPTIONS=0', '/D_HAS_STATIC_RTTI=0', '/D_ITERATOR_DEBUG_LEVEL=0',
       '/D_USE_STD_VECTOR_ALGORITHMS=0', '/D_CRT_SECURE_NO_WARNINGS', '/D_NO_CRT_STDIO_INLINE', '/DNOMINMAX', '/wd4996', '/wd4244', '/wd4815', '/wd4267'
$hw = @('iwx.cpp', 'iwx_sta.cpp', 'iwx_scan.cpp', 'fw.cpp' | ForEach-Object { Join-Path $shared $_ }) +
      @('kplat.cpp', 'kproxy.cpp', 'kdma.cpp', 'khal.cpp' | ForEach-Object { Join-Path $kdir $_ })
& $tools.Cl @common @defs @stl "/I$kdir" "/I$shared" "/Fo$out\" @hw
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }
$objs = Get-ChildItem $out -Filter *.obj | ForEach-Object FullName
$link = Join-Path (Split-Path $tools.Cl) 'link.exe'
$debug = if ($ExperimentalAx210) { @('/DEBUG:FULL', '/OPT:REF', '/OPT:ICF', "/PDB:$out\nekodrop-wlan.pdb") } else { @() }
& $link /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:FxDriverEntry /NODEFAULTLIB "/OUT:$out\nekodrop-wlan.sys" @debug @objs `
  ntoskrnl.lib hal.lib wmilib.lib BufferOverflowFastFailK.lib wdfldr.lib wdfdriverentry.lib `
  netadaptercxstub.lib wificxstub.lib WificxTLVGenParse.lib ndis.lib wdmsec.lib
if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }
Get-Item (Join-Path $out 'nekodrop-wlan.sys') | Select-Object Name, Length

# Driver package: .sys + INF + firmware, catalog (signed on the target PC).
$pkg = Join-Path $out 'package'
if (Test-Path -LiteralPath $pkg) {
  $resolved = (Resolve-Path -LiteralPath $pkg).ProviderPath
  if ($resolved -ine $pkg -or [IO.Path]::GetDirectoryName($resolved) -ine $out) { throw "Unexpected package directory: $resolved" }
  Remove-Item -LiteralPath $resolved -Recurse -Force
}
New-Item -ItemType Directory -Force $pkg | Out-Null
Copy-Item -LiteralPath (Join-Path $out 'nekodrop-wlan.sys') -Destination $pkg
$fw = Join-Path $repo 'third_party\ForbiddenItlwm\itlwm\firmware'
$inputInf = Join-Path $kdir 'nekodrop-wlan.inf'
$outputInf = Join-Path $pkg 'nekodrop-wlan.inf'
if ($ExperimentalAx210) {
  $inf = Get-Content -LiteralPath $inputInf -Raw
  $models = '(?ms)(^\[Models\.NTamd64\.10\.0\.\.\.22000\]\r?\n).*?(?=^\[Neko_Device\.ndi\.NT\])'
  if ([regex]::Matches($inf, $models).Count -ne 1) { throw 'The WiFiCx INF model section changed.' }
  $entry = '${1}%Ax201.DeviceDesc% = Neko_Device.ndi, PCI\VEN_8086&DEV_2725' + "`r`n`r`n"
  $inf = [regex]::Replace($inf, $models, $entry)
  $inf = [regex]::Replace($inf, '(?m)^iwlwifi-Qu[^\r\n]+\r?\n', '')
  $files = 'iwlwifi-ty-a0-gf-a0-68.ucode', 'iwlwifi-ty-a0-gf-a0.pnvm'
  $inf = $inf -replace '(?m)^\[SourceDisksFiles\]\r?$', ("[SourceDisksFiles]`r`n" + (($files | ForEach-Object { "$_ = 1,," }) -join "`r`n"))
  $inf = $inf -replace '(?m)^\[Firmware_Copy\]\r?$', ("[Firmware_Copy]`r`n" + ($files -join "`r`n"))
  $inf = $inf.Replace('"Intel Wi-Fi 6 AX201 (nekodrop, Wi-Fi + AirDrop)"', '"Intel Wi-Fi 6E AX210 (Airdraft Wi-FiCx development)"')
  $inf = $inf.Replace('"nekodrop project"', '"Airdraft"').Replace('"nekodrop driver"', '"Airdraft WiFiCx driver"')
  $inf = $inf.Replace('"nekodrop Wi-Fi and AWDL driver"', '"Airdraft Wi-Fi and AWDL driver"')
  Set-Content -LiteralPath $outputInf -Value $inf -Encoding Ascii -NoNewline
  foreach ($file in $files) { Copy-Item -LiteralPath (Join-Path $fw $file) -Destination $pkg }
} else {
  Copy-Item -LiteralPath $inputInf -Destination $outputInf
  Get-ChildItem $fw -Filter 'iwlwifi-Qu*-hr-b0-68.ucode' | Copy-Item -Destination $pkg
}
Copy-Item (Join-Path $repo 'third_party\ForbiddenItlwm\iwlwifi-firmware-license') (Join-Path $pkg 'LICENSE.iwlwifi-firmware.txt')
if (Test-Path $tools.InfVerif) {
  & $tools.InfVerif /w $outputInf | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "InfVerif failed ($LASTEXITCODE)" }
}
& $tools.Inf2Cat "/driver:$pkg" /os:10_X64 /uselocaltime | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed ($LASTEXITCODE)" }
Get-ChildItem $pkg | Select-Object Name, Length

if ($ExperimentalAx210) {
  # Keep the unsigned CAB beside the package. Microsoft requires a separate
  # subdirectory inside the cabinet for each driver and its symbols.
  $cabName = 'Airdraft-AX210-WiFiCx-unsigned.cab'
  Push-Location -LiteralPath $out
  try {
    $ddf = @(
      '.OPTION EXPLICIT', '.Set Cabinet=on', '.Set Compress=on', '.Set CompressionType=MSZIP',
      '.Set MaxDiskSize=0', '.Set DiskDirectoryTemplate=.', ".Set CabinetNameTemplate=$cabName",
      '.Set InfFileName=ax210-cab.inf', '.Set RptFileName=ax210-cab.rpt', '.Set DestinationDir=AirdraftAX210',
      'package\nekodrop-wlan.inf', 'package\nekodrop-wlan.sys', 'package\nekodrop-wlan.cat', 'nekodrop-wlan.pdb'
    ) + ($files | ForEach-Object { "package\$_" }) + 'package\LICENSE.iwlwifi-firmware.txt'
    Set-Content -LiteralPath 'ax210-cab.ddf' -Value $ddf -Encoding Ascii
    makecab.exe /f ax210-cab.ddf | Out-Null
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $cabName -PathType Leaf)) { throw 'Could not create the unsigned AX210 CAB.' }
    Get-Item -LiteralPath $cabName | Select-Object Name, Length
  } finally { Pop-Location }
}
