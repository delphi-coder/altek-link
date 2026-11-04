@echo off
setlocal
cd /d "%~dp0"
set "ALTEK_ARCH=x86"
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "ALTEK_ARCH=x64"
if defined PROCESSOR_ARCHITEW6432 set "ALTEK_ARCH=x64"
start "" "%~dp0bin\%ALTEK_ARCH%\AltekLink.exe"
