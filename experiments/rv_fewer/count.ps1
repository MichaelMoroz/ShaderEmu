# h2h's Tiberian Dawn window on rvc_cpu with no profile: core 0's instructions, frames 1 to 1299 apart, and the state. count.ps1 IMAGE [ENV="A=1 B=2"] [-One]
param([string]$Image, [string]$Env = '', [int]$Seconds = 900, [switch]$One, [int]$Scen = 3)
$main = (git worktree list --porcelain | Select-Object -First 1).Substring(9)
$env:RVC_THREADS = '1'
$td = "TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 $Env TDAWN_SCEN=$Scen TDAWN_FRAMES=1300"
$log = Join-Path $env:TEMP ("rvf" + $PID + ".uart")
$a = & "$main\bin\rvc_cpu.exe" --image $Image --quiet --uart-log $log --expect "/ # " --send "nano-X -p & sleep 2; echo H2H'' A; $td tdawn FROMINSTALL; echo H2H'' B\n" --count-from "H2H A" --until "H2H B" --seconds $Seconds 2>&1
$a | Select-String 'COUNT' | % { $_.Line.Trim() }
Select-String -Path $log -Pattern 'done at frame|looked at again|of them wrong|Oops|panic|Segmentation|unhandled' | % { $_.Line.Trim() }
if (-not $One) {
    $b = & "$main\bin\rvc_cpu.exe" --image $Image --quiet --expect "/ # " --send "nano-X -p & sleep 2; TDAWN_MARK=1,1299 $td tdawn FROMINSTALL; echo H2H'' B\n" --count-from ": mark begins" --until "H2H B" --seconds $Seconds 2>&1
    $b | Select-String 'COUNT' | % { 'play: ' + $_.Line.Trim() }
}
Remove-Item $log -ErrorAction SilentlyContinue
