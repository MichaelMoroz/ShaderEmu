# What ended core 0's passes in h2h's window, by reason and place: passes.ps1 IMAGE ELF [-Env "A=1"]
# (RVC_PASSLOG on rvc_cpu: end 0 budget, 1 ended itself, 2 waited, 4 full, 5 a copy)
param([string]$Image, [string]$Elf, [string]$Env = '')
$main = (git worktree list --porcelain | Select-Object -First 1).Substring(9)
$env:RVC_THREADS = '1'
$log = Join-Path $env:TEMP ("rvp" + $PID + ".log")
$env:RVC_PASSLOG = $log
$td = "TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 $Env TDAWN_SCEN=3 TDAWN_FRAMES=1300"
& "$main\bin\rvc_cpu.exe" --image $Image --quiet --expect "/ # " --send "nano-X -p & sleep 2; echo H2H'' A; $td tdawn FROMINSTALL; echo H2H'' B\n" --count-from "H2H A" --until "H2H B" --seconds 900 2>&1 | Select-String 'COUNT' | % { $_.Line.Trim() }
Remove-Item Env:RVC_PASSLOG
$u = (wsl -- wslpath -a ($log -replace '\\','/'))
wsl -- python3 experiments/rv_fewer/passes.py $u $Elf
Remove-Item $log -ErrorAction SilentlyContinue
