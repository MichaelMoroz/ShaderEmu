# Each load on each machine in turn.  pwsh experiments\rv_vm\all.ps1 [-Loads a,b] [-Machines rv,gvm] [-Rounds 2]
param([string]$Loads = 'boot,gzip,jpeg,png,mp3,mp3_1,flac,mpeg,avi,tdawn', [string]$Machines = 'rv,gvm', [int]$Rounds = 2, [string]$Tick = '', [string]$Tag = '', [string]$Image = '')
foreach ($r in 1..$Rounds) { foreach ($l in $Loads -split ',') { foreach ($k in $Machines -split ',') {
    $x = @('experiments\rv_vm\h2h.py', $k, $l)
    if ($k -eq 'gvm' -and $Tick) { $x += @('--tick', $Tick) }
    if ($k -eq 'gvm' -and $Tag) { $x += @('--tag', $Tag) }
    if ($k -eq 'gvm' -and $Image) { $x += @('--image', $Image) }
    python @x | Out-Null
    "$r $l $k done"
} } }
