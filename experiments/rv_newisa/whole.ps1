# Whole real loads, cold boot, the tree and a variant in turn: pwsh whole.ps1 -Defines 'NI_A,NI_B' [-Loads boot,...] [-Runs 3] [-Tag t]
param([string]$Defines = '', [string[]]$Loads = @('boot', 'jpeg_ns', 'png_ns', 'mp3', 'tdawn'), [int]$Runs = 3, [string]$Tag = 'best')
$tick = (Resolve-Path "$PSScriptRoot\..\rvc_lean_ni").Path
$lean = (Resolve-Path "$PSScriptRoot\..\rvc_lean").Path
$out = "$PSScriptRoot\whole.jsonl"
$h = "$PSScriptRoot\..\rv_vm\h2h.py"
$extra = ($Defines -split ',' | Where-Object { $_ } | ForEach-Object { "--define $_" }) -join ' '
foreach ($l in $Loads) { foreach ($r in 1..$Runs) {
    python $h rv $l --tick $lean --tag lean --out $out 2>&1 | Out-Null
    python $h rv $l --tick $tick --tag $Tag --out $out --extra " $extra" 2>&1 | Out-Null
} }
