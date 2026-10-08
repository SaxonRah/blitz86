@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0b86.ps1" %*
exit /b %ERRORLEVEL%
