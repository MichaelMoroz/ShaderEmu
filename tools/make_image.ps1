# Makes the Linux images (tools\make_linux_image.py, then linux\nommu\image.py on it), one caller
# at a time: sessions and agents share build\images, and a run that reads an image being written fails.
#   pwsh tools\make_image.ps1 [arguments of make_linux_image.py] [--aoe]
# --aoe: Age of Empires in the no-MMU image, its trial fetched: docs/games/aoe.md
$repo = Split-Path -Parent $PSScriptRoot
$lock = Join-Path $repo 'build\image.lock'
$waited = 0
while ($true) {
    try { New-Item -ItemType Directory -Path $lock -ErrorAction Stop | Out-Null; break }
    catch {
        if ($waited -ge 180) { Write-Output 'make_image: the lock was held for 180 s, taking it'; break }
        Start-Sleep -Seconds 2; $waited += 2
    }
}
$passed = @($args | Where-Object { $_ -ne '--aoe' })    # (inside the block below $args is the block's own, and empty)
$aoe = @($args | Where-Object { $_ -eq '--aoe' })
try {
    $t = Measure-Command { python (Join-Path $PSScriptRoot 'make_linux_image.py') @passed | Out-Host }
    Write-Output ("make_linux_image: {0:n1} s, exit {1}" -f $t.TotalSeconds, $LASTEXITCODE)
    # and Linux without the MMU on it (docs/system/nommu.md), the machine the harness and the world run
    $save = @($passed | Where-Object { $_ -eq '--save-prebuilt' })
    $t = Measure-Command { python (Join-Path $repo 'linux\nommu\image.py') @save @aoe | Select-Object -Last 2 | Out-Host }
    Write-Output ("nommu image: {0:n1} s, exit {1}" -f $t.TotalSeconds, $LASTEXITCODE)
} finally {
    Remove-Item -Recurse -Force $lock -Confirm:$false -ErrorAction SilentlyContinue
}
