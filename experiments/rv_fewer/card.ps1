# Images taken in turn on the card by h2h's Tiberian Dawn load: card.ps1 -Images a,b -Rounds 2 [-Load tdawn] [-Out FILE]
# (each line of FILE is h2h's result, tagged with the image's name)
param([string[]]$Images, [int]$Rounds = 2, [string]$Load = 'tdawn', [string]$Out = 'experiments/rv_fewer/card.jsonl')
$Images = $Images -split ','
$here = (Get-Location).Path
$out_abs = Join-Path $here $Out
for ($r = 0; $r -lt $Rounds; $r++) {
    foreach ($i in $Images) {
        $more = if ($Load -eq 'tdawn_play') { @('--env', 'RVC_MARK=: mark ') } else { @() }
        python experiments/rv_vm/h2h.py rv $Load --image "$here\build\images\$i" --tag "$i" --out $out_abs @more 2>&1 | Select-Object -Last 1
    }
}
