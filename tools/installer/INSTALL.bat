@echo off
setlocal DisableDelayedExpansion
if "%~1"=="" goto picker
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Install.ps1" -GameDirectory "%~1"
goto done
:picker
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Install.ps1"
:done
set "installExit=%errorlevel%"
echo.
pause
exit /b %installExit%
