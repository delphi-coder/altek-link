@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "ALTEK_ARCH=%~1"
set "ALTEK_MODE=%~2"
if "%ALTEK_ARCH%"=="" set "ALTEK_ARCH=x86"
if "%ALTEK_MODE%"=="" set "ALTEK_MODE=release"
if /i not "%ALTEK_MODE%"=="release" if /i not "%ALTEK_MODE%"=="debug" goto usage
if /i "%ALTEK_ARCH%"=="x64" (
 set "ALTEK_VCARCH=amd64"
 set "ALTEK_EXPECT=ALTEK_EXPECT_X64"
 if exist "%VS100COMNTOOLS%..\..\VC\bin\x86_amd64\cl.exe" set "ALTEK_VCARCH=x86_amd64"
) else if /i "%ALTEK_ARCH%"=="x86" (
 set "ALTEK_VCARCH=x86"
 set "ALTEK_EXPECT=ALTEK_EXPECT_X86"
) else goto usage
set "ALTEK_OBJ=build\%ALTEK_MODE%\vs-%ALTEK_ARCH%"
set "ALTEK_OUT=bin\%ALTEK_ARCH%"
set "ALTEK_CFLAGS=/O2 /DNDEBUG"
set "ALTEK_CONFIG_INCLUDE="
if /i "%ALTEK_MODE%"=="debug" (
 set "ALTEK_OUT=bin\debug\%ALTEK_ARCH%"
 set "ALTEK_CFLAGS=/Od /Zi /DALTEK_DEBUG_UDP_LOG"
 set "ALTEK_CONFIG_INCLUDE=/Ibuild\config\debug"
 call tools\prepare-debug-host.bat build\config\debug
 if errorlevel 1 exit /b 1
)
if defined VS100COMNTOOLS call "%VS100COMNTOOLS%..\..\VC\vcvarsall.bat" %ALTEK_VCARCH%
where cl >nul 2>nul
if errorlevel 1 (
 echo Visual Studio 2010 compiler was not found.
 exit /b 1
)
if not exist "%ALTEK_OBJ%" mkdir "%ALTEK_OBJ%"
if not exist "%ALTEK_OUT%" mkdir "%ALTEK_OUT%"
rc /nologo /I assets /fo "%ALTEK_OBJ%\app.res" assets\app.rc
if errorlevel 1 exit /b 1
cl /nologo /TC /W3 %ALTEK_CFLAGS% /Fd"%ALTEK_OBJ%\vc100.pdb" /MT /D_CRT_SECURE_NO_WARNINGS /D%ALTEK_EXPECT% %ALTEK_CONFIG_INCLUDE% /Iinclude /Ithird_party\openh264 /Fo"%ALTEK_OBJ%\\" src\*.c /Fe"%ALTEK_OUT%\AltekLink.exe" /link /OPT:REF /SUBSYSTEM:WINDOWS,6.01 "%ALTEK_OBJ%\app.res" ws2_32.lib secur32.lib crypt32.lib advapi32.lib iphlpapi.lib ole32.lib oleaut32.lib uuid.lib user32.lib usp10.lib gdi32.lib comdlg32.lib comctl32.lib shell32.lib winmm.lib msimg32.lib windowscodecs.lib mf.lib mfplat.lib mfuuid.lib
if errorlevel 1 (
 echo BUILD FAILED. Close a running AltekLink.exe and retry if the output is locked.
 exit /b 1
)
echo Built %ALTEK_OUT%\AltekLink.exe [%ALTEK_MODE%]
exit /b 0
:usage
echo Usage: build-vs2010.bat [x86^|x64] [release^|debug]
exit /b 1
