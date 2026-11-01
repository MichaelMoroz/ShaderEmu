# One tick on one snapshot on the card, one line. -NoCode runs without the code file; -Zero with a code area of zeros (nothing translated).
#   pwsh experiments\rv_fuse\one.ps1 -Tick experiments\rvc_lean -Load shell [-Frames 3000] [-Zero] [-More "--unity 11.1,7.1,1.0"]
param([string]$Tick, [string]$Load = 'shell', [int]$Frames = 3000, [switch]$NoCode, [switch]$Zero, [string]$More = '', [string]$Code = '',
      [string]$Dir = 'C:\Users\micha\agents_isa\snap', [string]$Image = 'C:\Users\micha\agents_isa\img', [string]$Name = '')
$snap = Join-Path $Dir "$Load\$Load.snap"
$env:RVC_CODE_FILE = if ($Code) { $Code } elseif ($Zero) { Join-Path $Dir 'zero.code' } elseif ($NoCode) { '' } else { Join-Path $Dir "$Load\$Load.code" }
$extra = @($More -split ' ' | Where-Object { $_ })
$log = "logs\one_$PID.frames"
for ($i = 0; $i -lt 20000; $i++) {
    $o = & pwsh -NoProfile -File tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --rvc $Tick --define NO_PAGING --image $Image --no-desktop --no-viz --no-stdin --load-state $snap --frames $Frames --stats-after 0.5 --frame-log $log @extra 2>&1 | ForEach-Object { "$_" }
    if (-not ($o | Select-String 'REFUSED')) { break }
    Start-Sleep 1
}
$env:RVC_CODE_FILE = ''
$t = [regex]::Match(($o -join "`n"), 'tick ([0-9.]+) ms, commit ([0-9.]+)')
$r = ($o | Where-Object { $_ -match '^STATS' -and $_ -match 'MIPS|instr' } | Select-Object -Last 1)
$u = python tools\tick_util.py $log 2>&1 | Select-Object -First 2 | ForEach-Object { "$_".Substring(0, [Math]::Min(110, "$_".Length)) }
"{0,-26} {1,-6} tick {2} commit {3} | {4} | {5}" -f ($(if ($Name) { $Name } else { Split-Path $Tick -Leaf })), $Load, $t.Groups[1].Value, $t.Groups[2].Value, $r, ($u -join ' / ')
if (-not $t.Success) { $o | Select-Object -Last 4 }
Remove-Item $log -ErrorAction SilentlyContinue
