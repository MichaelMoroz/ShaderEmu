# The card's code of the tick with a scratch array: its blob by a short WARP run, as DXIL on D3D12 under Nsight, then the trace's SASS (rv_fuse/sass.ps1 with the main checkout's programs).
#   pwsh experiments\rv_scratch\sass.ps1 -Name n64 -Defs SC_N=64    (no -Defs: the tree)
param([string]$Name, [string[]]$Defs = @(), [int]$Dur = 300, [int]$Wait = 400000, [string]$Load = 'gzip',
      [string]$Ngfx = 'C:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.3.1\host\windows-desktop-nomad-x64\ngfx.exe')
$Root = (Get-Location).Path
$Main = ((git worktree list --porcelain)[0] -split ' ', 2)[1]
$Harness = Join-Path $Main 'bin\rvc_harness.exe'; $Runner = Join-Path $Main 'bin\rvc_trace12.exe'; $Image = Join-Path $Main 'build\images\nommu'
$snap = "C:\Users\micha\agents_isa\ni\$Load\$Load.snap"
$cache = Join-Path $Root "build\sc_sass\$Name"
if (Test-Path $cache) { Remove-Item -Recurse -Force $cache }
New-Item -ItemType Directory -Force $cache | Out-Null
$h = @('--d3d11', '--warp', '--rvc', 'experiments\rvc_lean_sc', '--define', 'NO_PAGING', '--image', $Image, '--cache', $cache, '--no-desktop', '--no-viz', '--no-stdin', '--load-state', $snap, '--frames', '1')
foreach ($d in $Defs) { $h += @('--define', $d) }
for ($i = 0; $i -lt 200; $i++) { $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 $Harness @h 2>&1 | ForEach-Object { "$_" }; if ($LASTEXITCODE -ne 75) { break }; Start-Sleep 3 }
$picked = python experiments/rv_fuse/acct/pick_tick.py $cache
if (-not $picked) { $o | Select-Object -Last 15; throw 'no tick blob' }
$blob = Join-Path $cache 'tick.cso'
Copy-Item $picked $blob -Force
"blob: $picked $((Get-Item $blob).Length) bytes"
$out = Join-Path $Root "build\sc_sass\nsight_$Name"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
$env:RVC_DXIL = '1'; $env:RVC_PS_CSO = $blob; $env:RVC_CODE_FILE = ''
$appArgs = "--rvc experiments\rvc_lean_sc --define NO_PAGING --no-doubles --payload $Image --load-state $snap --seconds 8"
$cmd = @"
`$p = Start-Process -FilePath '$Ngfx' -ArgumentList @('--activity','"GPU Trace Profiler"','--exe','$Runner','--dir','$Root','--args','"$appArgs"','--start-after-ms','2500','--max-duration-ms','$Dur','--pc-samples-per-pm-interval-per-sm','64','--auto-export','--architecture','"Blackwell GB20x"','--real-time-shader-profiler','--set-gpu-clocks','unaltered','--output-dir','$out') -RedirectStandardOutput '$out\ngfx.out' -RedirectStandardError '$out\ngfx.err' -NoNewWindow -PassThru
if (-not `$p.WaitForExit($Wait)) { Stop-Process -Id `$p.Id -Force; Get-Process rvc_trace12 -ErrorAction SilentlyContinue | Stop-Process -Force }
"@
Set-Content -Path "$out\go.ps1" -Value $cmd
pwsh -NoProfile -File tools\gpu_queue.ps1 pwsh -NoProfile -File "$out\go.ps1" 2>&1 | Select-Object -Last 3
Get-Content "$out\ngfx.out" -Tail 8 -ErrorAction SilentlyContinue
$env:RVC_DXIL = ''; $env:RVC_PS_CSO = ''
Get-ChildItem $out -Recurse -Filter *.ngfx-gputrace | ForEach-Object { $_.FullName; python tools\nsight_sass.py $_.FullName --top 25 }
