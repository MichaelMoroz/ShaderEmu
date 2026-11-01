# The bundle ticks: first a run in software (it must get on with the guest), then each load in one run, ticks alternating.
#   pwsh experiments\rv_fuse\w3.ps1 [-Ticks w3,w3f] [-Set w3] [-Frames 5000] [-Loads shell,doom,quake,nxray]
param([string]$Ticks = 'w3,w3f', [string]$Set = 'w3', [int]$Frames = 5000, [string]$Loads = 'shell,doom,quake,nxray', [switch]$NoWarp)
$d = 'C:\Users\micha\agents_isa\snap'
if (-not $NoWarp) {
    foreach ($t in ($Ticks -split ',')) {
        $env:RVC_CODE_FILE = "$d\$Set\shell.code"
        Remove-Item Env:RVC_TICK_ALT -ErrorAction SilentlyContinue
        for ($try = 0; $try -lt 200000; $try++) {
            $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 bin\rvc_harness.exe --warp --d3d11 --rvc "experiments\rvc_opt_$t" --define NO_PAGING --image C:\Users\micha\agents_isa\img --no-desktop --no-viz --no-stdin --load-state "$d\shell\shell.snap" --frames 120 --frame-log "logs\warp_$t.frames" --uart-log "logs\warp_$t.uart" 2>&1 | ForEach-Object { "$_" }
            if (-not ($o | Where-Object { $_ -match 'refused' })) { break }
            Start-Sleep -Seconds 1
        }
        $txt = @("== $(Get-Date -Format HH:mm) WARP $t") + ($o | Select-Object -Last 3) + (python tools\tick_util.py "logs\warp_$t.frames" 2>&1 | Select-Object -First 3 | ForEach-Object { "$_".Substring(0, [Math]::Min(170, "$_".Length)) })
        $txt | Add-Content experiments\rv_fuse\alt2.log
        $txt
    }
}
foreach ($l in ($Loads -split ',')) {
    pwsh -NoProfile -File experiments\rv_fuse\alt2.ps1 -Load $l -Ticks $Ticks -Code "$d\$Set\$l.code" -Frames $Frames 2>&1 | ForEach-Object { "$_" }
}
