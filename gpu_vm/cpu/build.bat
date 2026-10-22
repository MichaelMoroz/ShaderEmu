@echo off
rem Builds bin\gvm_cpu.exe and bin\gvm_cpu_mk.exe (the tests' generator) with MSVC, found through vswhere
rem (gpu_vm/cpu/README.md).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VS=%%i"
if not defined VS echo Visual Studio not found & exit /b 1
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "OUT=%~dp0..\..\bin"
if not exist "%OUT%\gvm_cpu_obj" mkdir "%OUT%\gvm_cpu_obj"
cl /nologo /O2 /W3 /TC /Fo"%OUT%\gvm_cpu_obj\\" /Fe"%OUT%\gvm_cpu.exe" "%~dp0gvm_cpu.c"
if errorlevel 1 exit /b 1
cl /nologo /O1 /EHsc /W3 /std:c++17 /Fo"%OUT%\gvm_cpu_obj\\" /Fe"%OUT%\gvm_cpu_mk.exe" "%~dp0tests\mk.cpp"
if errorlevel 1 exit /b 1
exit /b 0
