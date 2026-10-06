@echo off
rem Builds the Linux programs: static RV32IMA binaries with no C library (libc.c stands in) and
rem compiler-rt's soft-float routines. Output: programs\bin\<name> (ELF). Needs LLVM and Python.
setlocal
cd /d "%~dp0.."
if "%LLVM_BIN%"=="" set "LLVM_BIN=C:\Program Files\LLVM\bin"
python linux\fetch.py || exit /b 1
set CFLAGS=--target=riscv32-unknown-elf -march=rv32ima -mabi=ilp32 -O2 -ffreestanding -fno-builtin -mno-relax -nostdlib -static -fuse-ld=lld -w -Ilinux\include
rem glxgears: the stock source, our OpenGL driver, the runtime
"%LLVM_BIN%\clang.exe" %CFLAGS% -Wl,-e,_start -Wl,-Ttext=0x10000 build\fetch\glxgears.c linux\gl.c linux\libc.c build\fetch\builtins\*.c -o bin\glxgears || exit /b 1
echo built bin\glxgears
