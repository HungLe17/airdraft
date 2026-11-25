#Requires -RunAsAdministrator
# Removes Airdraft: gives the Wi-Fi card back to its vendor driver first, then
# removes the service, the driver package, the certificate trust, the firewall
# rules, Airdraft's TAP adapter, settings and the files. Received files stay
# where they were saved. A failed Wi-Fi recovery leaves the application and
# recovery files in place. Cleanup failures go into Airdraft-uninstall.log.
param([string]$Target = "$env:ProgramFiles\Airdraft")
$ErrorActionPreference = 'Continue'
$Service = 'AirdraftRadioService'
$CertSubject = 'CN=Airdraft driver (signed on this PC)'
$Log = Join-Path $env:ProgramData 'Airdraft-uninstall.log'
"Airdraft uninstall $(Get-Date -Format s), target $Target" | Set-Content $Log -ErrorAction SilentlyContinue
$failures = 0
function Try-Step($what, [scriptblock]$do) {
  $what
  try { & $do 2>&1 | ForEach-Object { "  $_" } | Add-Content $Log -ErrorAction SilentlyContinue }
  catch { $script:failures++; "  FAILED: $what - $($_.Exception.Message)" | Tee-Object -Append $Log }
}

$radioService = Get-Service $Service -ErrorAction SilentlyContinue
if ($radioService) {
  try {
    Set-Service $Service -StartupType Disabled -ErrorAction Stop
    if ($radioService.Status -notin 'Stopped', 'StopPending') { $radioService.Stop() }
    $radioService.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(60))
  } catch {
    "Airdraft is still stopping. Wait for the service to stop before uninstalling again. $($_.Exception.Message)" | Tee-Object -Append $Log
    exit 1
  }
}
Try-Step 'Giving the Wi-Fi card back' {
  # Everything running from the install folder, the AirDrop engine's python.exe too (#22).
  $root = [IO.Path]::GetFullPath($Target).TrimEnd('\') + '\'
  # Never by path under a drive root or a folder that is not Airdraft's.
  $byPath = $root.Length -gt 4 -and ((Test-Path (Join-Path $root 'AirdraftRadioService.exe')) -or (Test-Path (Join-Path $root 'python\python.exe')))
  @(Get-Process Airdraft, airdraft-radio, airdraft-radio-rtl, airdraft-radio-rtl8852be, awdld -ErrorAction SilentlyContinue) +
  @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
      try { $byPath -and $_.Path -and $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) } catch { $false } }) |
    Stop-Process -Force -ErrorAction SilentlyContinue
  # Processes started from elsewhere that name the folder on their command
  # line: airdrop-beacon.ps1's powershell.exe held airdrop\ open (#22).
  @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
      $byPath -and $_.ProcessId -ne $PID -and $_.CommandLine -and
      $_.CommandLine.IndexOf($root.TrimEnd(''), [StringComparison]::OrdinalIgnoreCase) -ge 0 }) |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
  $svc = Join-Path $Target 'AirdraftRadioService.exe'
  if (Test-Path $svc) {
    & $svc --give-back
    if ($LASTEXITCODE -ne 0) { throw "Native Wi-Fi recovery is incomplete ($LASTEXITCODE)." }
  }
  else {
    # A failed update can leave the folder without the service: restore.ps1
    # gives the card back on its own, from the same folder or this script's.
    $restore = @((Join-Path $Target 'restore.ps1'), (Join-Path $PSScriptRoot 'restore.ps1')) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (!$restore) { throw "neither AirdraftRadioService.exe nor restore.ps1 is in $Target" }
    & $restore
    if ($LASTEXITCODE -ne 0) { throw "Standalone Wi-Fi recovery is incomplete ($LASTEXITCODE)." }
  }
}
if ($failures) {
  'Airdraft kept its recovery files. Run restore.cmd before retrying uninstall.' | Tee-Object -Append $Log
  exit 1
}

Try-Step 'Service' {
  if (Get-Service $Service -ErrorAction SilentlyContinue) {
    sc.exe delete $Service | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sc.exe delete exited $LASTEXITCODE" }
  }
}

Try-Step 'Driver package and certificate' {
  $store = [xml](pnputil /enum-drivers /format xml | Out-String)
  $store.PnpUtil.Driver | Where-Object { $_.OriginalName -eq 'winawdl_regproxy.inf' -and $_.ProviderName -eq 'winairdrop project' } |
    ForEach-Object {
      pnputil /delete-driver $_.DriverName /uninstall /force | Out-Null
      if ($LASTEXITCODE -ne 0) { throw "pnputil /delete-driver $($_.DriverName) exited $LASTEXITCODE" }
    }
  foreach ($s in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$s" | Where-Object { $_.Subject -eq $CertSubject } | Remove-Item -ErrorAction Stop
  }
}

Try-Step 'Firewall' {
  Get-NetFirewallRule -Group 'Airdraft AirDrop' -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction Stop
}

# Only the adapter setup-tap.ps1 made and named "Airdraft"; the TAP-Windows
# driver stays, other programs (VPNs) may use it (#22).
Try-Step 'TAP adapter' {
  foreach ($a in @(Get-NetAdapter -Name 'Airdraft' -IncludeHidden -ErrorAction SilentlyContinue |
                   Where-Object { $_.InterfaceDescription -like 'TAP-Windows Adapter V9*' })) {
    if ($a.PnPDeviceID) {
      pnputil /remove-device $a.PnPDeviceID | Out-Null
      if ($LASTEXITCODE -ne 0) { throw "pnputil /remove-device exited $LASTEXITCODE" }
    }
  }
}

Try-Step 'Files' {
  Remove-Item -LiteralPath (Join-Path ([Environment]::GetFolderPath('CommonStartup')) 'Airdraft.lnk') -ErrorAction SilentlyContinue
  $programs = [Environment]::GetFolderPath('CommonPrograms')
  foreach ($name in 'Airdraft', 'Airdraft - restore Windows Wi-Fi', 'Airdraft - diagnostic report') {
    Remove-Item -LiteralPath (Join-Path $programs ($name + '.lnk')) -ErrorAction SilentlyContinue
  }
  foreach ($dir in $Target, "$env:ProgramData\Airdraft", "$env:LOCALAPPDATA\Airdraft") {
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force -ErrorAction Stop }
  }
}
if ($failures) { "Airdraft uninstall had $failures failure(s). See $Log."; exit 1 } else { 'Airdraft is removed.' }
