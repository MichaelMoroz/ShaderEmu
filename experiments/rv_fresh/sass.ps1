# The card's code of a step of the fresh tick: its D3D11 blob as DXIL on D3D12 under Nsight (the runner's own draw with this pixel shader), then the trace's SASS.
#   pwsh experiments\rv_fresh\sass.ps1 -Name s12 -Hlsl experiments\rv_fresh\steps\s12.hlsl
param([string]$Name, [string]$Hlsl, [int]$Dur = 100, [int]$Wait = 400000, [string]$Snap = 'C:\Users\micha\agents_isa\snap\shell\shell.snap', [string]$Image = 'C:\Users\micha\agents_isa\img',
      [string]$Ngfx = 'C:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.3.1\host\windows-desktop-nomad-x64\ngfx.exe')
$Root = (Get-Location).Path
$blob = Join-Path $Root "logs\$Name.cso"
& bin\fresh.exe $Hlsl logs\fresh_empty.bin --warp --core 8x8 --budget 16 --passes 32 --out $blob | Out-Null
$out = Join-Path $Root "logs\nsight\$Name"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
$env:RVC_DXIL = '1'; $env:RVC_PS_CSO = $blob; $env:RVC_CODE_FILE = ''
$appArgs = "--rvc experiments\rvc_lean --define NO_PAGING --no-doubles --payload $Image --load-state $Snap --seconds 8"
$cmd = @"
`$p = Start-Process -FilePath '$Ngfx' -ArgumentList @('--activity','"GPU Trace Profiler"','--exe','$Root\bin\rvc_trace12.exe','--dir','$Root','--args','"$appArgs"','--start-after-ms','2500','--max-duration-ms','$Dur','--output-dir','$out') -NoNewWindow -PassThru -RedirectStandardOutput '$out\ngfx.out' -RedirectStandardError '$out\ngfx.err'
if (-not `$p.WaitForExit($Wait)) { Stop-Process -Id `$p.Id -Force; Get-Process rvc_trace12 -ErrorAction SilentlyContinue | Stop-Process -Force }
"@
Set-Content -Path "$out\go.ps1" -Value $cmd
pwsh -NoProfile -File tools\gpu_queue.ps1 pwsh -NoProfile -File "$out\go.ps1" 2>&1 | Select-Object -Last 3
Get-Content "$out\ngfx.out" -Tail 8 -ErrorAction SilentlyContinue
$env:RVC_DXIL = ''; $env:RVC_PS_CSO = ''
Get-ChildItem $out -Recurse -Filter *.ngfx-gputrace | ForEach-Object { $_.FullName; python tools\nsight_sass.py $_.FullName --top 25 }
