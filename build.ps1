# Copyright (c) 2026 Airdraft
# SPDX-License-Identifier: GPL-2.0-only

#Requires -Version 5.1
[CmdletBinding()]
param([switch]$UseCachedDependencies, [switch]$ExperimentalRtl8852be, [switch]$IncludeRtl8852be, [switch]$StageOnly)

$ErrorActionPreference = 'Stop'
if ($ExperimentalRtl8852be -and $IncludeRtl8852be) { throw 'Choose the Realtek-only or combined build.' }
if (!$UseCachedDependencies) {
    & "$PSScriptRoot\scripts\fetch-deps.ps1" -ExperimentalRealtek:($ExperimentalRtl8852be -or $IncludeRtl8852be)
}
& "$PSScriptRoot\scripts\build-installer.ps1" -Offline:($UseCachedDependencies -and $StageOnly) `
    -ExperimentalRtl8852be:$ExperimentalRtl8852be -IncludeRtl8852be:$IncludeRtl8852be -StageOnly:$StageOnly
if ($ExperimentalRtl8852be -or $IncludeRtl8852be -or $StageOnly) { return }
$sourceArchive = "$PSScriptRoot\build\Airdraft-0.1.6-Developer-Preview-Corresponding-Source.tar.gz"
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
$snapshots = "$PSScriptRoot\build\upstream-source"
New-Item -ItemType Directory -Force $snapshots | Out-Null

function Write-SourceArchive([string]$Destination, [string]$Base, [string[]]$Entries) {
    & $tar -czf $Destination --exclude '.git' --exclude 'build' --exclude 'bin' --exclude 'obj' -C $Base @Entries
    if ($LASTEXITCODE) { throw "Could not write $Destination (tar exit $LASTEXITCODE)." }
}

Write-SourceArchive "$snapshots\winawdl.tar.gz" "$PSScriptRoot\third_party\winawdl" @('LICENSE', 'airdrop', 'awdl', 'radio', 'deps/vcpkg.json', 'upstream/owl')
Write-SourceArchive "$snapshots\ForbiddenItlwm.tar.gz" "$PSScriptRoot\third_party\ForbiddenItlwm" @('.')
Write-SourceArchive "$snapshots\tap-windows6.tar.gz" "$PSScriptRoot\third_party\tap-windows6" @('.')
Write-SourceArchive $sourceArchive $PSScriptRoot @('app', 'docs', 'driver', 'installer', 'iwxradio', 'rtlradio', 'kdriver',
    'scripts', 'build.ps1', 'CREDITS.md', 'LICENSE', 'PROVENANCE.md', 'README.md', '-C', "$PSScriptRoot\build", 'upstream-source')
$artifacts = Get-ChildItem -LiteralPath "$PSScriptRoot\build" -File -Filter 'Airdraft-0.1.6-Developer-Preview-*'
if (!$artifacts) { throw 'No Airdraft preview package was produced.' }
$artifacts | ForEach-Object {
    $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
    "$($hash.Hash)  $($_.Name)"
} | Set-Content -LiteralPath "$PSScriptRoot\build\SHA256SUMS.txt" -Encoding ASCII
