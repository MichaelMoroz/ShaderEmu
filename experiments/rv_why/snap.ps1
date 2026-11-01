# A load's snapshot and a profile of the same run, made on the processor: pwsh snap.ps1 NAME [-Prof] [-Out DIR]
# NAME: tdawn, gzip, png. The snapshot is taken just after the program has started (SNAP on the console), with
# its code already in RAM; -Prof runs the same from boot to the load's end under RVC_PROF, without a snapshot.
param([string]$Name, [switch]$Prof, [string]$Out = 'C:\Users\micha\agents_isa\aot', [double]$Ips = 5500000)
$tree = (git -C $PSScriptRoot worktree list --porcelain | Select-Object -First 1) -replace '^worktree ', ''
$exe = Join-Path $tree 'bin\rvc_harness.exe'; $img = Join-Path $tree 'build\images\nommu'
$td = 'TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL'
$loads = @{
    tdawn = @{ cmd = "nano-X -p & sleep 2; (sleep 1; echo SN`"`"AP) & $td; echo LO`"`"AD; sleep 100"; until = 'LOAD' }
    gzip  = @{ cmd = "seq 1 20000 > /tmp/g; sleep 1; (sleep 0.5; echo SN`"`"AP; sleep 100) & while true; do gzip -c /tmp/g > /dev/null; gzip -c /tmp/g > /dev/null; echo LO`"`"AD; done"; until = 'LOAD' }
    png   = @{ cmd = "sleep 1; (sleep 0.1; echo SN`"`"AP; sleep 100) & nxview --decode /usr/share/picture-fox.png /tmp/a.ppm; echo LO`"`"AD; sleep 100"; until = 'LOAD' }
}
$l = $loads[$Name]
$dir = Join-Path $Out $Name
New-Item -ItemType Directory -Force $dir | Out-Null
$env:GPU_QUEUE_WHO = 'rv_why snap'
Remove-Item Env:RVC_PROF -ErrorAction SilentlyContinue
if ($Prof) { $env:RVC_PROF = $dir; $env:RVC_PROF_MEM = '0'; $until = $l.until; $save = @('--save-state', "$dir$Name.end.snap") } else { $env:RVC_THREADS = '8'; $until = 'SNAP'; $save = @('--save-state', "$dir\$Name.snap") }
Set-Location $tree
& pwsh -NoProfile -File (Join-Path $PSScriptRoot '..\..\tools\gpu_queue.ps1') $exe --d3d11 --rvc experiments\rvc_lean --define NO_PAGING --image $img --no-desktop --no-viz --no-stdin `
    --cpu --cpu-ips $Ips --frames 100000000 --uart-log "$dir\uart$(if ($Prof) {'_prof'}).log" --input ($l.cmd + '\n') --until $until @save 2>&1 | Select-Object -Last 5
Get-ChildItem $dir | Select-Object Name, Length | Format-Table
