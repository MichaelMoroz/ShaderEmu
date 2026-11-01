# The tree's instruction classes on the card (programs/classes), each mark to the next; then gpu_vm's loops. ns an instruction.
param([string]$Rvc = 'experiments\rvc_opt')
$names = 'alu', 'load', 'store', 'branch', 'mem', 'call', 'mix', 'last'
foreach ($k in 0..6) {
    for ($try = 0; $try -lt 2000; $try++) {
        $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --rvc $Rvc --define NO_PAGING --image classes --no-viz --no-stdin --stats-from "P$k" --until "P$($k + 1)" --stats-after 1 2>&1 | ForEach-Object { "$_" }
        if (-not ($o | Where-Object { $_ -match 'refused' })) { break }
        Start-Sleep -Seconds 1
    }
    $s = $o | Where-Object { $_ -match '^STATS seconds' } | Select-Object -Last 1
    $line = "classes P$k : $s"
    if (-not $s) { $line = "classes P$k : " + (($o | Select-Object -Last 2) -join ' ') }
    $line | Add-Content experiments\rv_fuse\classes.log
    $line
}
