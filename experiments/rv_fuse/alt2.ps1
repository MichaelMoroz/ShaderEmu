# Ticks side by side in one run from one snapshot on one clock, on their code file: ms of a frame each and what the guest did.
#   pwsh experiments\rv_fuse\alt2.ps1 -Load shell -Ticks f,w0 -Code C:\...\f\shell.code [-Frames 6000] [-Log name]
param([string]$Load = 'shell', [string]$Ticks, [string]$Code = '', [int]$Frames = 6000, [string]$Dir = 'C:\Users\micha\agents_isa\snap', [string]$Image = 'C:\Users\micha\agents_isa\img', [string]$More = '', [string]$Out = 'experiments\rv_fuse\alt2.log')
$snap = Join-Path $Dir "$Load\$Load.snap"
$env:RVC_CODE_FILE = $Code
$env:RVC_TICK_ALT = (($Ticks -split ',') | ForEach-Object { (Resolve-Path "experiments\rvc_opt_$_").Path }) -join ';'
$flags = '--d3d11', '--rvc', 'experiments\rvc_opt', '--define', 'NO_PAGING', '--image', $Image, '--no-desktop', '--no-viz', '--no-stdin', '--load-state', $snap, '--frames', "$Frames", '--stats-after', '1', '--frame-log', "logs\alt_$Load.frames", '--uart-log', "logs\alt_$Load.uart"
$extra = @($More -split ' ' | Where-Object { $_ })
for ($try = 0; $try -lt 200000; $try++) {
    $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 bin\rvc_harness.exe @flags @extra 2>&1 | ForEach-Object { "$_" }
    if (-not ($o | Where-Object { $_ -match 'refused' })) { break }
    Start-Sleep -Seconds 1
}
$rows = $o | Where-Object { $_ -match '^STATS tick|^STATS seconds|^\[harness\] [0-9]+ frames|rror|not run|panic' } | ForEach-Object { $_ -replace [regex]::Escape((Get-Location).Path + '\'), '' }
$u = python tools\tick_util.py "logs\alt_$Load.frames" 2>&1 | Select-Object -First 3 | ForEach-Object { "$_".Substring(0, [Math]::Min(170, "$_".Length)) }
$g = (Get-Content "logs\alt_$Load.uart" -Tail 3 -ErrorAction SilentlyContinue) -join ' | '
$txt = @("== $(Get-Date -Format HH:mm) $Load [$Ticks] rvc_cpu now: $((Get-Process rvc_cpu -ErrorAction SilentlyContinue | Measure-Object).Count)") + $rows + $u + @("guest: " + $g.Substring(0, [Math]::Min(200, $g.Length)))
$txt | Add-Content $Out
$txt
