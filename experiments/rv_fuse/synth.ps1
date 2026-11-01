# Each kind of synthetic bundle under some ticks, on the shell snapshot: ns a trip = (tick ms - tree ms) / (341 re-entries * 64 trips).
#   pwsh experiments\rv_fuse\synth.ps1 -Ticks syn,synf [-Kinds nop,sum,load] [-Frames 300]     (ticks made with --synth 64)
param([string]$Ticks, [string]$Kinds = 'nop,sum,unit,load,branch,fall', [int]$Frames = 300)
function T([string]$tick, [string]$code) {
    $o = if ($code) { pwsh -NoProfile -File experiments\rv_fuse\one.ps1 -Tick $tick -Load shell -Code $code -Frames $Frames 2>&1 } else { pwsh -NoProfile -File experiments\rv_fuse\one.ps1 -Tick $tick -Load shell -NoCode -Frames $Frames 2>&1 }
    $s = ($o | ForEach-Object { "$_" }) -join ' '
    $m = [regex]::Match($s, 'shell\s+tick ([0-9.]+)')
    if (-not $m.Success) { Write-Host $s.Substring(0, [Math]::Min(400, $s.Length)) }
    [double]($m.Groups[1].Value)
}
$tree = T 'experiments\rvc_opt' ''
"tree $tree ms"
foreach ($k in $Kinds -split ',') { foreach ($v in $Ticks -split ',') {
    $x = T "experiments\rvc_opt_$v" "C:\Users\micha\agents_isa\snap\try\syn_$k.code"
    "{0,-8} {1,-8} {2,8} ms  {3,6:n1} ns a trip" -f $v, $k, $x, (($x - $tree) * 1e6 / (341 * 64))
} }
