# A timed window from a load's snapshot on the card, warm: pwsh snaprun.ps1 LOAD TICK [-Secs 16] [-After 8] [-Defines A=1,B=2] [-Env A=1,B=2]
# Prints the harness's STATS lines and the graphics clock seen halfway through.
param([string]$Load, [string]$Tick = 'rvc_lean', [int]$Secs = 16, [int]$After = 8, [string[]]$Defines = @(), [string[]]$EnvVars = @(), [string]$Extra = '')
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$main = (git -C $root worktree list --porcelain | Select-Object -First 1) -replace '^worktree ', ''
$bin = if (Test-Path "$root\bin\rvc_harness.exe") { "$root\bin\rvc_harness.exe" } else { "$main\bin\rvc_harness.exe" }
$snap = "C:\Users\micha\agents_isa\ni\$Load\$Load.snap"
$a = @($bin, '--d3d11', '--rvc', "$root\experiments\$Tick", '--define', 'NO_PAGING', '--image', "$main\build\images\nommu", '--load-state', $snap,
       '--no-desktop', '--no-viz', '--no-stdin', '--seconds', $Secs, '--stats-after', $After) + ($Extra -split ' ' | Where-Object { $_ })
foreach ($d in $Defines) { $a += '--define'; $a += $d }
$env:GPU_QUEUE_WHO = 'rv_newisa'
foreach ($e in $EnvVars) { $k, $v = $e -split '=', 2; Set-Item "Env:$k" $v }
$job = Start-Job { param($s) Start-Sleep ($s * 0.6); (nvidia-smi --query-gpu=clocks.gr,utilization.gpu,power.draw,clocks_event_reasons.active --format=csv,noheader) } -ArgumentList $Secs
$o = pwsh -NoProfile -File "$root\tools\gpu_queue.ps1" @a 2>&1
$c = Receive-Job $job -Wait -AutoRemoveJob
"[$Load $Tick $($Defines -join ' ') $($EnvVars -join ' ')] clock $c"
$o | Select-String 'STATS|error|refused|not run|REFUSED' | Select-Object -Last 5 | ForEach-Object { $_.Line }
