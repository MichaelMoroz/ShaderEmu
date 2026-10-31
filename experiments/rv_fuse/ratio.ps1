# Unrun shapes against the tree: tick over commit (the card's clock taken out; the tree is 28). No code is given, the wide loop never runs.
#   pwsh experiments\rv_fuse\ratio.ps1 -Shapes "15617 1073757185" [-Frames 1200] [-Maker make_wide.py]
param([string]$Shapes, [int]$Frames = 1200, [string]$Dir = 'C:\Users\micha\agents_isa\snap', [string]$Image = 'C:\Users\micha\agents_isa\img', [string]$Load = 'shell', [string]$Fxc = '')
$snap = Join-Path $Dir "$Load\$Load.snap"
$env:RVC_CODE_FILE = ''
function Run($tick) {
    for ($i = 0; $i -lt 20000; $i++) { $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --rvc $tick --define NO_PAGING --image $Image --no-desktop --no-viz --no-stdin --load-state $snap --frames $Frames --stats-after 0.5 2>&1 | ForEach-Object { "$_" }; if (-not ($o | Select-String 'REFUSED')) { break }; Start-Sleep 1 }
    $m = [regex]::Match(($o -join "`n"), 'tick ([0-9.]+) ms, commit ([0-9.]+)')
    if ($m.Success) { "{0,-34} tick {1} commit {2} ratio {3:n1}" -f $tick, $m.Groups[1].Value, $m.Groups[2].Value, ([double]$m.Groups[1].Value / [double]$m.Groups[2].Value) } else { "$tick : " + (($o | Select-Object -Last 3) -join ' | ') }
}
Run 'experiments\rvc_opt'
foreach ($s in $Shapes -split ' ') {
    $t = "experiments\rvc_opt_r" + ($s -replace ":","_")
    $sp = $s -split ':'
    python experiments\rv_fuse\make_wide.py $t --shape $sp[0] --test $(if ($sp.Count -gt 1) { $sp[1] } else { '0' }) | Out-Null
    $h = & bin\rvc_harness.exe --rvc $t --stamp 2>&1 | Select-Object -Last 1
    Set-Content -NoNewline -Path "$t\warp_ok" -Value "$h"
    Run $t
    Remove-Item -Recurse -Force $t
}
