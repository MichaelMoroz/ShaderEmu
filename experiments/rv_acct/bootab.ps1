# Boot to the prompt, the image's kernel against the kernel with patch_bootwork.py, in turn: pwsh bootab.ps1 [-Rounds 3] [-Tag bw]
param([int]$Rounds = 3, [string]$Tag = 'bw')
$img = Join-Path $PSScriptRoot "logs\img_$Tag"
foreach ($r in 1..$Rounds) {
    python "$PSScriptRoot\acct.py" boot --tag "base$r" --env 'RVC_MARK=/ # ' --cmd 'echo H2H"" B' --out "$PSScriptRoot\boot.jsonl" | Out-Null
    python "$PSScriptRoot\acct.py" boot --tag "$Tag$r" --image $img --env 'RVC_MARK=/ # ' --cmd 'echo H2H"" B' --out "$PSScriptRoot\boot.jsonl" | Out-Null
    "round $r done"
}
