# wide.h's shapes on one load: each made, run under WARP a few frames, stamped, then all and the tree in turn on the card.
#   pwsh experiments\rv_fuse\shapes.ps1 -Load shell -Shapes "1 3 5" [-Rounds 2] [-Frames 3000] [-More "--unity 11.1,7.1,1.0"] [-Src experiments\rvc_opt]
param([string]$Load, [string]$Shapes = '0', [int]$Rounds = 2, [int]$Frames = 3000, [string]$More = '', [string]$Src = 'experiments\rvc_opt', [int]$WarpFrames = 8,
      [string]$Dir = 'C:\Users\micha\agents_isa\snap', [string]$Image = 'C:\Users\micha\agents_isa\img')
$snap = Join-Path $Dir "$Load\$Load.snap"
$code = Join-Path $Dir "$Load\$Load.code"
$extra = @($More -split ' ' | Where-Object { $_ })
function Run($name, $tick, $withCode, $flags) {
    $env:RVC_CODE_FILE = if ($withCode) { $code } else { '' }
    for ($try = 0; $try -lt 20000; $try++) {
        $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 @flags --rvc $tick --define NO_PAGING --image $Image --no-desktop --no-viz --no-stdin --load-state $snap --stats-after 0.5 --frame-log "logs\sh_$name.frames" 2>&1 | ForEach-Object { "$_" }
        if (-not ($o | Where-Object { $_ -match 'REFUSED by the harness lock' })) { break }
        Start-Sleep -Seconds 1
    }
    $l = $o | Where-Object { $_ -match '^STATS|not run on|rror|FAILED' } | ForEach-Object { $_.Substring(0, [Math]::Min(120, $_.Length)) }
    $u = python tools\tick_util.py "logs\sh_$name.frames" 2>&1 | Select-Object -First 1 | ForEach-Object { "$_".Substring(0, [Math]::Min(120, "$_".Length)) }
    "$name : " + ($l -join ' | ') + ' | ' + $u
}
New-Item -ItemType Directory -Force logs | Out-Null
foreach ($s in $Shapes -split ' ') {
    $t = "experiments\rvc_opt_s$s"
    python experiments\rv_fuse\make_wide.py $t --shape $s --src $Src | Out-Null
    Run "warp-$s" $t $true @('--warp', '--frames', "$WarpFrames")
    $h = & bin\rvc_harness.exe --rvc $t --stamp 2>&1 | Select-Object -Last 1
    Set-Content -NoNewline -Path "$t\warp_ok" -Value "$h"
}
for ($r = 0; $r -lt $Rounds; $r++) {
    Run "$Load-tree-$r" $Src $false (@('--frames', "$Frames") + $extra)
    foreach ($s in $Shapes -split ' ') { Run "$Load-s$s-$r" "experiments\rvc_opt_s$s" $true (@('--frames', "$Frames") + $extra) }
}
$env:RVC_CODE_FILE = ''
