@echo off
setlocal
set "AIRDRAFT_SETUP_SCRIPT=%~dp0install.ps1"
powershell.exe -NoProfile -Command "try { $q = [char]34; $argsForSetup = '-NoProfile -ExecutionPolicy Bypass -File ' + $q + $env:AIRDRAFT_SETUP_SCRIPT + $q; exit (Start-Process powershell.exe -Verb RunAs -ArgumentList $argsForSetup -Wait -PassThru).ExitCode } catch { Write-Host $_; exit 1 }"
if errorlevel 1 (
  echo Airdraft Setup did not complete. Check the setup window or %%ProgramData%%\Airdraft\install-error.txt.
  pause
  exit /b 1
)
start "" "%ProgramFiles%\Airdraft\Airdraft.exe"
