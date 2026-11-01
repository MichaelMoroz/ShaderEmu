# Compiler settings, one a line of FILE, on the tree's C++: each is built in the private home, made flat and counted on rvc_cpu.
param([string]$File, [string]$Out = 'experiments/rv_fewer/flags.txt')
foreach ($f in Get-Content $File) {
    wsl -- bash experiments/rv_fewer/env.sh build "TDAWN_XFLAGS=$f" | Select-String 'exit [1-9]|rror' | Out-Host
    pwsh experiments/rv_fewer/mkimg.ps1 flagtry *> $null
    $r = pwsh experiments/rv_fewer/count.ps1 build/images/flagtry -One
    $t = wsl -- bash experiments/rv_fewer/env.sh run bash -c 'riscv32-linux-size $W/src/tdawn-build/vanillatd'
    Add-Content $Out ("== $f`n" + ($r -join "`n") + "`n" + ($t | Select-Object -Last 1))
}
