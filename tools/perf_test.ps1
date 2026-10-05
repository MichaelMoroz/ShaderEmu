# Measures rvc emulation speed in a few seconds: resumes a snapshot of Linux running a shell
# busy loop, runs a fixed number of frames on a fixed timestep and reports instructions/second.
# Runs are deterministic, so `instructions` and `state` only change when emulation changes.
#
#   tools\perf_test.ps1                 # default 2048 ticks per frame
#   tools\perf_test.ps1 -Rvc experiments\rvc_opt   # another shader folder, same snapshot
#   tools\perf_test.ps1 -Ticks 8192    # extra arguments after that go to rvc_harness
param(
    [int]$Ticks = 2048,
    [int]$Frames = 500,
    [int]$Warmup = 30,
    [string]$Rvc = 'rvc\_Nix\rvc',
    [string]$Payload = 'rvc\_Nix\rvc\data-net',
    [string]$Snapshot = 'build\snapshots\rvc_bench.snap',
    [Parameter(ValueFromRemainingArguments)] [string[]]$Extra
)
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
$exe = 'bin\rvc_harness.exe'
New-Item -ItemType Directory -Force logs | Out-Null

if (-not (Test-Path $Snapshot)) {
    $shell = 'build\snapshots\rvc_shell.snap'
    if (-not (Test-Path $shell)) { Write-Error "missing $shell - see AGENTS.md for how to create it"; exit 2 }
    Write-Host "creating $Snapshot (shell busy loop on top of $shell)..."
    & $exe --rvc $Rvc --payload $Payload --no-stdin --load-state $shell --fixed-dt 0.004 --frames 1500 `
        --input 'i=0; while true; do i=$((i+1)); done\n' --save-state $Snapshot --uart-log logs\uart.log > $null 2> logs\perf_setup.err
    if ($LASTEXITCODE -ne 0) { Get-Content logs\perf_setup.err -Tail 5; exit 2 }
}

$sw = [Diagnostics.Stopwatch]::StartNew()
& $exe --rvc $Rvc --payload $Payload --no-stdin --load-state $Snapshot --fixed-dt 0.004 --ticks $Ticks `
    --bench $Warmup --frames ($Warmup + $Frames) @Extra > $null 2> logs\perf.err
$rc = $LASTEXITCODE
$line = Select-String -Path logs\perf.err -Pattern '^BENCH ' | Select-Object -Last 1
if ($rc -ne 0 -or -not $line) { Get-Content logs\perf.err -Tail 8; Write-Error "benchmark failed (exit $rc)"; exit 1 }
$m = @{}
foreach ($kv in $line.Line.Substring(6).Split(' ')) { $k, $v = $kv.Split('='); $m[$k] = $v }
"{0,8:n1}k IPS | {1,6:n1} frames/s | {2,7:n1} instr/frame of {3} | {4} instr | state {5} | total {6:n1}s" -f `
    ([double]$m.ips / 1000), [double]$m.fps, [double]$m.per_frame, $Ticks, $m.instructions, $m.state, $sw.Elapsed.TotalSeconds
