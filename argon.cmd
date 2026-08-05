@echo off
rem ArgonOS - one entry point for everything.
rem
rem   argon help
rem   argon build
rem   argon run            boot the emulator with the console in this window
rem   argon run -tcp       expose the console on a TCP port instead
rem   argon test           automated boot test, prints the resulting screen
rem   argon tests          host unit tests
rem   argon flash -port COM5
rem
rem This is a .cmd rather than a .ps1 on purpose: Windows refuses to run
rem PowerShell scripts by default, and a batch file can ask for an exception
rem for itself without anyone having to change a machine-wide setting.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\argon.ps1" %*
exit /b %ERRORLEVEL%
