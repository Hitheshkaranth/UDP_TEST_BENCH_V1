@echo off
rem Run once as Administrator on the RECEIVER PC to allow incoming test traffic.
netsh advfirewall firewall add rule name="UDP Bandwidth Tester" dir=in action=allow protocol=UDP localport=5201
pause
