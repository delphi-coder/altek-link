@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "MODE=%~1"
set "TARGET=%~2"
if "%MODE%"=="" set "MODE=release"
if "%TARGET%"=="" set "TARGET=all"
if /i "%MODE%"=="clean" goto clean
if /i not "%MODE%"=="release" if /i not "%MODE%"=="debug" goto usage
if /i not "%TARGET%"=="all" if /i not "%TARGET%"=="x86" if /i not "%TARGET%"=="x64" goto usage
if /i "%TARGET%"=="x64" goto build_x64
call build-vs2010.bat x86 %MODE%
if errorlevel 1 exit /b 1
if /i "%TARGET%"=="x86" exit /b 0
:build_x64
call build-tdm.bat x64 %MODE%
if errorlevel 1 exit /b 1
exit /b 0
:clean
if exist build rmdir /s /q build
del /q "bin\x86\AltekLink.exe" "bin\x64\AltekLink.exe" 2>nul
del /q "bin\debug\x86\AltekLink.exe" "bin\debug\x64\AltekLink.exe" 2>nul
del /q "bin\debug\x86\AltekLink.pdb" "bin\debug\x64\AltekLink.pdb" 2>nul
del /q "bin\x86\screen.h264" "bin\x64\screen.h264" 2>nul
del /q "bin\debug\x86\screen.h264" "bin\debug\x64\screen.h264" 2>nul
del /q vc*.pdb 2>nul
if exist "bin\x86\AltekLink.exe" goto clean_locked
if exist "bin\x64\AltekLink.exe" goto clean_locked
if exist "bin\debug\x86\AltekLink.exe" goto clean_locked
if exist "bin\debug\x64\AltekLink.exe" goto clean_locked
echo Windows generated files removed.
exit /b 0
:clean_locked
echo ERROR: AltekLink.exe is still running or locked. Close it and run build.bat clean again.
exit /b 1
:usage
echo Usage: build.bat [release^|debug^|clean] [all^|x86^|x64]
exit /b 1
