# Ticks side by side in one run from one snapshot on one clock (RVC_TICK_ALT): for what a tick costs on code that does not
# use it. pwsh experiments\rv_fuse\alt.ps1 -Load shell -Ticks a,b,c [-Frames 12000] [-Snap x.snap]
param([string]$Load = 'shell', [string]$Ticks, [int]$Frames = 12000, [string]$Snap = '', [string]$Dir = 'C:\Users\micha\agents_isa\snap',
      [string]$Image = (Join-Path $PSScriptRoot '..\..\build\images\nommu'))
$snap = Join-Path $Dir "$Load\$(if ($Snap) { $Snap } else { "$Load.snap" })"
$env:RVC_TICK_ALT = (($Ticks -split ',') | ForEach-Object { (Resolve-Path "experiments\rvc_opt_$_").Path }) -join ';'
$flags = '--d3d11', '--rvc', 'experiments\rvc_opt', '--define', 'NO_PAGING', '--image', $Image, '--no-desktop', '--no-viz', '--no-stdin',
    '--load-state', $snap, '--frames', "$Frames", '--stats-after', '2'
for ($try = 0; $try -lt 1500; $try++) {
    $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 bin\rvc_harness.exe @flags 2>&1 | ForEach-Object { "$_" }
    if (-not ($o | Where-Object { $_ -match 'REFUSED by the harness lock' })) { break }
    Start-Sleep -Seconds 2
}
$o | Where-Object { $_ -match '^STATS tick|rror|not run|refused' } | ForEach-Object { $_ -replace [regex]::Escape((Get-Location).Path), '' }
