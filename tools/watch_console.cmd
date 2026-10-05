@echo off
rem Opens a terminal window that follows the emulator console (logs\uart.log) live.
start "emulator console" pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0watch_console.ps1"
