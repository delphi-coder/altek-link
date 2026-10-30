@echo off
rem Optional: right-click Run as administrator. Rule is private-network and local-subnet only.
netsh advfirewall firewall add rule name="ALTEK Link UDP debug 45879" dir=in action=allow protocol=UDP localport=45879 profile=private remoteip=localsubnet
pause
