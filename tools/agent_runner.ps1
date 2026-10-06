# Lets Claude (whose shell on this PC is a Linux VM) run Windows-only build/test steps.
#
# Claude writes one command line to logs\agent\request.txt; this script runs it from the repo
# root, captures output to logs\agent\output.txt and writes logs\agent\status.txt when done.
# Only commands starting with an allowed prefix run, and shell operators are refused, so a
# request cannot chain arbitrary commands. Create logs\agent\cancel.txt to kill a running job.
# Stop the runner with Ctrl+C.

$Root = Split-Path -Parent $PSScriptRoot
$Dir = Join-Path $Root 'logs\agent'
New-Item -ItemType Directory -Force $Dir | Out-Null
$Allowed = @('build.bat', 'run_rvc.bat', 'bin\')
$Req = Join-Path $Dir 'request.txt'
$Out = Join-Path $Dir 'output.txt'
$Status = Join-Path $Dir 'status.txt'
$Cancel = Join-Path $Dir 'cancel.txt'

Write-Host "ShaderEmu agent runner: watching $Req (Ctrl+C to stop)"
Set-Content $Status "idle"
while ($true) {
    if (Test-Path $Req) {
        Start-Sleep -Milliseconds 300   # let the writer finish
        $cmd = (Get-Content $Req -Raw).Trim()
        Remove-Item $Req -Force
        Remove-Item $Cancel -Force -ErrorAction SilentlyContinue
        $id = Get-Date -Format 'yyyyMMdd-HHmmss'
        $ok = $false
        foreach ($a in $Allowed) { if ($cmd.StartsWith($a, [StringComparison]::OrdinalIgnoreCase)) { $ok = $true } }
        if ($cmd -match '[&|<>^%]' -or $cmd -match '\.\.') { $ok = $false }
        if (-not $ok) {
            Set-Content $Out "REFUSED: $cmd"
            Set-Content $Status "done $id rc=refused $cmd"
            Write-Host "[$id] refused: $cmd"
            continue
        }
        Write-Host "[$id] $cmd"
        Set-Content $Status "running $id $cmd"
        $p = Start-Process -FilePath 'cmd.exe' -ArgumentList "/c $cmd > `"$Out`" 2>&1" `
             -WorkingDirectory $Root -NoNewWindow -PassThru
        $handle = $p.Handle   # keeps ExitCode available after exit
        while (-not $p.HasExited) {
            if (Test-Path $Cancel) {
                & taskkill.exe /T /F /PID $p.Id | Out-Null
                Remove-Item $Cancel -Force -ErrorAction SilentlyContinue
                Write-Host "[$id] cancelled"
            }
            Start-Sleep -Milliseconds 500
        }
        $rc = $p.ExitCode
        Set-Content $Status "done $id rc=$rc $cmd"
        Write-Host "[$id] exit $rc"
    }
    Start-Sleep -Milliseconds 500
}
