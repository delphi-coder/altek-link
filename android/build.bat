@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
set "MODE=%~1"
if "%MODE%"=="" set "MODE=release"
if /i "%MODE%"=="clean" goto clean
if /i not "%MODE%"=="release" if /i not "%MODE%"=="debug" goto usage

set "SDK=E:\sdk"
if defined ALTEK_SDK set "SDK=%ALTEK_SDK%"
set "BT=%SDK%\build-tools\33.0.1"
set "PLATFORM=%SDK%\platforms\android-34\android.jar"
set "MODE_SOURCE=modes\%MODE%\ir\altek\link\DebugLog.java"
set "OUTPUT=bin\ALTEK-Link-0.1.17-%MODE%.apk"
set "JAVAC_DEBUG=-g:none"
set "D8_MODE=--release"

if /i "%MODE%"=="debug" goto configure_debug
goto mode_ready

:configure_debug
set "JAVAC_DEBUG=-g"
set "D8_MODE=--debug"
if not exist ".local" mkdir ".local"
if exist ".local\udphost" goto read_debug_host
set /p "ALTEK_UDP_HOST=UDP log receiver IPv4: "
if not defined ALTEK_UDP_HOST goto badhost
echo(%ALTEK_UDP_HOST%| findstr /r /x "[0-9][0-9.]*" >nul
if errorlevel 1 goto badhost
>".local\udphost" echo %ALTEK_UDP_HOST%
:read_debug_host
set /p "ALTEK_UDP_HOST="<".local\udphost"
if not defined ALTEK_UDP_HOST goto badhost

:mode_ready

if not exist "%PLATFORM%" (
 echo ERROR: Missing %PLATFORM%
 goto fail
)
if not exist "%BT%\aapt.exe" (
 echo ERROR: Build-tools 33.0.1 not found in %SDK%
 goto fail
)
where javac >nul 2>nul
if errorlevel 1 (
 echo ERROR: javac not found. Add your JDK bin directory to PATH. JDK 8 or 17 recommended.
 goto fail
)
if not defined ALTEK_KEYSTORE if exist ".local\signing.bat" call ".local\signing.bat"
if not defined ALTEK_KEYSTORE (
 echo ERROR: Set ALTEK_KEYSTORE or create .local\signing.bat from signing.example.bat.
 goto fail
)
if not defined ALTEK_KEY_ALIAS (
 echo ERROR: Set ALTEK_KEY_ALIAS to the signing-key alias.
 goto fail
)
if not defined ALTEK_KEYSTORE_PASS (
 echo ERROR: Set ALTEK_KEYSTORE_PASS to the keystore password.
 goto fail
)
if not defined ALTEK_KEY_PASS set "ALTEK_KEY_PASS=%ALTEK_KEYSTORE_PASS%"
if not exist "%ALTEK_KEYSTORE%" (
 echo ERROR: Signing keystore not found: %ALTEK_KEYSTORE%
 goto fail
)

call :clean_generated
mkdir build
if errorlevel 1 goto fail
mkdir gen
if errorlevel 1 goto fail
mkdir bin 2>nul
"%BT%\aapt.exe" package -f -m -J gen -M AndroidManifest.xml -S res -I "%PLATFORM%"
if errorlevel 1 goto fail
if /i "%MODE%"=="debug" (
 >"gen\ir\altek\link\DebugUdpConfig.java" echo package ir.altek.link;
 >>"gen\ir\altek\link\DebugUdpConfig.java" echo final class DebugUdpConfig {
 >>"gen\ir\altek\link\DebugUdpConfig.java" echo   static final String HOST = "%ALTEK_UDP_HOST%";
 >>"gen\ir\altek\link\DebugUdpConfig.java" echo }
)
javac %JAVAC_DEBUG% -encoding UTF-8 -Xlint:-options -source 7 -target 7 -bootclasspath "%PLATFORM%" -cp ".;%PLATFORM%" -d build src\ir\altek\link\*.java "%MODE_SOURCE%" gen\ir\altek\link\*.java
if errorlevel 1 goto fail
java -cp "%BT%\lib\d8.jar" com.android.tools.r8.D8 build\ir\altek\link\*.class %D8_MODE% --lib "%PLATFORM%" --min-api 16 --output build
if errorlevel 1 goto fail
"%BT%\aapt.exe" package -f -M AndroidManifest.xml -S res -I "%PLATFORM%" -F app-unsigned.apk
if errorlevel 1 goto fail
jar uf app-unsigned.apk -C build classes.dex
if errorlevel 1 goto fail
"%BT%\zipalign.exe" -f 4 app-unsigned.apk app-stage.apk
if errorlevel 1 goto fail
java -jar "%BT%\lib\apksigner.jar" sign --ks "%ALTEK_KEYSTORE%" --ks-key-alias "%ALTEK_KEY_ALIAS%" --ks-pass env:ALTEK_KEYSTORE_PASS --key-pass env:ALTEK_KEY_PASS --min-sdk-version 16 --v1-signing-enabled true --v2-signing-enabled true --out app-signed.apk app-stage.apk
if errorlevel 1 goto fail
java -jar "%BT%\lib\apksigner.jar" verify --min-sdk-version 16 --verbose app-signed.apk
if errorlevel 1 goto fail
move /y app-signed.apk "%OUTPUT%" >nul
if errorlevel 1 goto fail
del /q app-unsigned.apk app-stage.apk app-signed.apk.idsig 2>nul
if not exist ".local" mkdir ".local"
>".local\last-build" echo %MODE%
echo.
echo SUCCESS: %CD%\%OUTPUT%
echo ALTEK Link 0.1.17 %MODE% build completed.
exit /b 0

:clean
call :clean_generated
if exist bin rmdir /s /q bin
echo Android generated files removed.
exit /b 0

:clean_generated
del /q app-unsigned.apk app-stage.apk app-signed.apk app-signed.apk.idsig app-aligned.apk app-aligned.apk.idsig 2>nul
if exist build rmdir /s /q build
if exist gen rmdir /s /q gen
exit /b 0

:badhost
echo ERROR: Debug UDP host must contain only an IPv4 address.
exit /b 1

:usage
echo Usage: build.bat [release^|debug^|clean]
exit /b 1

:fail
echo.
echo BUILD FAILED. Do not install an older APK by mistake.
exit /b 1
