# The real-work bench on the RISC-V machine's lean tick (README.md): boots the private image lean_bench of the main
# checkout under rvc_harness, runs CoreMark twice, gzip and gunzip at the prompt, each line stamped by stamp.py.
#   pwsh gpu_vm\bench_real\run_lean.ps1 [-Main E:\Development\Emu\ShaderEmu] [-Log FILE] [-Iter1 60] [-Iter2 180]
param([string]$Main = 'E:\Development\Emu\ShaderEmu', [string]$Log = '', [int]$Iter1 = 60, [int]$Iter2 = 180)
$here = $PSScriptRoot
$root = (Resolve-Path "$here\..\..").Path
if (-not $Log) { $Log = "$root\build\bench_real\lean.log" }
$send = "echo BENCH-START coremark-$Iter1; coremark 0x0 0x0 0x66 $Iter1; echo BENCH-END coremark-$Iter1; " +
        "echo BENCH-START coremark-$Iter2; coremark 0x0 0x0 0x66 $Iter2; echo BENCH-END coremark-$Iter2; " +
        "echo BENCH-START gzip; time minigzip < /usr/share/bench/input > /tmp/input.gz; echo BENCH-END gzip; " +
        "echo BENCH-START gunzip; time minigzip -d < /tmp/input.gz > /tmp/output; echo BENCH-END gunzip; " +
        "ls -l /tmp/input.gz; cmp /usr/share/bench/input /tmp/output && echo BENCH gunzip the same; echo BENCH-D''ONE\n"
Push-Location $Main
try {
    & pwsh "$root\tools\gpu_queue.ps1" python "$here\stamp.py" --log $Log --until 'BENCH-DONE' -- `
        "$Main\bin\rvc_harness.exe" --d3d11 --rvc experiments\rvc_lean --image build\images\lean_bench --no-stdin `
        --expect 'laid out 255' --send $send --until 'BENCH-DONE'
} finally { Pop-Location }
