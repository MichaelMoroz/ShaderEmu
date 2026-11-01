# Who ended core 0's passes in play (frames 1 to 1299) on rvc_cpu, each with its callers: passlog.ps1 IMAGE OUT [-Env "A=1"] [-Every N]
param([string]$Image, [string]$Out, [string]$Env = '', [int]$Every = 6, [int]$Stack = 8)
$main = (git worktree list --porcelain | Select-Object -First 1).Substring(9)
$env:RVC_THREADS = '8'
$env:RVC_PASSLOG = $Out
$env:RVC_PASSLOG_STACK = "$Stack"
$env:RVC_DEVICE_EVERY = "$Every"
$td = "TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 $Env TDAWN_MARK=1,1299 TDAWN_SCEN=3 TDAWN_FRAMES=1300"
& "$main\bin\rvc_cpu.exe" --image $Image --quiet --expect "/ # " --send "nano-X -p & sleep 2; $td tdawn FROMINSTALL; echo H2H'' B\n" --count-from ": mark begins" --until "H2H B" --seconds 900 2>&1 | Select-String 'COUNT|done at' | % { $_.Line.Trim() }
