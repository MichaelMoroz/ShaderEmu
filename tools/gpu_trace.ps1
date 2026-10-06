# Captures an Nsight GPU Trace (per-draw GPU hardware counters) of the rvc shader and prints a
# summary per marker (CPUTick, Commit). Nsight only attaches to D3D12, so this runs
# bin\rvc_trace12.exe: the same FXC bytecode as rvc_harness, on Direct3D 12.
#
#   pwsh tools\gpu_trace.ps1                                   # experiments\rvc_opt
#   pwsh tools\gpu_trace.ps1 -Filter warps_issue_stalled,latency   # only metrics containing these
#   pwsh tools\gpu_trace.ps1 -Extra '--define NO_PAGING'      # more rvc_trace12 arguments
#
# --no-doubles is always passed: NVIDIA's D3D12 path miscomputes rvc's double math (timer and
# MULH), so the guest diverges without it. The shader folder must support NO_DOUBLES.
param(
    [string]$Rvc = 'experiments\rvc_opt',
    [string]$Payload = 'rvc\_Nix\rvc\data-net',
    [string]$Out = 'logs\nsight',
    [int]$Frames = 20,
    [string]$Extra = '',
    [string]$Filter = '',   # comma-separated substrings of metric names
    [string]$Ngfx = 'C:\Program Files\NVIDIA Corporation\Nsight Graphics 2025.5.0\host\windows-desktop-nomad-x64\ngfx.exe'
)
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
if (-not (Test-Path $Ngfx)) { Write-Error "Nsight Graphics not found at $Ngfx (pass -Ngfx)"; exit 2 }
$run = Join-Path $Root (Join-Path $Out (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $run | Out-Null

$appArgs = "--rvc $Rvc --payload $Payload --no-doubles --seconds 30 $Extra"
$p = Start-Process -FilePath $Ngfx -ArgumentList @(
    '--activity', '"GPU Trace Profiler"', '--exe', (Join-Path $Root 'bin\rvc_trace12.exe'), '--dir', $Root, '--args', "`"$appArgs`"",
    '--start-after-ms', '5000', '--limit-to-frames', $Frames, '--auto-export', '--architecture', '"Blackwell GB20x"',
    '--metric-set-id', '0', '--set-gpu-clocks', 'unaltered', '--output-dir', $run
) -RedirectStandardOutput (Join-Path $run 'ngfx.out') -RedirectStandardError (Join-Path $run 'ngfx.err') -NoNewWindow -PassThru
if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id -Force; Write-Error 'ngfx did not finish in 120 s'; exit 1 }
$regimes = Get-ChildItem $run -Recurse -Filter GPUTRACE_REGIMES.xls | Select-Object -First 1
if (-not $regimes) { Get-Content (Join-Path $run 'ngfx.out') -Tail 8; Write-Error 'no trace was exported'; exit 1 }

"trace: $run"
Get-Content (Join-Path $regimes.DirectoryName 'D3DPERF_EVENTS.xls') | ForEach-Object {
    $c = $_ -split "`t"
    if ($c[0] -ne 'event_text') { "{0,-8} {1:n3} ms per draw (mean of {2})" -f $c[0], (($c[1..($c.Count - 1)] | ForEach-Object { [double]$_ } | Measure-Object -Average).Average), ($c.Count - 1) }
}
$filters = @($Filter -split ',' | Where-Object { $_ })
python tools\nsight_summary.py $regimes.FullName @filters | Set-Content (Join-Path $run 'summary.txt')
"summary: $(Join-Path $run 'summary.txt')"
if ($Filter) { Get-Content (Join-Path $run 'summary.txt') }
