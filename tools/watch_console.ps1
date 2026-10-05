# Live view of the emulated machine's console: follows logs\uart.log byte by byte, so prompts
# without a trailing newline show up at once. One window serves every run (the harness appends).
param([string]$Log)

$Root = Split-Path -Parent $PSScriptRoot
if (-not $Log) { $Log = Join-Path $Root 'logs\uart.log' }
$Dir = Split-Path -Parent $Log
New-Item -ItemType Directory -Force $Dir | Out-Null
if (-not (Test-Path $Log)) { New-Item -ItemType File $Log | Out-Null }
Set-Content (Join-Path $Dir 'console_viewer.pid') $PID

$Host.UI.RawUI.WindowTitle = "emulator console - $Log"
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$fs = [IO.File]::Open($Log, 'Open', 'Read', 'ReadWrite,Delete')
$null = $fs.Seek([Math]::Max(0, $fs.Length - 4096), 'Begin')   # start with the recent tail
$buf = New-Object byte[] 65536
$dec = [Text.Encoding]::UTF8.GetDecoder()
$chars = New-Object char[] 65536
while ($true) {
    if ($fs.Position -gt $fs.Length) { $null = $fs.Seek(0, 'Begin') }   # log was truncated
    $n = $fs.Read($buf, 0, $buf.Length)
    if ($n -gt 0) {
        $c = $dec.GetChars($buf, 0, $n, $chars, 0)
        [Console]::Write($chars, 0, $c)
    } else {
        Start-Sleep -Milliseconds 50
    }
}
