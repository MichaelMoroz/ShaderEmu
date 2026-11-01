# The harness through the queue, again while another run's lock refuses it (exit 75): pwsh h.ps1 ARGS...
$tree = (git -C $PSScriptRoot worktree list --porcelain | Select-Object -First 1) -replace '^worktree ', ''
Set-Location $tree
$env:GPU_QUEUE_WHO = 'rv_why'
for ($i = 0; $i -lt 4000; $i++) {
    $o = & pwsh -NoProfile -File (Join-Path $PSScriptRoot '..\..\tools\gpu_queue.ps1') (Join-Path $tree 'bin\rvc_harness.exe') @args 2>&1
    if ($LASTEXITCODE -ne 75) { $o | Select-Object -Last 14; exit $LASTEXITCODE }
    Start-Sleep 3
}
