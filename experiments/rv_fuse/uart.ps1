# Is the wide tick still the machine? Each tick on a load's snapshot for many frames, with the console kept: what the guest printed.
#   pwsh experiments\rv_fuse\uart.ps1 -Load shell -Shapes "1 33" [-Frames 12000]
param([string]$Load, [string]$Shapes = '1', [int]$Frames = 12000, [string]$Dir = 'C:\Users\micha\agents_isa\snap', [string]$Image = 'C:\Users\micha\agents_isa\img')
$snap = Join-Path $Dir "$Load\$Load.snap"
$code = Join-Path $Dir "$Load\$Load.code"
foreach ($t in @('tree') + ($Shapes -split ' ')) {
    $tick = if ($t -eq 'tree') { 'experiments\rvc_lean' } else { "experiments\rvc_lean_s$t" }
    $env:RVC_CODE_FILE = if ($t -eq 'tree') { '' } else { $code }
    $log = "logs\ua_$Load-$t.txt"
    Remove-Item $log -ErrorAction SilentlyContinue
    for ($try = 0; $try -lt 20000; $try++) {
        $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --rvc $tick --define NO_PAGING --image $Image --no-desktop --no-viz --no-stdin --load-state $snap --frames $Frames --uart-log $log 2>&1 | ForEach-Object { "$_" }
        if (-not ($o | Where-Object { $_ -match 'REFUSED by the harness lock' })) { break }
        Start-Sleep -Seconds 1
    }
    "--- $t : " + (($o | Where-Object { $_ -match '^STATS seconds' }) -join '')
    if (Test-Path $log) { "bytes " + (Get-Item $log).Length; Get-Content $log -Tail 8 | ForEach-Object { "$_".Substring(0, [Math]::Min(100, "$_".Length)) } }
}
$env:RVC_CODE_FILE = ''
