@echo off
rem Builds bin\floor.exe (floor.cpp: the pieces of an interpreter step timed on the card) with MSVC, fxc2's compiler beside it.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VS=%%i"
if not defined VS echo Visual Studio not found & exit /b 1
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "OUT=%~dp0..\..\..\bin"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /O2 /EHsc /W3 /std:c++17 /Fo"%OUT%\\" /Fe"%OUT%\floor.exe" "%~dp0floor.cpp" d3d11.lib d3dcompiler.lib user32.lib
if errorlevel 1 exit /b 1
copy /y "%~dp0..\..\..\tools\fxc2\d3dcompiler_47.dll" "%OUT%\" >nul
exit /b 0
