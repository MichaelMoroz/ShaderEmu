# A snapshot of each of the study's loads in its steady part, made on the processor with a profile of what ran
# (RVC_PROF), for fuse_snap.py and the card: pwsh experiments\rv_fuse\snaps.ps1 [-Only shell,doom] [-Out DIR]
param([string[]]$Only = @('shell', 'doom', 'quake', 'nxray'), [string]$Out = 'C:\Users\micha\agents_isa\snap',
      [string]$Exe = 'D:\Development\ShaderX86\bin\rvc_harness.exe', [string]$Image = 'D:\Development\ShaderX86\build\images\nommu')
New-Item -ItemType Directory -Force $Out | Out-Null
$loads = @{
    shell = '(sleep 2; echo SN""AP) & i=0; while [ $i -lt 2000000 ]; do i=$((i+1)); done'
    doom  = 'nano-X -p & sleep 2; (sleep 12; echo SN""AP) & DOOM_VIEW=told doom -1 -timedemo demo1'
    quake = 'nano-X -p & sleep 2; (sleep 25; echo SN""AP) & quake +map e1m1'
    nxray = 'nano-X -p & sleep 2; (sleep 8; echo SN""AP) & RAY_FRAMES=100000 nxray'
}
$env:RVC_THREADS = '8'
foreach ($n in $Only) {
    $dir = Join-Path $Out $n
    New-Item -ItemType Directory -Force $dir | Out-Null
    $env:RVC_PROF = $dir
    $env:RVC_PROF_MEM = '0'
    $t = Measure-Command {
        & pwsh -NoProfile -File tools\gpu_queue.ps1 $Exe --d3d11 --rvc experiments\rvc_opt --define NO_PAGING --image $Image --no-desktop --no-viz --no-stdin `
            --cpu --cpu-ips 5500000 --frames 1000000 --uart-log "$dir\uart.log" --input ($loads[$n] + '\n') --until 'SNAP' --save-state "$dir\$n.snap" 2>&1 | Select-Object -Last 4 | Out-Host
    }
    "$n : $([int]$t.TotalSeconds) s, $((Get-Item "$dir\$n.snap").Length) bytes, pcs $((Get-Content "$dir\pcs.txt").Count)"
}
