# The head-to-head, each load on each machine in turn, twice flat out, once in each world's frame.
#   pwsh experiments\rv_fuse\h2h\all.ps1 [-Loads shell,seq,gzip,doom,quake,nxray] [-Modes flat,world,vr] [-Rounds 2]
param([string]$Loads = 'shell,seq,gzip,doom,quake,nxray', [string]$Modes = 'flat,world,vr', [int]$Rounds = 2)
foreach ($r in 1..$Rounds) { foreach ($m in $Modes -split ',') { foreach ($l in $Loads -split ',') { foreach ($k in 'gvm', 'rv') {
    if ($m -ne 'flat' -and $r -gt 1) { continue }
    python experiments\rv_fuse\h2h\h2h.py $k $l --mode $m 2>&1 | Out-Null
    "$r $m $l $k done"
} } } }
