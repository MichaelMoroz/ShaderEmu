# The world's tick and it with the wide loop, in turn, on a load's snapshot (the code made with --sink 64).
#   pwsh experiments\rv_fuse\lean.ps1 -Load shell [-Rounds 2] [-Frames 2500] [-More "--unity 11.1,7.1,1.0"]
param([string]$Load, [int]$Rounds = 2, [int]$Frames = 2500, [string]$More = '', [string]$Dir = 'C:\Users\micha\agents_isa\snap', [string]$Image = 'C:\Users\micha\agents_isa\img')
$snap = Join-Path $Dir "$Load\$Load.snap"
$code = Join-Path $Dir "$Load\$Load.lean.code"
$extra = @($More -split ' ' | Where-Object { $_ })
python experiments\rv_fuse\xlate_snap.py $snap (Join-Path $Dir $Load) $code --sink 64 | Select-Object -First 1
python experiments\rv_fuse\make_wide_lean.py experiments\rvc_lean_w | Out-Null
function Run($name, $tick, $withCode, $flags) {
    $env:RVC_CODE_FILE = if ($withCode) { $code } else { '' }
    for ($try = 0; $try -lt 20000; $try++) {
        $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 @flags --rvc $tick --image $Image --no-desktop --no-viz --no-stdin --load-state $snap --stats-after 0.5 --frame-log "logs\ln_$name.frames" 2>&1 | ForEach-Object { "$_" }
        if (-not ($o | Where-Object { $_ -match 'REFUSED by the harness lock' })) { break }
        Start-Sleep -Seconds 1
    }
    $l = $o | Where-Object { $_ -match '^STATS sec|^STATS gpu|not run on|rror|FAILED|refus' } | ForEach-Object { $_.Substring(0, [Math]::Min(120, $_.Length)) }
    $u = python tools\tick_util.py "logs\ln_$name.frames" 2>&1 | Select-Object -First 1 | ForEach-Object { "$_".Substring(0, [Math]::Min(120, "$_".Length)) }
    "$name : " + ($l -join ' | ') + ' | ' + $u
}
Run "warp-$Load" 'experiments\rvc_lean_w' $true @('--warp', '--frames', '6')
$h = & bin\rvc_harness.exe --rvc experiments\rvc_lean_w --stamp 2>&1 | Select-Object -Last 1
Set-Content -NoNewline -Path 'experiments\rvc_lean_w\warp_ok' -Value "$h"
for ($r = 0; $r -lt $Rounds; $r++) {
    Run "$Load-tree-$r" 'experiments\rvc_lean' $false (@('--frames', "$Frames") + $extra)
    Run "$Load-wide-$r" 'experiments\rvc_lean_w' $true (@('--frames', "$Frames") + $extra)
}
$env:RVC_CODE_FILE = ''
