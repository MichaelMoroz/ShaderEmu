@echo off
rem Boot rvc Linux in the headless harness. Extra arguments are passed through, e.g.
rem   run_rvc.bat --warp --until "login:" --seconds 600
cd /d "%~dp0"
if not exist logs mkdir logs
bin\rvc_harness.exe --rvc rvc\_Nix\rvc --uart-log logs\uart.log %*
