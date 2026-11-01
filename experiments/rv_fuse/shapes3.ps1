# What a bundle's shape costs: one tick, the code translated to several shapes, each load: ms a pass of 8192 RV against the tree in the same run.
#   pwsh experiments\rv_fuse\shapes.ps1 [-Tick w3f] [-Shapes "1 1,2 1,3 1,3 2"] [-Loads shell,doom] [-Frames 4000]
param([string]$Tick = 'w3f', [string]$Shapes = '1 1,2 1,3 1,3 2', [string]$Loads = 'shell,doom', [int]$Frames = 4000, [string]$More = '')
$d = 'C:\Users\micha\agents_isa\snap'
foreach ($l in ($Loads -split ',')) {
    foreach ($sh in ($Shapes -split ',')) {
        $o, $m = $sh -split ' '
        $code = "$d\w3\$l.$o.$m.code"
        $x = python experiments\rv_fuse\xlate3.py "$d\$l\$l.snap" "$d\$l" $code --ops $o --mems $m @(-split $More) 2>&1 | Select-Object -Last 1
        Remove-Item "logs\alt_$l.uart" -ErrorAction SilentlyContinue
        $r = pwsh -NoProfile -File experiments\rv_fuse\alt2.ps1 -Load $l -Ticks $Tick -Code $code -Frames $Frames 2>&1 | ForEach-Object { "$_" }
        $t = @($r | Where-Object { $_ -match 'STATS tick' } | ForEach-Object { if ($_ -match ': ([0-9.]+) ms') { [double]$Matches[1] } })
        $g = ($r | Where-Object { $_ -match '^guest' }) -match 'panic|Oops'
        $ratio = if ($t.Count -ge 2) { [math]::Round($t[1] / $t[0], 3) } else { '?' }
        $rvb = if ($x -match 'RV a bundle ([0-9.]+)') { [double]$Matches[1] } else { 0 }
        $line = "$Tick $More $l ops $o mems $m : RV a bundle $rvb tree $($t[0]) tick $($t[1]) ratio $ratio $(if ($g) { 'GUEST FAILED' })"
        $line | Add-Content experiments\rv_fuse\shapes.log
        $line
    }
}
