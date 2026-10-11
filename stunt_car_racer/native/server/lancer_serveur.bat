@echo off
rem Serveur relais Stunt Car Racer : journal dans scr_relay.log
cd /d "%~dp0"
scr_relay.exe --log scr_relay.log
pause
