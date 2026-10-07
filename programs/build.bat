@echo off
rem Builds the bare-metal programs into programs\bin\<name>.bin (raw memory images loaded at
rem 0x80000000). Needs LLVM (clang, lld, llvm-objcopy) with the RISC-V target; set LLVM_BIN to
rem its bin folder if it is not in "C:\Program Files\LLVM\bin".
setlocal
cd /d "%~dp0"
if "%LLVM_BIN%"=="" set "LLVM_BIN=C:\Program Files\LLVM\bin"
set CFLAGS=--target=riscv32-unknown-elf -march=rv32ima -mabi=ilp32 -O2 -ffreestanding -fno-builtin -nostdlib -static -fuse-ld=lld -Wall -Wl,-T,common\link.ld
if not exist bin mkdir bin
if not exist build mkdir build
for %%P in (raytrace raycast gears rects blend sound) do (
    "%LLVM_BIN%\clang.exe" %CFLAGS% common\start.S %%P\%%P.c -o build\%%P.elf || exit /b 1
    "%LLVM_BIN%\llvm-objcopy.exe" -O binary build\%%P.elf bin\%%P.bin || exit /b 1
    echo built bin\%%P.bin
)
