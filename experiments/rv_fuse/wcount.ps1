# The wide tick's three counting builds on one load: how much ran where, under WARP (no card time, no card lock).
#   pwsh experiments\rv_fuse\wcount.ps1 -Load shell [-Frames 40]
param([string]$Load, [int]$Frames = 40, [string]$Dir = 'C:\Users\micha\agents_isa\snap', [string]$Image = 'C:\Users\micha\agents_isa\img')
foreach ($m in 1, 2, 3) {
    $tick = "experiments\rvc_opt_wc$m"
    python experiments\rv_fuse\make_wide.py $tick --count $m | Out-Null
    $env:RVC_CODE_FILE = Join-Path $Dir "$Load\$Load.code"
    for ($try = 0; $try -lt 1500; $try++) {
        $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --warp --rvc $tick --define NO_PAGING --image $Image --no-desktop --no-viz --no-stdin --load-state (Join-Path $Dir "$Load\$Load.snap") --frames $Frames --stats-after 0 --frame-log "logs\wc_$m.frames" 2>&1 | ForEach-Object { "$_" }
        if (-not ($o | Where-Object { $_ -match 'REFUSED by the harness lock' })) { break }
        Start-Sleep -Seconds 2
    }
    "mode $m : " + (($o | Where-Object { $_ -match 'STATS seconds|rror' } | ForEach-Object { $_.Substring(0, [Math]::Min(150, $_.Length)) }) -join ' | ')
    Remove-Item -Recurse -Force $tick
}
$env:RVC_CODE_FILE = ''
