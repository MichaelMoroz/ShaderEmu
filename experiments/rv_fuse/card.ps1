# The tree's tick on a snapshot and a fused tick on the snapshot fused, in turn (A B A B), each through the queue.
#   pwsh experiments\rv_fuse\card.ps1 -Load shell -Rvc experiments\rvc_lean_fuse -Fused shell.addi.snap [-Rounds 2] [-Frames 4000] [-Warp] [-More '--ticks','10000']
param([string]$Code = '', [string]$Load, [string]$Rvc = 'experiments\rvc_lean_fuse', [string]$Fused, [int]$Rounds = 2, [int]$Frames = 4000, [switch]$Warp, [switch]$OnlyB,
      [string]$More = '', [string]$MoreA = '', [string]$Base = 'experiments\rvc_lean', [string]$Dir = 'C:\Users\micha\agents_isa\snap',
      [string]$Image = (Join-Path $PSScriptRoot '..\..\build\images\nommu'), [string]$Exe = 'bin\rvc_harness.exe')
$plain = Join-Path $Dir "$Load\$Load.snap"
$snapB = Join-Path $Dir "$Load\$Fused"
function One($name, $tick, $snap, $extra) {
    $flags = @('--d3d11') + $(if ($Warp) { @('--warp') } else { @() }) + @('--rvc', $tick, '--define', 'NO_PAGING', '--image', $Image, '--no-desktop', '--no-viz', '--no-stdin',
        '--load-state', $snap, '--frames', "$Frames", '--stats-after', '0.5', '--frame-log', "logs\fuse_$name.frames") + $extra
    $env:RVC_CODE_FILE = if ($name -match 'fuse' -and $Code) { Join-Path $Dir "$Load\$Code" } else { '' }
    $t = [Diagnostics.Stopwatch]::StartNew()
    for ($try = 0; $try -lt 1500; $try++) {   # (the harness lock refuses a GPU run beside CPU runs of other sessions)
        $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 $Exe @flags 2>&1 | ForEach-Object { "$_" }
        if (-not ($o | Where-Object { $_ -match 'REFUSED by the harness lock' })) { break }
        Start-Sleep -Seconds 2
    }
    $t.Stop()
    $lines = $o | Where-Object { $_ -match '^(STATS|\[harness\] [0-9]+ frames|\[harness\] [0-9]+ workers)|refused|not run on|error|FAILED' }
    "$name : " + (($lines | ForEach-Object { $_.Substring(0, [Math]::Min(170, $_.Length)) }) -join "`n    ")
    $u = python tools\tick_util.py "logs\fuse_$name.frames" 2>&1 | Select-Object -First 4 | ForEach-Object { "$_".Substring(0, [Math]::Min(150, "$_".Length)) }
    "    " + ($u -join "`n    ")
}
New-Item -ItemType Directory -Force logs | Out-Null
for ($r = 0; $r -lt $Rounds; $r++) {
    if (-not $OnlyB) { One "$Load-tree-$r" $Base $plain @($MoreA -split ' ' | Where-Object { $_ }) }
    One "$Load-fuse-$r" $Rvc $snapB @($More -split ' ' | Where-Object { $_ })
}
