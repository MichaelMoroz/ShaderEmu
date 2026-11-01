# A private image: the main checkout's flat root and kernel with this build's Tiberian Dawn. mkimg.ps1 NAME
param([string]$Name, [string]$Root = 'build/rv_fewer/root')
$main = (git worktree list --porcelain | Select-Object -First 1).Substring(9)
foreach ($p in 'build\images\linux', 'rvc') {
    if (-not (Test-Path $p)) { New-Item -ItemType Directory -Force (Split-Path $p -Parent) -ErrorAction SilentlyContinue | Out-Null; New-Item -ItemType Junction -Path $p -Target "$main\$p" | Out-Null }
}
if (-not (Test-Path $Root)) { New-Item -ItemType Directory -Force (Split-Path $Root) | Out-Null; Copy-Item -Recurse "$main\build\nommu\flatroot" $Root }
wsl -- bash experiments/rv_fewer/flat_game.sh $Root $Name
$env:HALFLIFE_DATA = '0'
python linux/nommu/image.py $Name $Root "$main\build\nommu\Image" | Select-Object -Last 2
