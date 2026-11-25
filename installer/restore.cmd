@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0restore.ps1" -Elevate %*
set "AIRDRAFT_RESTORE_EXIT=%errorlevel%"
if /I "%~1"=="-SelfTest" exit /b %AIRDRAFT_RESTORE_EXIT%
if not "%AIRDRAFT_RESTORE_EXIT%"=="0" echo Airdraft recovery did not complete. Check %ProgramData%\Airdraft\restore.log for details.
echo.
pause
exit /b %AIRDRAFT_RESTORE_EXIT%
