@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0\.."
set "CONFIG_DIR=%~1"
if "%CONFIG_DIR%"=="" set "CONFIG_DIR=build\config\debug"
if not exist ".local" mkdir ".local"
if exist ".local\udphost" goto read_host
set /p "ALTEK_UDP_HOST=UDP log receiver IPv4: "
if not defined ALTEK_UDP_HOST goto bad_host
echo(%ALTEK_UDP_HOST%| findstr /r /x "[0-9][0-9.]*" >nul
if errorlevel 1 goto bad_host
>".local\udphost" echo %ALTEK_UDP_HOST%
:read_host
set /p "ALTEK_UDP_HOST="<".local\udphost"
if not defined ALTEK_UDP_HOST goto bad_host
if not exist "%CONFIG_DIR%" mkdir "%CONFIG_DIR%"
>"%CONFIG_DIR%\build_config.h" echo #define ALTEK_DEBUG_UDP_HOST "%ALTEK_UDP_HOST%"
exit /b 0
:bad_host
echo ERROR: Debug UDP host must contain only an IPv4 address.
exit /b 1
