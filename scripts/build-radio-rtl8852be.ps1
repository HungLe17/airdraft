# Copyright (c) 2026 Airdraft
# SPDX-License-Identifier: GPL-2.0-only
# Builds the experimental RTL8852BE radio into the workspace.
param([switch]$UseCachedDependencies)

$ErrorActionPreference = 'Stop'
if (!$UseCachedDependencies) { & "$PSScriptRoot\fetch-deps.ps1" -RealtekRadioOnly }

. "$PSScriptRoot\toolchain.ps1"
$repo = Split-Path $PSScriptRoot -Parent
$tools = Initialize-Toolchain
$manifest = Join-Path $repo 'third_party\winawdl\deps'
$build = Join-Path $repo 'build\rtl8852be'
$vcpkgRoot = $env:VCPKG_ROOT
if (!$vcpkgRoot -and $tools.VsRoot) { $vcpkgRoot = Join-Path $tools.VsRoot 'VC\vcpkg' }
if (!$vcpkgRoot -or !(Test-Path -LiteralPath (Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'))) {
    throw 'Set VCPKG_ROOT to the pinned vcpkg checkout that provides the x64-windows-static toolchain.'
}

$env:VCPKG_ROOT = $vcpkgRoot
$configure = @(
    '-S', (Join-Path $repo 'rtlradio'), '-B', $build, '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$($tools.Ninja)", "-DCMAKE_C_COMPILER=$($tools.Cl)",
    "-DCMAKE_CXX_COMPILER=$($tools.Cl)", '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkgRoot\scripts\buildsystems\vcpkg.cmake",
    "-DVCPKG_MANIFEST_DIR=$manifest", '-DVCPKG_TARGET_TRIPLET=x64-windows-static',
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded'
)
& $tools.CMake @configure
if ($LASTEXITCODE) { throw "RTL8852BE configure failed ($LASTEXITCODE)." }
& $tools.CMake --build $build --target airdraft-radio-rtl8852be
if ($LASTEXITCODE) { throw "RTL8852BE compile failed ($LASTEXITCODE)." }
Get-Item -LiteralPath (Join-Path $build 'airdraft-radio-rtl8852be.exe')
