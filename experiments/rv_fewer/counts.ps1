# The played counts made anew on the tree: a counting build, scenarios on rvc_cpu with no workers, linux/tdawn/tdawn.profile written.
# counts.ps1 [-Scenarios 1,3,...] [-Frames 1200] (docs/games/tdawn.md, "What the compiler is told")
param([string]$Scenarios = '1,3,5', [int]$Frames = 1200)
$main = (git worktree list --porcelain | Select-Object -First 1).Substring(9)
"export TDAWN_PROFILE=count`n" | Set-Content -NoNewline build/rv_fewer/cnt.env
@"
. build/rv_fewer/cnt.env
bash experiments/rv_fewer/env.sh build
"@ -replace "`r`n","`n" | Set-Content -NoNewline build/rv_fewer/cnt.sh
wsl -- bash build/rv_fewer/cnt.sh | Select-Object -Last 1
pwsh experiments/rv_fewer/mkimg.ps1 cnt | Select-Object -Last 1
$env:RVC_THREADS = '1'
$logs = @()
foreach ($s in ($Scenarios -split ',')) {
    $log = (Resolve-Path build/rv_fewer).Path + "\cnt$s.uart"
    $td = "TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCENE=inline TDAWN_FRAMES_CORE=0 TDAWN_SCEN=$s TDAWN_FRAMES=$Frames"
    & "$main\bin\rvc_cpu.exe" --image build/images/cnt --quiet --uart-log $log --expect "/ # " --send "nano-X -p & sleep 2; $td tdawn FROMINSTALL; echo H2H'' B\n" --until "H2H B" --seconds 1500 2>&1 | Select-Object -Last 1
    Select-String -Path $log -Pattern 'done at frame' | % { $_.Line.Trim() }
    "$s GC lines: " + (Select-String -Path $log -Pattern 'GC ' -SimpleMatch).Count
    $logs += "build/rv_fewer/cnt$s.uart"
}
wsl -- python3 linux/tdawn/profile.py make linux/tdawn/tdawn.profile @logs
Get-Item linux/tdawn/tdawn.profile | % { "$($_.Name) $($_.Length)" }
