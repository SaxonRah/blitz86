@echo off
rem blitz86: every command goes through scripts\b86.ps1. "b86 help" lists them.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\b86.ps1" %*
exit /b %ERRORLEVEL%
