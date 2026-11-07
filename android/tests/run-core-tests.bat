@echo off
setlocal
cd /d "%~dp0.."
set "SDK=E:\sdk"
if defined ALTEK_SDK set "SDK=%ALTEK_SDK%"
set "PLATFORM=%SDK%\platforms\android-17\android.jar"
if not exist build\ir\altek\link\Wire.class (
 echo Run build.bat first.
 pause
 exit /b 1
)
javac -encoding UTF-8 -source 7 -target 7 -cp "build;%PLATFORM%" -d build tests\CoreTest.java tests\UiLogicTest.java tests\RemoteInputTest.java
if errorlevel 1 goto fail
java -cp "build;%PLATFORM%" ir.altek.link.UiLogicTest
if errorlevel 1 goto fail
java -cp "build;%PLATFORM%" ir.altek.link.RemoteInputTest
if errorlevel 1 goto fail
java -cp "build;%PLATFORM%" ir.altek.link.CoreTest "%TEMP%\altek-link-core-test"
if errorlevel 1 goto fail
pause
exit /b 0
:fail
pause
exit /b 1
