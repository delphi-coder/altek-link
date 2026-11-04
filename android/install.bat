@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
set "SDK=E:\sdk"
if defined ALTEK_SDK set "SDK=%ALTEK_SDK%"
set "BT=%SDK%\build-tools\33.0.1"
set "MODE=%~1"
if "%MODE%"=="" if exist ".local\last-build" set /p "MODE="<".local\last-build"
if "%MODE%"=="" if exist "bin\ALTEK-Link-0.1.16-debug.apk" if not exist "bin\ALTEK-Link-0.1.16-release.apk" set "MODE=debug"
if "%MODE%"=="" set "MODE=release"
if /i not "%MODE%"=="release" if /i not "%MODE%"=="debug" goto usage
set "APK=bin\ALTEK-Link-0.1.16-%MODE%.apk"
echo Selected %MODE% APK: %APK%
if not exist "%APK%" goto missing_apk
if not exist "%BT%\lib\apksigner.jar" goto missing_signer
if not exist "%SDK%\platform-tools\adb.exe" goto missing_adb
where java >nul 2>nul
if errorlevel 1 goto missing_java

java -jar "%BT%\lib\apksigner.jar" verify --min-sdk-version 16 --verbose "%APK%"
if errorlevel 1 goto bad_signature

"%SDK%\platform-tools\adb.exe" devices
set "INSTALL_LOG=%TEMP%\altek-install-%RANDOM%-%RANDOM%.log"
"%SDK%\platform-tools\adb.exe" install --no-incremental -r "%APK%" >"%INSTALL_LOG%" 2>&1
set "ADB_RESULT=%ERRORLEVEL%"
type "%INSTALL_LOG%"
if not "%ADB_RESULT%"=="0" goto install_failed
findstr /l /i /c:"Failure [" /c:"error:" "%INSTALL_LOG%" >nul
if not errorlevel 1 goto install_failed
findstr /r /c:"^Success[ ]*$" "%INSTALL_LOG%" >nul
if errorlevel 1 goto install_failed
del /q "%INSTALL_LOG%" 2>nul
echo Installed successfully. Open ALTEK Link.
endlocal
exit /b 0

:missing_apk
echo ERROR: %APK% is missing. Run build.bat %MODE% first.
goto failure
:missing_signer
echo ERROR: Android signature verifier is missing: %BT%\lib\apksigner.jar
goto failure
:missing_adb
echo ERROR: adb.exe is missing: %SDK%\platform-tools\adb.exe
goto failure
:missing_java
echo ERROR: Java is required for APK signature verification.
goto failure
:bad_signature
echo ERROR: APK signature verification failed. Nothing was installed.
goto failure
:install_failed
echo Installation was NOT confirmed. Log retained at: %INSTALL_LOG%
goto failure
:usage
echo Usage: install.bat [release^|debug]
goto failure
:failure
echo INSTALL FAILED. No success message will be printed and existing app data was not removed.
endlocal
exit /b 1
