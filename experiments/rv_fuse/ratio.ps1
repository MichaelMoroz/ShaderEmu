# Shapes against the tree with a code area of zeros, so the map says nothing is translated and the wide loop truly never runs
# (with no code file at all the map's rows are the ROM's last bytes, and the loop ran on them: that was "the doubling").
#   pwsh experiments\rv_fuse\ratio.ps1 -Shapes "1 15617" [-Frames 1200]     a shape, or shape:test
param([string]$Shapes, [int]$Frames = 1200, [string]$Dir = 'C:\Users\micha\agents_isa\snap', [string]$Image = 'C:\Users\micha\agents_isa\img', [string]$Load = 'shell')
$snap = Join-Path $Dir "$Load\$Load.snap"
$zero = Join-Path $Dir 'zero.code'
if (-not (Test-Path $zero)) { $fs = [IO.File]::Create($zero); $fs.SetLength(5700L * 2048 * 16); $fs.Close() }
function Run($tick, $code) {
    $env:RVC_CODE_FILE = $code
    for ($i = 0; $i -lt 20000; $i++) { $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --rvc $tick --define NO_PAGING --image $Image --no-desktop --no-viz --no-stdin --load-state $snap --frames $Frames --stats-after 0.5 2>&1 | ForEach-Object { "$_" }; if (-not ($o | Select-String 'REFUSED')) { break }; Start-Sleep 1 }
    $m = [regex]::Match(($o -join "`n"), 'tick ([0-9.]+) ms, commit ([0-9.]+)')
    if ($m.Success) { "{0,-34} tick {1} commit {2} ratio {3:n1}" -f $tick, $m.Groups[1].Value, $m.Groups[2].Value, ([double]$m.Groups[1].Value / [double]$m.Groups[2].Value) } else { "$tick : " + (($o | Select-Object -Last 3) -join ' | ') }
}
Run 'experiments\rvc_opt' ''
foreach ($s in $Shapes -split ' ') {
    $t = "experiments\rvc_opt_r" + ($s -replace ":","_")
    $sp = $s -split ':'
    python experiments\rv_fuse\make_wide.py $t --shape $sp[0] --test $(if ($sp.Count -gt 1) { $sp[1] } else { '0' }) | Out-Null
    $h = & bin\rvc_harness.exe --rvc $t --stamp 2>&1 | Select-Object -Last 1
    Set-Content -NoNewline -Path "$t\warp_ok" -Value "$h"
    Run $t $zero
    Remove-Item -Recurse -Force $t
}
$env:RVC_CODE_FILE = ''
