@echo off
rem Builds the worker cores' test (docs\multicore.md): mctest, a Linux program whose own
rem functions the workers run, with the library every such program links (mcw.c). Output:
rem programs\bin\mctest. Needs LLVM and Python, as programs\linux\build.bat does.
setlocal
cd /d "%~dp0.."
if "%LLVM_BIN%"=="" set "LLVM_BIN=C:\Program Files\LLVM\bin"
python linux\fetch.py || exit /b 1
set BASE=--target=riscv32-unknown-elf -march=rv32imaf -mabi=ilp32 -O2 -ffreestanding -fno-builtin -mno-relax -nostdlib -static -fuse-ld=lld -Wall
"%LLVM_BIN%\clang.exe" %BASE% -w -Ilinux\include -Imc -Wl,-e,_start -Wl,-Ttext=0x10000 mc\mctest.c mc\mcw.c linux\libc.c build\fetch\builtins\*.c -o bin\mctest || exit /b 1
echo built bin\mctest
