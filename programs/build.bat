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
for %%P in (raytrace raycast gears rects blend sound classes) do (
    "%LLVM_BIN%\clang.exe" %CFLAGS% common\start.S %%P\%%P.c -o build\%%P.elf || exit /b 1
    "%LLVM_BIN%\llvm-objcopy.exe" -O binary build\%%P.elf bin\%%P.bin || exit /b 1
    echo built bin\%%P.bin
)
rem fpcheck uses the F extension (the full machine has it)
"%LLVM_BIN%\clang.exe" %CFLAGS:rv32ima=rv32imaf% common\start.S fpcheck\fpcheck.c -o build\fpcheck.elf || exit /b 1
"%LLVM_BIN%\llvm-objcopy.exe" -O binary build\fpcheck.elf bin\fpcheck.bin || exit /b 1
echo built bin\fpcheck.bin
rem lazy runs in place of the kernel, in supervisor mode (tools\make_lazy_image.py)
"%LLVM_BIN%\clang.exe" %CFLAGS:common\link.ld=lazy\link.ld% common\start.S lazy\lazy.c -o build\lazy.elf || exit /b 1
"%LLVM_BIN%\llvm-objcopy.exe" -O binary build\lazy.elf bin\lazy.bin || exit /b 1
echo built bin\lazy.bin
rem copytest too: the parallel copy over the shader machine's chunk (docs\machine\gpu.md, "RAM in one texture")
"%LLVM_BIN%\clang.exe" %CFLAGS:common\link.ld=lazy\link.ld% common\start.S copytest\copytest.c -o build\copytest.elf || exit /b 1
"%LLVM_BIN%\llvm-objcopy.exe" -O binary build\copytest.elf bin\copytest.bin || exit /b 1
echo built bin\copytest.bin
rem gpubench0-8: the GPU's fragment cost a mode, in the kernel's place too (docs\machine\gpu.md, "VRAM")
for %%M in (0 1 2 3 4 5 6 7 8) do (
    "%LLVM_BIN%\clang.exe" %CFLAGS:common\link.ld=lazy\link.ld% -DMODE=%%M common\start.S gpubench\gpubench.c -o build\gpubench%%M.elf || exit /b 1
    "%LLVM_BIN%\llvm-objcopy.exe" -O binary build\gpubench%%M.elf bin\gpubench%%M.bin || exit /b 1
)
echo built bin\gpubench0-8.bin
