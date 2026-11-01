# A private image: the main checkout's flat root and kernel with this build's programs, each "PATH-IN-ROOT=ELF-UNDER-WORK":
#   mkimg.ps1 NAME -Prog usr/bin/nxview=src/apps/nxview,usr/lib/tdawn/game=src/tdawn-build/vanillatd
param([string]$Name, [string[]]$Prog, [string]$Root = 'build/rv_play/root')
$main = (git worktree list --porcelain | Select-Object -First 1).Substring(9)
foreach ($p in 'build\images\linux', 'rvc') {
    if (-not (Test-Path $p)) { New-Item -ItemType Directory -Force (Split-Path $p -Parent) -ErrorAction SilentlyContinue | Out-Null; New-Item -ItemType Junction -Path $p -Target "$main\$p" | Out-Null }
}
if (-not (Test-Path $Root)) { New-Item -ItemType Directory -Force (Split-Path $Root) | Out-Null; Copy-Item -Recurse "$main\build\nommu\flatroot" $Root }
foreach ($x in ($Prog -split ',')) {
    $a, $b = $x -split '='
    wsl -- bash experiments/rv_play/env.sh flat $Root $a "/home/micha/agents/play/nm/work/$b"
}
$env:HALFLIFE_DATA = '0'
python linux/nommu/image.py $Name $Root "$main\build\nommu\Image" | Select-Object -Last 1
