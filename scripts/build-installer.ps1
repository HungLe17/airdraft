# Builds the AX210 developer preview and stages it in build\stage.
#   radio\    Intel radio and AX210 firmware
#   awdl\     awdld.exe (winawdl's AWDL daemon, OWL) + wintun.dll
#   airdrop\  winawdl's AirDrop engine (Python, MIT)
#   python\   the Python embeddable distribution
#   driver\   the register-proxy UMDF driver package (signed on the target PC)
#   Airdraft.exe (native tray), AirdraftRadioService.exe (service), install/uninstall scripts
# -Offline skips the downloads (Python embeddable, TAP-Windows) and uses what
# build\downloads already holds.
# -StageOnly builds the local Setup folder without running project self-tests or making a release archive.
param([switch]$Offline, [string]$PythonVersion = '3.12.8', [string]$TapVersion = '9.24.7-I601',
      [switch]$ExperimentalRtl8852be, [switch]$IncludeRtl8852be, [switch]$StageOnly)
$ErrorActionPreference = 'Stop'
if ($ExperimentalRtl8852be -and $IncludeRtl8852be) { throw 'Choose the Realtek-only or combined build.' }
. "$PSScriptRoot\toolchain.ps1"
$repo = Split-Path $PSScriptRoot -Parent
$build = [IO.Path]::GetFullPath((Join-Path $repo 'build'))
$stageName = if ($IncludeRtl8852be) { 'stage-dual-radio' }
             elseif ($ExperimentalRtl8852be) { 'stage-rtl8852be' }
             elseif ($StageOnly) { 'stage-ax210-local' } else { 'stage' }
$stage = Join-Path $build $stageName
$downloads = "$repo\build\downloads"
$tools = Initialize-Toolchain
foreach ($directory in $build, $stage) {
  if ((Test-Path -LiteralPath $directory) -and
      ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw "Cannot stage the build through a reparse point: $directory"
  }
}
if (Test-Path -LiteralPath $stage) {
  $resolvedStage = (Resolve-Path -LiteralPath $stage).ProviderPath
  if ([IO.Path]::GetDirectoryName($resolvedStage) -ine $build -or $resolvedStage -ine $stage) {
    throw "Refusing to replace a staging directory outside $build."
  }
  Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
New-Item -ItemType Directory -Force $stage, $downloads | Out-Null

'radio'
if ($ExperimentalRtl8852be -or $IncludeRtl8852be) {
  $radio = & "$PSScriptRoot\build-radio-rtl8852be.ps1" -UseCachedDependencies | Select-Object -Last 1
  New-Item -ItemType Directory -Force "$stage\radio" | Out-Null
  Copy-Item -LiteralPath $radio.FullName -Destination "$stage\radio"
  $marker = if ($IncludeRtl8852be) { 'Airdraft.DualRadio' } else { 'RTL8852BE.experimental' }
  Set-Content -LiteralPath (Join-Path $stage $marker) -Value '10EC:B852', '10EC:B85B' -Encoding ASCII
}
if (!$ExperimentalRtl8852be) {
  & "$PSScriptRoot\build-radio.ps1" -StageOnly:$StageOnly | Out-Null
  New-Item -ItemType Directory -Force "$stage\radio\firmware" | Out-Null
  Copy-Item "$repo\build\iwxradio\winawdl_radio.exe" "$stage\radio\airdraft-radio.exe"
  Copy-Item "$repo\build\iwxradio\firmware\iwlwifi-ty-a0-gf-a0-68.ucode",
            "$repo\build\iwxradio\firmware\iwlwifi-ty-a0-gf-a0.pnvm" "$stage\radio\firmware\"
}

'awdl'
& "$PSScriptRoot\build-awdl.ps1" -MinGW "$repo\third_party\winlibs\mingw64\bin" -StageOnly:$StageOnly | Out-Null
New-Item -ItemType Directory -Force "$stage\awdl" | Out-Null
Copy-Item "$repo\third_party\winawdl\build\awdl\awdld.exe", "$repo\third_party\winawdl\deps\wintun\wintun.dll" "$stage\awdl\"

'airdrop engine'
New-Item -ItemType Directory -Force "$stage\airdrop" | Out-Null
Copy-Item "$repo\third_party\winawdl\airdrop\*.py", "$repo\third_party\winawdl\airdrop\LICENSE" "$stage\airdrop\"
& python -I "$PSScriptRoot\patch-engine.py" "$stage\airdrop"
if ($LASTEXITCODE -ne 0) { throw 'engine patch failed' }
# sender.py looks for the Bluetooth beacon script in ..\scripts beside its own folder.
New-Item -ItemType Directory -Force "$stage\scripts" | Out-Null
# Airdraft's copy (#22): leaves the install folder and exits with the engine.
Copy-Item "$repo\installer\airdrop-beacon.ps1" "$stage\scripts\"

'driver'
function Stage-Proxy([bool]$Realtek, [string]$Destination) {
  & "$repo\driver\regproxy\build.ps1" -ExperimentalRtl8852be:$Realtek -PreviewAx210:(!$Realtek) | Out-Null
  $proxy = Join-Path "$repo\driver\regproxy\build" $(if ($Realtek) { 'rtl8852be' } else { 'ax210' })
  New-Item -ItemType Directory -Force $Destination | Out-Null
  Copy-Item "$proxy\winawdl_regproxy.inf", "$proxy\winawdl_regproxy.dll" $Destination
  & $tools.StampInf -f (Join-Path $Destination 'winawdl_regproxy.inf') -d '11/24/2026' -v '0.1.6.0' | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "StampInf failed ($LASTEXITCODE)" }
  & $tools.Inf2Cat "/driver:$Destination" /os:10_X64 /uselocaltime | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed ($LASTEXITCODE)" }
}
Stage-Proxy $ExperimentalRtl8852be.IsPresent (Join-Path $stage 'driver')
if ($IncludeRtl8852be) { Stage-Proxy $true (Join-Path $stage 'driver-rtl8852be') }

'native C++ tray'
$native = "$repo\build\native"
& $tools.CMake -S "$repo\app\airdraft" -B $native -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'native tray configure failed' }
& $tools.CMake --build $native --config Release
if ($LASTEXITCODE -ne 0) { throw 'native tray build failed' }
Copy-Item "$native\Release\Airdraft.exe" $stage
Copy-Item "$repo\CREDITS.md", "$repo\PROVENANCE.md", "$repo\README.md", "$repo\LICENSE" $stage

'service'
$single = '--self-contained', 'true', '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true',
          '-p:EnableCompressionInSingleFile=true', '-p:DebugType=none'
if ($ExperimentalRtl8852be) { $single += '-p:ExperimentalRtl8852be=true' }
if ($Offline) { $single += '--source', $downloads, '-p:NuGetAudit=false' }
dotnet publish "$repo\app\service\Airdraft.Service.csproj" -c Release -o "$stage" --nologo -v q @single
if ($LASTEXITCODE -ne 0) { throw 'service publish failed' }
Copy-Item "$repo\installer\Setup.cmd", "$repo\installer\install.ps1", "$repo\installer\uninstall.ps1", "$repo\installer\setup-tap.ps1",
          "$repo\installer\restore.ps1", "$repo\installer\restore.cmd",
          "$repo\installer\report.ps1", "$repo\installer\report.cmd" $stage
# uninstall.ps1 and setup-tap.ps1 have no self-test: parse them (and the rest)
# under Windows PowerShell 5.1 so a syntax error fails the build (#22).
foreach ($script in 'install.ps1', 'uninstall.ps1', 'setup-tap.ps1', 'restore.ps1', 'report.ps1', 'scripts\airdrop-beacon.ps1') {
  & powershell.exe -NoProfile -Command "`$e = `$null; [void][Management.Automation.Language.Parser]::ParseFile('$stage\$script', [ref]`$null, [ref]`$e); if (`$e) { `$e | ForEach-Object { `$_.ToString() }; exit 1 }"
  if ($LASTEXITCODE -ne 0) { throw "$script does not parse" }
}

'downloads'
$py = "$downloads\python-$PythonVersion-embed-amd64.zip"
$tap = "$downloads\tap-windows-$TapVersion.exe"
if (-not $Offline) {
  if (-not (Test-Path $py)) {
    Invoke-WebRequest "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-embed-amd64.zip" -OutFile $py -UseBasicParsing
  }
  if (-not $ExperimentalRtl8852be -and -not (Test-Path $tap)) {
    Invoke-WebRequest "https://build.openvpn.net/downloads/releases/tap-windows-$TapVersion-Win10.exe" -OutFile $tap -UseBasicParsing
  }
}
if (Test-Path $py) {
  if ((Get-FileHash $py -Algorithm SHA256).Hash -ne '8D3F33BE9EB810F23C102F08475AF2854E50484B8E4E06275E937BE61CE3D2FB') {
    throw 'Unexpected Python 3.12.8 embeddable archive SHA-256.'
  }
  Expand-Archive $py "$stage\python" -Force
  # The embeddable distribution's ._pth fixes sys.path: list the engine's folder
  # there, since its modules import each other.
  $pth = Get-ChildItem "$stage\python" -Filter 'python*._pth' | Select-Object -First 1
  Add-Content $pth.FullName '..\airdrop'
} else { throw "Required Python embeddable runtime missing: $py" }
if ($ExperimentalRtl8852be) {
  'The RTL8852BE build uses temporary handoff and does not install TAP.'
} elseif (Test-Path $tap) {
  if ((Get-FileHash $tap -Algorithm SHA256).Hash -ne '1C44E77AB148DDFB174DE8041100EFC38C4F23500183FAFF88F9BADAC5782E3C') {
    throw 'Unexpected TAP-Windows 9.24.7-I601 installer SHA-256.'
  }
  $sig = Get-AuthenticodeSignature $tap
  if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'OpenVPN') {
    throw "tap-windows installer is not validly signed by OpenVPN: $($sig.Status)"
  }
  Copy-Item $tap "$stage\tap-windows.exe"
} else { throw "Required TAP-Windows installer missing: $tap" }

'licenses'
New-Item -ItemType Directory -Force "$stage\licenses" | Out-Null
if ($ExperimentalRtl8852be -or $IncludeRtl8852be) {
  Copy-Item "$repo\third_party\orchard-devourer\LICENSE" "$stage\licenses\orchard-devourer-GPL-2.0.txt"
}
if (!$ExperimentalRtl8852be) {
  Copy-Item "$repo\third_party\ForbiddenItlwm\LICENSE" "$stage\licenses\ForbiddenItlwm-GPL-2.0.txt"
  Copy-Item "$repo\third_party\ForbiddenItlwm\iwlwifi-firmware-license" "$stage\licenses\iwlwifi-firmware.txt"
  New-Item -ItemType Directory -Force "$stage\licenses\tap-windows6" | Out-Null
  Copy-Item "$repo\third_party\tap-windows6\COPYING", "$repo\third_party\tap-windows6\COPYRIGHT.GPL",
            "$repo\third_party\tap-windows6\COPYRIGHT.MIT" "$stage\licenses\tap-windows6"
  Copy-Item "$repo\third_party\ForbiddenItlwm\iwlwifi-firmware-license" "$stage\radio\firmware\LICENSE.iwlwifi-firmware"
}
Copy-Item "$repo\third_party\winawdl\LICENSE" "$stage\licenses\winawdl-MIT.txt"
Copy-Item "$repo\third_party\winawdl\awdl\LICENSE" "$stage\licenses\awdld-GPL-3.0.txt"
Copy-Item "$repo\third_party\winawdl\airdrop\LICENSE" "$stage\licenses\airdrop-engine-MIT.txt"
Copy-Item "$repo\LICENSE" "$stage\licenses\GPL-2.0.txt"
Copy-Item "$repo\third_party\winawdl\deps\wintun\LICENSE.txt" "$stage\licenses\wintun.txt"

$assets = Get-Content -Raw "$repo\app\service\obj\project.assets.json" | ConvertFrom-Json
$runtime = $assets.project.frameworks.PSObject.Properties.Value.downloadDependencies |
    Where-Object name -eq 'Microsoft.NETCore.App.Runtime.win-x64' | Select-Object -First 1
if (!$runtime) { throw '.NET runtime metadata is missing from the service build.' }
$runtimeVersion = ($runtime.version -split '[\[\], ]+' | Where-Object { $_ })[0]
$runtimePackage = $assets.packageFolders.PSObject.Properties.Name | ForEach-Object {
  Join-Path $_ "microsoft.netcore.app.runtime.win-x64\$runtimeVersion"
} | Where-Object { Test-Path "$_\LICENSE.TXT" } | Select-Object -First 1
if (!$runtimePackage) { throw '.NET runtime notices were not found.' }
Copy-Item "$runtimePackage\LICENSE.TXT" "$stage\licenses\dotnet-runtime-LICENSE.txt"
Copy-Item "$runtimePackage\THIRD-PARTY-NOTICES.TXT" "$stage\licenses\dotnet-runtime-THIRD-PARTY-NOTICES.txt"
if ($StageOnly) {
  "Built the local development setup at $stage. Project tests were skipped."
  return
}
# Service and installer checks do not use a card or require administrator rights.
& "$stage\AirdraftRadioService.exe" --selftest
if ($LASTEXITCODE -ne 0) { throw 'service self-test failed' }
foreach ($script in 'install.ps1', 'restore.ps1', 'report.ps1') {
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$stage\$script" -SelfTest
  if ($LASTEXITCODE -ne 0) { throw "$script self-test failed ($LASTEXITCODE)" }
}
python -I "$PSScriptRoot\test-airdraft-engine.py" $stage
if ($LASTEXITCODE -ne 0) { throw 'Airdraft native identity or engine receive control test failed.' }
if ($ExperimentalRtl8852be -or $IncludeRtl8852be) {
  "Built the local setup at $stage. See PROVENANCE.md for its redistribution status."
  return
}
$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if ($iscc) {
  & $iscc "$repo\installer\Airdraft.iss" | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed ($LASTEXITCODE)" }
  Get-ChildItem "$repo\build\Airdraft-*-Developer-Preview-Setup.exe" | Select-Object Name, Length
} else {
  $zip = "$repo\build\Airdraft-0.1.6-Developer-Preview-x64.zip"
  Compress-Archive "$stage\*" $zip -Force
  "Inno Setup not found: made $zip instead (unzip, then run Setup.cmd)."
}
