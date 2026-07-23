@echo off
setlocal

rem Native Windows entry point. The implementation lives in PowerShell so
rem paths containing spaces and external-tool failures can be handled safely.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
