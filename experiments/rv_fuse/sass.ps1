# The card's code of a tick: its D3D11 blob as DXIL on D3D12 under Nsight, then the trace's SASS.  Run through tools\gpu_queue.ps1 by this script.
#   pwsh experiments\rv_fuse\sass.ps1 -Name tree -Rvc experiments\rvc_lean [-Stamp]    (the blob comes from a short WARP run of the harness)
param([int]$Wait = 400000, [int]$Ticks = 0, [int]$Dur = 400, [string]$Name, [string]$Rvc, [string]$Code = '', [string]$Load = 'shell', [string]$Dir = 'C:\Users\micha\agents_isa\snap', [string]$Image = 'C:\Users\micha\agents_isa\img',
      [string]$Ngfx = 'C:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.3.1\host\windows-desktop-nomad-x64\ngfx.exe')
$snap = Join-Path $Dir "$Load\$Load.snap"
$Root = (Get-Location).Path
$env:RVC_CODE_FILE = ''
# the harness prints nothing of the blob's name: the newest large one after a short run is it (delete it first, so it is made anew)
Get-ChildItem build\shadercache -File -ErrorAction SilentlyContinue | Remove-Item -Force
for ($i = 0; $i -lt 20000; $i++) { $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --warp --rvc $Rvc --define NO_PAGING --image $Image --no-desktop --no-viz --no-stdin --load-state $snap --frames 1 2>&1 | ForEach-Object { "$_" }; if (-not ($o | Select-String 'REFUSED')) { break }; Start-Sleep 1 }
$b = Get-Item (python experiments/rv_fuse/acct/pick_tick.py build/shadercache)
$blob = Join-Path $Root "logs\$Name.cso"
Copy-Item $b.FullName $blob -Force
"blob: $($b.Name) $($b.Length) " + (python tools\cso_loops.py $blob | Select-Object -First 6 | Out-String)
$out = Join-Path $Root "logs\nsight\$Name"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
$env:RVC_DXIL = '1'; $env:RVC_PS_CSO = $blob
if ($Code) { $env:RVC_CODE_FILE = $Code }
$appArgs = "--rvc $Rvc --define NO_PAGING --no-doubles --payload $Image --load-state $snap --seconds 8 $(if ($Ticks) { "--ticks $Ticks" })"
$cmd = @"
`$p = Start-Process -FilePath '$Ngfx' -ArgumentList @('--activity','"GPU Trace Profiler"','--exe','$Root\bin\rvc_trace12.exe','--dir','$Root','--args','"$appArgs"','--start-after-ms','2500','--max-duration-ms','$Dur','--pc-samples-per-pm-interval-per-sm','64','--auto-export','--architecture','"Blackwell GB20x"','--real-time-shader-profiler','--set-gpu-clocks','unaltered','--output-dir','$out') -RedirectStandardOutput '$out\ngfx.out' -RedirectStandardError '$out\ngfx.err' -NoNewWindow -PassThru
if (-not `$p.WaitForExit($Wait)) { Stop-Process -Id `$p.Id -Force; Get-Process rvc_trace12 -ErrorAction SilentlyContinue | Stop-Process -Force }
"@
Set-Content -Path "$out\go.ps1" -Value $cmd
pwsh -NoProfile -File tools\gpu_queue.ps1 pwsh -NoProfile -File "$out\go.ps1" 2>&1 | Select-Object -Last 3
Get-Content "$out\ngfx.out" -Tail 6 -ErrorAction SilentlyContinue
$env:RVC_DXIL = ''; $env:RVC_PS_CSO = ''
Get-ChildItem $out -Recurse -Filter *.ngfx-gputrace | ForEach-Object { $_.FullName; python tools\nsight_sass.py $_.FullName --top 25 }
