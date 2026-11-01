# Core 0's traces of real loads under rvc_cpu (needs trace.patch built): 16 bytes an instruction, pc, word, rs1's value, rs2's value.
param([string]$Out = 'C:\Users\micha\agents_isa\ni\trace', [long]$N = 6000000)
$main = (git -C "$PSScriptRoot\..\.." worktree list --porcelain | Select-Object -First 1) -replace '^worktree ', ''
$cpu = "$PSScriptRoot\..\..\bin\rvc_cpu.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$td = 'nano-X -p & sleep 2; TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL; echo DO""NE'
$loads = @{
    boot   = @(0, 'true; echo DO""NE')
    tdawn  = @(90000000, $td)
    mp3    = @(40000000, 'media_test audio /usr/share/play-test.mp3 /tmp/o.raw 0; echo DO""NE')
    png    = @(12000000, 'nxview --decode /usr/share/picture-fox.png /tmp/a.ppm; echo DO""NE')
    jpeg   = @(9000000, 'NXVIEW_WORKERS=0 nxview --decode /usr/share/picture-fox.jpg /tmp/b.ppm; echo DO""NE')
    gzip   = @(30000000, 'seq 1 20000 > /tmp/g; gzip -c /tmp/g > /dev/null; gzip -c /tmp/g > /dev/null; echo DO""NE')
}
foreach ($k in $loads.Keys) {
    $env:RVC_NI_TRACE = "$Out\$k.bin"; $env:RVC_NI_SKIP = $loads[$k][0]; $env:RVC_NI_N = $N
    $env:RVC_THREADS = '1'
    $f = "$Out\$k.bin"
    & $cpu --image "$main\build\images\nommu" --quiet --expect '/ # ' --send ($loads[$k][1] + '\n') --until 'DONE' --instructions ($loads[$k][0] + $N + 2000000) --wall 240 2>&1 | Select-Object -Last 1
    "$k $((Get-Item $f).Length / 16)"
}
