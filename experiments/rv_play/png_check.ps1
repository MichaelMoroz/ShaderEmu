# The one-stream PNG and inflate: host's tests, then rvc_cpu with RVC_RACES and RVC_RACES_NEAR on an image (png_check.ps1 IMAGE)
param([string]$Image)
$main = (git worktree list --porcelain | Select-Object -First 1).Substring(9)
$mt = '/home/micha/agents/play/nm/work/src/libmedia/media_test-host'
wsl -- python3 linux/libmedia/test/check_pictures.py $mt --wsl 2>&1 | Select-Object -Last 3
wsl -- python3 experiments/rv_play/inflate_check.py $mt 2>&1 | Select-Object -Last 3
$env:RVC_THREADS = '8'
$r = Join-Path $env:TEMP 'play_races.txt'
foreach ($cmd in 'media_test decode /usr/share/picture-fox.png /tmp/o 63 3', 'media_test decode /usr/share/picture-fox.png /tmp/o 16 4', 'MT_OPEN=63 MT_BUSY=20 media_test decode /usr/share/picture-fox.png /tmp/o 63 4', 'nxview --decode /usr/share/picture-fox.png /tmp/a.ppm', 'MEDIA_PNG_ONE=1 media_test decode /usr/share/picture-fox.png /tmp/o 16 3') {
    Remove-Item $r -ErrorAction SilentlyContinue
    $env:RVC_RACES = $r
    $env:RVC_RACES_NEAR = '4'
    $o = & "$main\bin\rvc_cpu.exe" --image $Image --quiet --uart-log "$r.uart" --expect "/ # " --send "$cmd; echo H2H'' B\n" --until "H2H B" --seconds 300 2>&1
    Remove-Item Env:RVC_RACES, Env:RVC_RACES_NEAR
    "== $cmd"
    Select-String -Path "$r.uart" -Pattern 'sum|nxview:' | Select-Object -Last 2 | % { $_.Line.Trim() }
    Remove-Item "$r.uart" -ErrorAction SilentlyContinue
    $o | Select-String 'races|undone|near' | Select-Object -First 3 | % { $_.Line.Trim() }
}
