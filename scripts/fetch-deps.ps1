# Fetches what the build needs into third_party\ (not in this repository):
#   winawdl         radio pipe, RTL8852CE radio, awdld (OWL), Wintun, AirDrop engine (MIT/GPL-2.0/GPL-3.0)
#   orchard-devourer  optional experimental Realtek PCIe radio source (GPL-2.0)
#   ForbiddenItlwm  iwlwifi firmware and its licence (GPL-2.0 / Intel redistribution)
#   winlibs MinGW   GCC for awdld
#   WDK (NuGet)     UMDF headers and tools, only when no WDK is installed
# Dependency versions are pinned. Downloads with a recorded SHA-256 are verified.
param([switch]$ExperimentalRealtek, [switch]$RealtekRadioOnly)

$ErrorActionPreference = 'Stop'
$tp = Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party'
New-Item -ItemType Directory -Force $tp | Out-Null

function Get-Repo($name, $url, $commit) {
  $dir = Join-Path $tp $name
  if (-not (Test-Path "$dir\.git")) { git clone --quiet $url $dir; if ($LASTEXITCODE) { throw "clone $url failed" } }
  git -C $dir fetch --quiet origin
  git -C $dir checkout --quiet $commit
  if ($LASTEXITCODE) { throw "$name`: commit $commit not found" }
  "$name @ $($commit.Substring(0, 7))"
}

function Get-File($url, $out, $sha256) {
  if (-not (Test-Path $out)) { Invoke-WebRequest $url -OutFile $out -UseBasicParsing }
  $got = (Get-FileHash $out -Algorithm SHA256).Hash
  if ($sha256 -and $got -ne $sha256) { Remove-Item $out; throw "$out`: SHA-256 $got, expected $sha256" }
}

Get-Repo winawdl https://github.com/raph559/winawdl.git 0db7031589dc2bca03c0893db341176376962047
if ($ExperimentalRealtek -or $RealtekRadioOnly) {
  Get-Repo orchard-devourer https://github.com/orchard-win/orchard-devourer.git 4f6d1e93fdd002aba08d028a803372cb706fa9ec
}
if ($RealtekRadioOnly) { return }
Get-Repo ForbiddenItlwm https://github.com/retropineaple/ForbiddenItlwm.git 96ff71e0211789239f6fbf6b64ac8e7fd22b41aa
Get-Repo tap-windows6 https://github.com/OpenVPN/tap-windows6.git eb764efe1579dc0d9d64a64186b16491af37de88

git -C "$tp\winawdl" submodule update --init --recursive upstream/owl
if ($LASTEXITCODE) { throw 'Could not fetch the pinned OWL and radiotap submodules.' }

# Wintun for awdld: winawdl's script checks its SHA-256 and WireGuard's signature.
if (-not (Test-Path "$tp\winawdl\deps\wintun\wintun.dll")) {
  & "$tp\winawdl\scripts\fetch-wintun.ps1"
}

$mingw = Join-Path $tp 'winlibs'
if (-not (Test-Path "$mingw\mingw64\bin\gcc.exe")) {
  New-Item -ItemType Directory -Force $mingw | Out-Null
  Get-File 'https://github.com/brechtsanders/winlibs_mingw/releases/download/16.2.0posix-23.1.3-14.0.0-ucrt-r3/winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r3.zip' `
           "$mingw\mingw.zip" '3C69305081E4A647144339A936244FCF7D3D9E558319E1AD7BE2CD2886D4B6A1'
  Expand-Archive "$mingw\mingw.zip" $mingw -Force
}
'winlibs MinGW (GCC 16.2.0)'

$kit = "${env:ProgramFiles(x86)}\Windows Kits\10"
if (-not (Test-Path "$kit\Include\wdf\umdf")) {
  $wdk = Join-Path $tp 'wdk-nuget'
  if (-not (Test-Path "$wdk\wdk\c")) {
    New-Item -ItemType Directory -Force $wdk | Out-Null
    Get-File 'https://www.nuget.org/api/v2/package/Microsoft.Windows.WDK.x64/10.0.26100.6584' "$wdk\wdk.nupkg" $null
    Copy-Item "$wdk\wdk.nupkg" "$wdk\wdk.zip"
    Expand-Archive "$wdk\wdk.zip" "$wdk\wdk" -Force
    Remove-Item "$wdk\wdk.zip"
  }
  'WDK 10.0.26100.6584 (NuGet)'
} else { 'WDK: installed' }
