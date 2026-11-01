# What kinds of operation cost in a loop of 64 trips inside the tree's tick (wide.h's WIDE_FAKE), the machine not touched:
# ns a trip = (tick ms - tree ms) / (341 * 64).  pwsh experiments\rv_fuse\fake.ps1 -Fakes "0 1 4 128 256 8" [-Frames 300] [-Src experiments/rvc_opt]
param([string]$Fakes, [int]$Frames = 300, [string]$Src = 'experiments/rvc_opt', [string]$Extra = '')
function T([string]$tick) {
    $o = pwsh -NoProfile -File experiments\rv_fuse\one.ps1 -Tick $tick -Load shell -NoCode -Frames $Frames 2>&1
    $s = ($o | ForEach-Object { "$_" }) -join ' '
    $m = [regex]::Match($s, 'shell\s+tick ([0-9.]+)')
    if (-not $m.Success) { Write-Host $s.Substring(0, [Math]::Min(500, $s.Length)) }
    [double]($m.Groups[1].Value)
}
$tree = T $Src
"tree $tree ms"
foreach ($f in $Fakes -split ' ') {
    $n = "fk$f"
    pwsh -NoProfile -File experiments\rv_fuse\mk.ps1 $n --fake $f --src $Src @($Extra -split ' ' | Where-Object { $_ })
    $x = T "experiments\rvc_opt_$n"
    "fake {0,-6} {1,8} ms  {2,6:n1} ns a trip" -f $f, $x, (($x - $tree) * 1e6 / (341 * 64))
    Remove-Item -Recurse -Force "experiments\rvc_opt_$n"
}
