@echo off
rem Configure and build with CMake (Release). Output: bin\rvc_harness.exe, log: logs\build.log
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
(cmake -S . -B build && cmake --build build --config Release) > logs\build.log 2>&1
set RC=%ERRORLEVEL%
type logs\build.log
if not "%RC%"=="0" echo BUILD FAILED (%RC%)
exit /b %RC%
