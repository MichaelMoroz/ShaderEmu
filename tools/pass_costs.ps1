param([string[]]$work = @("loop", "doom", "gzip", "sha", "find", "dd", "grep"), [int[]]$at = @(20, 60, 211),
      [string]$out = "logs\pass_costs.csv", [string[]]$extra = @(), [int]$frames = 2600, [switch]$make)
# What a pass's time is made of (docs/machine/tick-costs.md, "What a pass is made of").
#
#   tools\pass_costs.ps1 [-make]          then          python tools\pass_regress.py logs\pass_costs.csv
#
# For several kinds of guest work under Linux and a few places in each, one pass is timed and
# counted: timed on a frozen state (RVC_FREEZE: nothing is written back and guest time stands
# still, so every frame runs the same pass; the fastest 1,024 frames of a run during which the
# card's clock was high), and counted by the profiling build's first frame from the same
# state. A line of the .csv each; pass_regress.py fits a cost to each kind of event.
# -make boots Linux and makes the snapshots (build\snapshots\cost_*.snap) first.
Set-Location (Split-Path $PSScriptRoot)
$cmds = @{
    loop = 'i=0; while true; do i=$((i+1)); done'
    doom = 'nano-X -p & sleep 2; doom -timedemo demo1 -nodraw'
    gzip = 'while true; do gzip -c /bin/busybox > /dev/null; done'
    sha  = 'while true; do sha256sum /bin/busybox > /dev/null; done'
    find = 'while true; do find / > /dev/null 2>&1; done'
    dd   = 'while true; do dd if=/dev/zero of=/dev/null bs=64k count=64 2>/dev/null; done'
    grep = 'while true; do grep -r root /etc /bin /usr > /dev/null 2>&1; done'
}
$wait = @{ loop = 9; doom = 24; gzip = 12; sha = 12; find = 12; dd = 12; grep = 12 }
$base = @("--d3d11", "--rvc", "experiments\rvc_lean", "--image", "linux-net", "--no-stdin")
$exe = (Resolve-Path "bin\rvc_harness.exe").Path
New-Item -ItemType Directory -Force logs, build\snapshots | Out-Null

function Get-GpuClock { [int]((nvidia-smi --query-gpu=clocks.gr --format=csv,noheader,nounits) | Select-Object -First 1) }
# The harness with these arguments, again until the card's clock was high all through the
# run's second half (it runs at half its clock for the first seconds of a load).
function Run-Hot([string[]]$a, [int]$tries = 6, [int]$need = 2600) {
    for ($t = 0; $t -lt $tries; $t++) {
        $err = "logs\pass_costs_$PID.err"
        $quoted = $a | ForEach-Object { if ($_ -match '[ ;"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }
        $p = Start-Process -FilePath $exe -ArgumentList $quoted -NoNewWindow -PassThru -RedirectStandardError $err -RedirectStandardOutput "$err.out"
        $clocks = @()
        while (-not $p.HasExited) { Start-Sleep -Milliseconds 400; $clocks += Get-GpuClock }
        $text = Get-Content $err
        $n = $clocks.Count
        $half = if ($n -ge 4) { $clocks[([int]($n / 2))..($n - 2)] } else { $clocks }
        if (($half | Measure-Object -Minimum).Minimum -ge $need) { return $text }
        & $exe @base --load-state "build\snapshots\cost_$($work[0]).snap" --fixed-dt 0.004 --frames 2500 2>&1 | Out-Null
    }
    return $text
}

foreach ($w in $work) {
    if ($make -or -not (Test-Path "build\snapshots\cost_$w.snap")) {
        & $exe @base --uart-log logs\uart_cost.log --expect "/ # " --send "$($cmds[$w])\n" --seconds $wait[$w] --save-state "build\snapshots\cost_$w.snap" 2>&1 | Out-Null
    }
}
foreach ($w in $work) {
    foreach ($n in $at) {
        # (some passes on from the snapshot: its TLBs and the like are then as a run has them)
        & $exe @base --load-state "build\snapshots\cost_$w.snap" --fixed-dt 0.004 --frames $n --save-state build\snapshots\cost_tmp.snap 2>&1 | Out-Null
        $snap = "build\snapshots\cost_tmp.snap"
        $p = & $exe @base @extra --profile --load-state $snap --fixed-dt 0.000000001 --frames 1 2>&1
        $c = ($p | Select-String "^PROF (\w+)\s+(\d+)" | ForEach-Object { "$($_.Matches[0].Groups[1].Value)=$($_.Matches[0].Groups[2].Value)" }) -join ";"
        $env:RVC_FREEZE = "1"
        $o = Run-Hot ($base + $extra + @("--load-state", $snap, "--fixed-dt", "0.000000001", "--bench", "200", "--frames", "$(200 + $frames)"))
        $env:RVC_FREEZE = $null
        $ms = if ((($o | Select-String "^BENCH ").Line) -match "best_ms=([\d.]+)") { $Matches[1] } else { "0" }
        "$w,$n,$ms,$c" | Tee-Object -FilePath $out -Append
    }
}
