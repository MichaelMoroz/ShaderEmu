# An Nsight Graphics GPU Trace of gpu_vm's tick (bin\gvmtrace12.exe on D3D12, DXC with debug information) with the
# real-time shader profiler, and a summary of its counters (tools/nsight_summary.py). Open the .ngfx-gputrace in
# Nsight Graphics for the shader's own code against the source lines (Shader Profiler, Source and SASS).
#
#   pwsh gpu_vm\tick\trace.ps1 -Shader gpu_vm\tick\steps\x12.hlsl -Defines 'OK_MIN=1,INT_FIRST=1' -Case loads
#   pwsh gpu_vm\tick\trace.ps1 ... -Filter warps_issue_stalled,latency     # only the metrics with these in their names
#
# Through tools\gpu_queue.ps1 like every run on the card; it takes about a minute.
param(
    [string]$Shader = 'gpu_vm\tick\steps\x12.hlsl',
    [string]$Defines = '',
    [string]$Case = 'arith',
    [string]$Filter = '',
    [string]$Out = 'logs\nsight-gvm',
    [string]$Ngfx = 'C:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.3.1\host\windows-desktop-nomad-x64\ngfx.exe'
)
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
if (-not (Test-Path $Ngfx)) { Write-Error "Nsight Graphics not found at $Ngfx (pass -Ngfx)"; exit 2 }
$name = "$Case-" + (Get-Date -Format 'yyyyMMdd-HHmmss')
$run = Join-Path $Root (Join-Path $Out $name)
New-Item -ItemType Directory -Force $run | Out-Null
$d = @($Defines -split ',' | Where-Object { $_ } | ForEach-Object { "-D $_" }) -join ' '
$appArgs = "--shader $Shader $d --case $Case --seconds 8"
$p = Start-Process -FilePath $Ngfx -ArgumentList @(
    '--activity', '"GPU Trace Profiler"', '--exe', (Join-Path $Root 'bin\gvmtrace12.exe'), '--dir', $Root, '--args', "`"$appArgs`"",
    '--start-after-ms', '3000', '--max-duration-ms', '400', '--auto-export', '--architecture', '"Blackwell GB20x"',
    '--metric-set-id', '0', '--real-time-shader-profiler', '--set-gpu-clocks', 'unaltered', '--output-dir', $run
) -RedirectStandardOutput (Join-Path $run 'ngfx.out') -RedirectStandardError (Join-Path $run 'ngfx.err') -NoNewWindow -PassThru
if (-not $p.WaitForExit(150000)) { Stop-Process -Id $p.Id -Force; Write-Error 'ngfx did not finish in 150 s'; exit 1 }
$regimes = Get-ChildItem $run -Recurse -Filter GPUTRACE_REGIMES.xls | Select-Object -First 1
if (-not $regimes) { Get-Content (Join-Path $run 'ngfx.out') -Tail 12; Get-Content (Join-Path $run 'ngfx.err') -Tail 5; Write-Error 'no trace was exported'; exit 1 }
"trace: $run"
Get-ChildItem $run -Recurse -Filter *.ngfx-gputrace | ForEach-Object { "open in Nsight Graphics: $($_.FullName)" }
$filters = @($Filter -split ',' | Where-Object { $_ })
python tools\nsight_summary.py $regimes.FullName @filters | Set-Content (Join-Path $run 'summary.txt')
"summary: $(Join-Path $run 'summary.txt')"
if ($Filter) { Get-Content (Join-Path $run 'summary.txt') }
