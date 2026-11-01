# PC samples of a step on real code: fresh12 under Nsight, a short capture and a long wait (a trace killed in its transfer leaves the counters unavailable).
#   pwsh experiments\rv_fresh\trace12.ps1 -Name t15 -Hlsl experiments\rv_fresh\steps\s15.hlsl -Load doom -Passes 4 [-Code logs\x.bin -Lo 0x86405000u]
param([string]$Name, [string]$Hlsl, [string]$Load = 'doom', [int]$Passes = 4, [string]$Code = '', [string]$Lo = '', [int]$Dur = 500, [int]$Wait = 400000,
      [string]$Ngfx = 'C:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.3.1\host\windows-desktop-nomad-x64\ngfx.exe')
$Root = (Get-Location).Path
$snap = "C:\Users\micha\agents_isa\snap\$Load\$Load.snap"
$dirty = "$Root\logs\dirty_$Load.bin"
python experiments\rv_fresh\dirty.py $snap $dirty
$out = "$Root\logs\nsight\$Name"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
$a = "$Hlsl experiments\rv_fresh\steps\commit.hlsl $snap $dirty --passes $Passes --seconds 8"
if ($Code) { $a += " --code $Code -D CODE_LO=$Lo" }
$cmd = @"
`$p = Start-Process -FilePath '$Ngfx' -ArgumentList @('--activity','"GPU Trace Profiler"','--exe','$Root\bin\fresh12.exe','--dir','$Root','--args','"$a"','--start-after-ms','3000','--max-duration-ms','$Dur','--pc-samples-per-pm-interval-per-sm','64','--real-time-shader-profiler','--output-dir','$out') -NoNewWindow -PassThru -RedirectStandardOutput '$out\ngfx.out' -RedirectStandardError '$out\ngfx.err'
if (-not `$p.WaitForExit($Wait)) { Stop-Process -Id `$p.Id -Force; Get-Process fresh12 -ErrorAction SilentlyContinue | Stop-Process -Force }
"@
Set-Content -Path "$out\go.ps1" -Value $cmd
pwsh -NoProfile -File tools\gpu_queue.ps1 pwsh -NoProfile -File "$out\go.ps1" 2>&1 | Select-Object -Last 3
Get-Content "$out\ngfx.out" -Tail 6 -ErrorAction SilentlyContinue
Get-ChildItem $out -Recurse -Filter *.ngfx-gputrace | ForEach-Object { $_.FullName }
