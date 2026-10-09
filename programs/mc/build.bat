@echo off
rem Builds the worker cores' test (docs\multicore.md): worker.c as a raw image linked to run in
rem the arena, and mctest, the Linux program that carries it (programs\bin\mctest). Needs LLVM
rem and Python, as programs\linux\build.bat does.
setlocal
cd /d "%~dp0.."
if "%LLVM_BIN%"=="" set "LLVM_BIN=C:\Program Files\LLVM\bin"
python linux\fetch.py || exit /b 1
if not exist build mkdir build
set BASE=--target=riscv32-unknown-elf -march=rv32ima -mabi=ilp32 -O2 -ffreestanding -fno-builtin -mno-relax -nostdlib -static -fuse-ld=lld -Wall
"%LLVM_BIN%\clang.exe" %BASE% -Wl,-T,mc\worker.ld -Wl,--oformat=binary mc\worker.c -o build\mc_worker.bin || exit /b 1
python -c "d=open('build/mc_worker.bin','rb').read(); d+=bytes(-len(d)%%4); open('build/mc_worker.h','w').write('static const unsigned char mc_worker_code[] __attribute__((aligned(4))) = {'+','.join(map(str,d))+'};\n')" || exit /b 1
"%LLVM_BIN%\clang.exe" %BASE% -w -Ilinux\include -Ibuild -Imc -Wl,-e,_start -Wl,-Ttext=0x10000 mc\mctest.c mc\mcw.c linux\libc.c build\fetch\builtins\*.c -o bin\mctest || exit /b 1
echo built bin\mctest
