@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "ALTEK_ARCH=%~1"
set "ALTEK_MODE=%~2"
if "%ALTEK_ARCH%"=="" set "ALTEK_ARCH=x64"
if "%ALTEK_MODE%"=="" set "ALTEK_MODE=release"
if /i not "%ALTEK_ARCH%"=="x86" if /i not "%ALTEK_ARCH%"=="x64" goto usage
if /i not "%ALTEK_MODE%"=="release" if /i not "%ALTEK_MODE%"=="debug" goto usage
if /i "%ALTEK_MODE%"=="debug" (
 call tools\prepare-debug-host.bat build\config\debug
 if errorlevel 1 exit /b 1
)
where gcc >nul 2>nul
if errorlevel 1 (
 echo GCC is not in PATH. Open the TDM-GCC command prompt first.
 exit /b 1
)
mingw32-make -k ARCH=%ALTEK_ARCH% MODE=%ALTEK_MODE%
if errorlevel 1 (
 echo BUILD FAILED. Copy the compiler output for diagnosis.
 exit /b 1
)
if /i "%ALTEK_MODE%"=="debug" (
 echo Built bin\debug\%ALTEK_ARCH%\AltekLink.exe [debug]
) else (
 echo Built bin\%ALTEK_ARCH%\AltekLink.exe [release]
)
exit /b 0
:usage
echo Usage: build-tdm.bat [x86^|x64] [release^|debug]
exit /b 1
