@echo off
rem Builds bin\sxbench.exe with MSVC (found through vswhere): SX's tick against the reference (sx/tick/README.md).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VS=%%i"
if not defined VS echo Visual Studio not found & exit /b 1
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "OUT=%~dp0..\..\bin"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /O2 /EHsc /W3 /std:c++17 /Fo"%OUT%\\" /Fe"%OUT%\sxbench.exe" "%~dp0sxbench.cpp" d3d11.lib d3dcompiler.lib dxguid.lib dxgi.lib user32.lib
if errorlevel 1 exit /b 1
rem sxtrace12: the tick on D3D12 for Nsight Graphics (DXC with debug information)
cl /nologo /O2 /EHsc /W3 /std:c++17 /Fo"%OUT%\\" /Fe"%OUT%\sxtrace12.exe" "%~dp0sxtrace12.cpp" d3d12.lib d3dcompiler.lib dxgi.lib dxguid.lib
if errorlevel 1 exit /b 1
rem the HLSL compiler is fxc2 (docs/machine/fxc2.md): its d3dcompiler_47.dll beside the exe
copy /y "%~dp0..\..\tools\fxc2\d3dcompiler_47.dll" "%OUT%\" >nul
exit /b 0
