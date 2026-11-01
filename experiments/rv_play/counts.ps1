# The played counts made anew on the tree (a counting build in the private home, no workers, linux/tdawn/tdawn.profile written), the game then built with them.
param([string]$Scenarios = '1,3,5', [int]$Frames = 1200)
$main = (git worktree list --porcelain | Select-Object -First 1).Substring(9)
wsl -- bash experiments/rv_play/env.sh build tdawn/build.sh TDAWN_PROFILE=count | Select-Object -Last 1
pwsh experiments/rv_play/mkimg.ps1 cnt -Prog usr/lib/tdawn/game=src/tdawn-build/vanillatd | Select-Object -Last 1
$env:RVC_THREADS = '1'
$logs = @()
foreach ($s in ($Scenarios -split ',')) {
    $log = (Resolve-Path build/rv_play).Path + "\cnt$s.uart"
    Remove-Item $log -ErrorAction SilentlyContinue  # (--uart-log adds to what is there; profile.py refuses two builds' runs)
    $td = "TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCENE=inline TDAWN_FRAMES_CORE=0 TDAWN_SCEN=$s TDAWN_FRAMES=$Frames"
    & "$main\bin\rvc_cpu.exe" --image build/images/cnt --quiet --uart-log $log --expect "/ # " --send "nano-X -p & sleep 2; $td tdawn FROMINSTALL; echo H2H'' B\n" --until "H2H B" --seconds 1500 2>&1 | Select-Object -Last 1
    Select-String -Path $log -Pattern 'done at frame' | % { $_.Line.Trim() }
    $logs += "build/rv_play/cnt$s.uart"
}
wsl -- python3 linux/tdawn/profile.py make linux/tdawn/tdawn.profile @logs
Get-Item linux/tdawn/tdawn.profile | % { "$($_.Name) $($_.Length)" }
wsl -- bash experiments/rv_play/env.sh build tdawn/build.sh | Select-Object -Last 1
