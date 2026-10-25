@echo off
rem Builds bin\waves.exe with MSVC (found through vswhere): the wave probe (gpu_vm/tick/WAVES.md).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VS=%%i"
if not defined VS echo Visual Studio not found & exit /b 1
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "OUT=%~dp0..\..\..\bin"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /O2 /EHsc /W3 /std:c++17 /Fo"%OUT%\\" /Fe"%OUT%\waves.exe" "%~dp0waves.cpp" d3d12.lib dxgi.lib dxguid.lib
exit /b %errorlevel%
