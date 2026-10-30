@echo off
setlocal
cd /d "%~dp0.."
echo Run this optional script as Administrator if Windows Firewall blocks discovery or calls.
netsh advfirewall firewall add rule name="ALTEK Link x64 local TCP" dir=in action=allow program="%CD%\bin\x64\AltekLink.exe" protocol=TCP localport=45871 remoteip=LocalSubnet profile=private
if errorlevel 1 exit /b 1
netsh advfirewall firewall add rule name="ALTEK Link x64 discovery" dir=in action=allow program="%CD%\bin\x64\AltekLink.exe" protocol=UDP localport=45872 remoteip=LocalSubnet profile=private
if errorlevel 1 exit /b 1
netsh advfirewall firewall add rule name="ALTEK Link x86 local TCP" dir=in action=allow program="%CD%\bin\x86\AltekLink.exe" protocol=TCP localport=45871 remoteip=LocalSubnet profile=private
if errorlevel 1 exit /b 1
netsh advfirewall firewall add rule name="ALTEK Link x86 discovery" dir=in action=allow program="%CD%\bin\x86\AltekLink.exe" protocol=UDP localport=45872 remoteip=LocalSubnet profile=private
if errorlevel 1 exit /b 1
echo Rules added for Private networks on the local subnet.
pause
