# Tick utilization of a workload on the shader machine (D3D11 + fxc2), from power-on: how much
# of each pass's instructions the guest ran, and what ended the passes that ran fewer.
#   pwsh tools\tick_util.ps1 -Name doom1 -Cmd "nano-X -p & sleep 2; doom -1 -timedemo demo1" -From 'doom: ' -Until 'doom: stopped' -Seconds 50 -Nm doom=build\tick_util\doom.nm
# -Cmd is typed at the prompt. Frames are counted from where the console first says -From
# (default FL-GO: put  echo FL-''GO;  into -Cmd). -Nm name=FILE: `nm -n` of a program, to name
# where its passes ended (the kernel's is made here). -Name must be new: logs\tick_NAME.log.
param([string]$Name, [string]$Cmd = '', [string]$From = 'FL-GO', [string]$Until = 'LX-DONE', [int]$Seconds = 60,
      [string[]]$Nm = @(), [string]$Rvc = 'shader_vm', [string[]]$More = @(), [int]$Top = 10, [switch]$Whole)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$out = 'build\tick_util'
New-Item -ItemType Directory -Force $out, logs | Out-Null
$log = "logs\tick_$Name.log"
if (Test-Path $log) { throw "log $log exists: pick a new name" }
# the harness boots Linux without the MMU once its image is made (docs/system/nommu.md), and its kernel's names
$nommu = Test-Path build\images\nommu\linux_payload.bin
$kernel = if ($nommu) { "$out\vmlinux-nommu.nm" } else { "$out\vmlinux.nm" }
$image = if ($nommu) { 'build\images\nommu\Image' } else { 'build\images\linux\Image' }
$vmlinux = if ($nommu) { '~/nm/k-rom/vmlinux' } else { 'linux/vmlinux' }
if (-not (Test-Path $kernel) -or (Get-Item $kernel).LastWriteTime -lt (Get-Item $image).LastWriteTime) {
    wsl -- bash -c "cd ~/shaderemu-linux && toolchain/bin/riscv32-linux-nm -n $vmlinux" | Set-Content $kernel -Encoding ascii
}
$all = @('--rvc', $Rvc, '--image', 'linux-net', '--no-stdin', '--uart-log', $log,
         '--frame-log', "$out\$Name.frames", '--until', $Until, '--seconds', "$Seconds")
if ($Cmd -ne '') { $all += @('--expect', '/ # ', '--send', ($Cmd + "; echo LX-''DONE\n")) }
if (-not $Whole) { $all += @('--stats-from', $From) }
$all += $More
# (one run on the card at a time, for every session: tools\gpu_queue.ps1)
$t = Measure-Command { & tools\gpu_queue.ps1 bin\rvc_harness.exe @all 2>&1 | Select-Object -Last 3 | Out-Host }
"run ${Name}: {0:n1} s, exit {1}, gpu {2}" -f $t.TotalSeconds, $LASTEXITCODE, (nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader)
python tools\tick_util.py "$out\$Name.frames" "kernel=$kernel" @Nm --top $Top --json "$out\$Name.json"
