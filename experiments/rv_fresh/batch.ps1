# A step's measures one after another: pwsh batch.ps1 "steps\s2.hlsl adds -D KEEP --note x" ... ; run from anywhere
param([Parameter(ValueFromRemainingArguments)] [string[]] $Lines)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
foreach ($l in $Lines) {
    $a = $l -split ' '
    python (Join-Path $here 'run.py') @a --reps 2 2>&1 | Select-Object -Last 1
}
