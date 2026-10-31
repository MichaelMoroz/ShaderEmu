# A variant of the fused tick: made, run a little on WARP, stamped, timed on the plain and the fused snapshot.
#   pwsh experiments\rv_fuse\try.ps1 NAME -MakeArgs '--set','addi','--shape','noret' [-Load shell] [-Fused shell.addi.snap] [-Frames 3000]
param([string]$Name, [string]$MakeArgs = "", [string]$Load = 'shell', [string]$Fused = 'shell.addi.snap', [int]$Frames = 3000, [switch]$SkipPlain)
$dir = "experiments\rvc_opt_$Name"
$made = @($MakeArgs -split ' ' | Where-Object { $_ })
python experiments\rv_fuse\make_tick.py $dir @made
pwsh -NoProfile -File experiments\rv_fuse\card.ps1 -Load $Load -Rvc $dir -Fused $Fused -Rounds 1 -Frames 100 -Warp -OnlyB | Select-String 'ips=.*per_frame|rror' | ForEach-Object { "warp: $($_.Line)" }
$s = (bin\rvc_harness.exe --rvc $dir --stamp).Split(' ')[1]
Add-Content "$dir\warp_ok" "$s  WARP ran 100 frames of a fused snapshot"
if (-not $SkipPlain) { pwsh -NoProfile -File experiments\rv_fuse\card.ps1 -Load $Load -Rvc $dir -Fused "$Load.snap" -Rounds 1 -Frames $Frames -OnlyB | Select-String 'gpu per|all cores' | ForEach-Object { "plain $($_.Line)" } }
pwsh -NoProfile -File experiments\rv_fuse\card.ps1 -Load $Load -Rvc $dir -Fused $Fused -Rounds 1 -Frames $Frames -OnlyB | Select-String 'gpu per|all cores' | ForEach-Object { "fused $($_.Line)" }
