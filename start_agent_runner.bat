@echo off
rem Start the runner that lets Claude trigger builds and test runs. Leave this window open.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\agent_runner.ps1"
