# The real-work bench on gpu_vm's tick on the card (README.md): CoreMark bare-metal at the two counts, then gpu_vm's
# Linux with build.sh's initramfs (init.c runs CoreMark twice, gzip and gunzip), every line stamped by stamp.py.
#   pwsh gpu_vm\bench_real\run_gvm.ps1 [-Iter1 60] [-Iter2 180] [-Extra '--profile']
#   pwsh gpu_vm\bench_real\run_gvm.ps1 -Trace     Linux alone with --trace-pass into build\bench_real\trace_pass.log,
#                                                  then passes.py's table (not a timing)
param([int]$Iter1 = 60, [int]$Iter2 = 180, [string[]]$Extra = @('--stats'), [switch]$Trace)
$here = $PSScriptRoot
$root = (Resolve-Path "$here\..\..").Path
$b = "$root\build\bench_real"
Push-Location $root
try {
    if ($Trace) {
        & pwsh "$root\tools\gpu_queue.ps1" cmd /c "bin\gvm_gpu.exe --trace-pass --no-stdin --until BENCH-DONE --dtb $b\gvm\gvm.dtb $b\gvm\vmlinux > $b\trace_pass.log 2>&1"
        python "$here\passes.py" "$b\trace_pass.log"
        return
    }
    foreach ($n in $Iter1, $Iter2) {
        "== bare-metal CoreMark, $n iterations"
        & pwsh "$root\tools\gpu_queue.ps1" python "$here\stamp.py" --log "$b\bare$n.log" -- "$root\bin\gvm_gpu.exe" @Extra `
            "$b\bare\$n\coremark.elf"
    }
    "== Linux"
    & pwsh "$root\tools\gpu_queue.ps1" python "$here\stamp.py" --log "$b\gvm_linux.log" --until 'BENCH-DONE' -- `
        "$root\bin\gvm_gpu.exe" @Extra --no-stdin --dtb "$b\gvm\gvm.dtb" "$b\gvm\vmlinux"
} finally { Pop-Location }
