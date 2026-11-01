# One harness run at a time, for every session on this computer. Runs the command when its turn comes.
#   pwsh tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_lean ...
# Every rvc_harness run goes through it, GPU or CPU, --cpu --warp included. The harnesses' own
# lock (run_lock) refuses one that breaks "many CPU harnesses or one GPU harness" with exit 75
# (docs/machine/cpu-harness.md, "One card, many processors"); it does not wait, this queue does.
#
# Whoever holds the card is written to a file beside the lock (gpu_queue.holder in the temporary
# folder): its process, the process that started it, since when, the folder and the command, and
# GPU_QUEUE_WHO if the caller set it (a session's or a script's name). A run that has to wait prints
# that first, so a wait says whose run it is behind.
$mutex = [Threading.Mutex]::new($false, 'Global\ShaderEmu-gpu-run')
$holder = Join-Path ([IO.Path]::GetTempPath()) 'ShaderEmu-gpu-run.holder'
$waited = [Diagnostics.Stopwatch]::StartNew()
$got = $false
try { $got = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $got = $true }   # its holder died: the turn is ours
if (-not $got) {
    $who = try { (Get-Content $holder -Raw -ErrorAction Stop).Trim() } catch { 'unknown (a queue from before the holder file)' }
    [Console]::Error.WriteLine("gpu_queue: the card is taken, waiting:`n  $($who -replace "`n", "`n  ")")
    try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] { }
}
if ($waited.Elapsed.TotalSeconds -ge 1) { [Console]::Error.WriteLine("gpu_queue: waited $([int]$waited.Elapsed.TotalSeconds) s for the card") }
try {
    $command, $rest = $args
    $parent = try { (Get-CimInstance Win32_Process -Filter "ProcessId=$((Get-CimInstance Win32_Process -Filter "ProcessId=$PID").ParentProcessId)").CommandLine } catch { '?' }
    if ($parent.Length -gt 200) { $parent = $parent.Substring(0, 200) + '...' }
    $line = "$command $($rest -join ' ')"
    if ($line.Length -gt 300) { $line = $line.Substring(0, 300) + '...' }
    @(
        "process $PID since $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))" + $(if ($env:GPU_QUEUE_WHO) { ", for $env:GPU_QUEUE_WHO" } else { '' })
        "started by: $parent"
        "in: $((Get-Location).Path)"
        "runs: $line"
    ) | Set-Content $holder -ErrorAction SilentlyContinue
    & $command @rest
    # 75: the harness's own lock refused it (a CPU harness running, or another GPU one); its line says which
    if ($LASTEXITCODE -eq 75) { [Console]::Error.WriteLine("gpu_queue: REFUSED by the harness lock, nothing ran (see the [lock] line above)") }
    exit $LASTEXITCODE
} finally {
    Remove-Item $holder -ErrorAction SilentlyContinue
    $mutex.ReleaseMutex()
}
