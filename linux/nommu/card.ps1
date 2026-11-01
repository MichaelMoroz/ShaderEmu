# Card timings of docs/system/nommu.md, one run a configuration, IMAGE:DEFINE,DEFINE (build/images/IMAGE):
#   pwsh linux/nommu/card.ps1 -App doom|quake|q3|ray|ra -Configs linux:,nommu:NO_PAGING,nommu-zb:NO_PAGING,ZB
param([string]$App = 'doom', [string[]]$Configs = @('linux:', 'nommu:NO_PAGING'))
$repo = Split-Path (Split-Path $PSScriptRoot)
$out = "$repo\build\nommu\card"; New-Item -ItemType Directory -Force $out | Out-Null
$cmds = @{
    doom = @('nano-X -p & sleep 2; doom -1 -timedemo demo1; echo LX-''''DONE', 'LX-DONE', 120)
    quake = @('nano-X -p & sleep 2; quake +timedemo demo1', ' fps', 150)
    ray = @('nano-X -p & sleep 2; RAY_FRAMES=8 nxray; echo LX-''''DONE', 'LX-DONE', 120)
    ra = @('nano-X -p & sleep 2; TDAWN_AUTO=1 RALERT_SIDE=soviet RALERT_SEED=7 RALERT_FRAMES=900 ralert; echo LX-''''DONE', 'LX-DONE', 240)
    q3 = @('nano-X -p & sleep 2; QUAKE3_SEED=7 QUAKE3_SERVER=inline QUAKE3_CLIENT=inline QUAKE3_SCENE=inline QUAKE3_STATS_FRAMES=100 quake3 +set fixedtime 33 +devmap q3dm17 +addbot sarge 3 +wait 100 +setviewpos 488 -968 378 90', 'q3stat: frame 300:', 200)
}
$cmd, $until, $secs = $cmds[$App]
foreach ($c in $Configs) {
    $img, $def = $c.Split(':')
    # (the card alone: no other harness, and nothing else holding it)
    while ($true) {
        $others = Get-CimInstance Win32_Process -Filter "Name='rvc_harness.exe' or Name='rvc_cpu.exe'"
        $util = [int](nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits)
        if (-not $others -and $util -lt 15) { break }
        Write-Host "waiting: $(@($others).Count) harness processes, card $util%"; Start-Sleep 20
    }
    # rvc_harness takes build/images/linux-net from its folder: a folder whose linux is IMAGE
    $root = "$out\root-$img"
    if (-not (Test-Path "$root\build\images\linux")) {
        New-Item -ItemType Directory -Force "$root\build\images" | Out-Null
        New-Item -ItemType Junction -Path "$root\build\images\linux" -Target "$repo\build\images\$img" | Out-Null
    }
    $defs = @(); foreach ($d in ($def -split ',' | Where-Object { $_ })) { $defs += '--define'; $defs += $d }
    $log = "$out\$App-$img-$($def -replace ',','_').log"; Remove-Item $log -ErrorAction SilentlyContinue
    Push-Location $root
    $t = Measure-Command { $o = pwsh -NoProfile -File "$repo\tools\gpu_queue.ps1" "$repo\bin\rvc_harness.exe" --d3d11 --rvc "$repo\experiments\rvc_lean" @defs --image linux-net --no-stdin --uart-log $log --seconds $secs --expect '/ # ' --send "$cmd\n" --until $until 2>&1 }
    Pop-Location
    $h = ($o | Select-String '^\[harness\] \d+ frames' | ForEach-Object { $_.Line.Trim() }) -join ' '
    $r = (Select-String -Path $log -Pattern 'timed \d+ gametics in \d+ realtics|\d+ frames [\d.]+ seconds\s+[\d.]+ fps|raystat: picture \d+, \d+ x \d+: \d+ ms|rastat: frame \d+: [\d.]+ game frames/s|q3stat: frame \d+: 100 frames in \d+ ms' | ForEach-Object { $_.Matches[0].Value }) -join ' | '
    "$App $img [$def] $([int]$t.TotalSeconds) s: $h || $r" | Tee-Object -Append "$out\card.txt"
}
