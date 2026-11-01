# Tiberian Dawn's window (rv_vm/h2h.py's load) on rvc_cpu under RVC_PROF: prof_td.ps1 OUT IMAGE [-Play]
# -Play: from frame 1's mark to frame 1300's, without the level's load.
param([string]$Out, [string]$Image, [switch]$Play, [string]$Extra = '')
$main = (git worktree list --porcelain | Select-Object -First 1).Substring(9)
$env:RVC_THREADS = '1'; $env:RVC_WCACHE = '1'
$td = "TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 $Extra TDAWN_SCEN=3 TDAWN_FRAMES=1300"
if ($Play) {
    python tools/prof.py run $Out --exe "$main\bin\rvc_cpu.exe" -- --image $Image --quiet --expect "/ # " --send "nano-X -p & sleep 2; TDAWN_MARK=1,1299 $td tdawn FROMINSTALL; echo H2H'' B\n" --count-from ": mark begins" --until "H2H B" --seconds 900 2>&1 | Select-String 'COUNT|passes in|state|done at|mark'
} else {
    python tools/prof.py run $Out --exe "$main\bin\rvc_cpu.exe" -- --image $Image --quiet --expect "/ # " --send "nano-X -p & sleep 2; echo H2H'' A; $td tdawn FROMINSTALL; echo H2H'' B\n" --count-from "H2H A" --until "H2H B" --seconds 900 2>&1 | Select-String 'COUNT|passes in|state|done at'
}
