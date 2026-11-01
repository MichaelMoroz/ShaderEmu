# A B A B, whole runs from one snapshot: the tree and a tick on its code file. Instructions a second, the guest's own figure, and what ended passes (the write cache).
#   pwsh experiments\rv_fuse\ab.ps1 -Load nxray -Tick w3y -Code ...\nxray.ft.code [-Frames 6000] [-Reps 2] [-Grep raystat]
param([string]$Load, [string]$Tick, [string]$Code, [int]$Frames = 6000, [int]$Reps = 2, [string]$Grep = 'raystat|doomstat|quakestat')
$d = 'C:\Users\micha\agents_isa\snap'
$snap = "$d\$Load\$Load.snap"
Remove-Item Env:RVC_TICK_ALT -ErrorAction SilentlyContinue
for ($r = 0; $r -lt $Reps; $r++) {
    foreach ($t in 'rvc_opt', "rvc_opt_$Tick") {
        if ($t -eq 'rvc_opt') { Remove-Item Env:RVC_CODE_FILE -ErrorAction SilentlyContinue } else { $env:RVC_CODE_FILE = $Code }
        $u = "logs\ab_$Load.uart"; $f = "logs\ab_$Load.frames"
        Remove-Item $u -ErrorAction SilentlyContinue
        for ($try = 0; $try -lt 200000; $try++) {
            $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --rvc "experiments\$t" --define NO_PAGING --image C:\Users\micha\agents_isa\img --no-desktop --no-viz --no-stdin --load-state $snap --frames $Frames --stats-after 1 --frame-log $f --uart-log $u 2>&1 | ForEach-Object { "$_" }
            if (-not ($o | Where-Object { $_ -match 'refused' })) { break }
            Start-Sleep -Seconds 1
        }
        $s = ($o | Where-Object { $_ -match '^STATS seconds' } | Select-Object -Last 1)
        $h = (($o | Where-Object { $_ -match '^\[harness\] ' } | Select-Object -Last 2) -join ' ')
        $g = @(Get-Content $u -ErrorAction SilentlyContinue | Where-Object { $_ -match $Grep -and $_ -notmatch 'error' })
        $bad = @(Get-Content $u -ErrorAction SilentlyContinue | Where-Object { $_ -match 'Oops|panic' }).Count
        $util = python tools\tick_util.py $f 2>&1 | Select-Object -First 14 | ForEach-Object { "$_" }
        $why = ($util | Where-Object { $_ -match 'full|cache|UTIL|passes,' } | ForEach-Object { $_.Substring(0, [Math]::Min(120, $_.Length)) }) -join ' / '
        $line = "$(Get-Date -Format HH:mm) $Load $t oops=$bad | $s | $h | $($g | Select-Object -Last 1) | $why"
        $line | Add-Content experiments\rv_fuse\ab.log
        $line
    }
}
