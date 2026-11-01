# A build of the tree with compiler settings of several words, its image and count, stopping if the program was not made anew:
#   try.ps1 NAME [-Flags "--param=a=1 --param=b=2"] [-Env "A=1"]
param([string]$Name, [string]$Flags = '', [string]$Env = '')
$f = "build/rv_fewer/try.env"
"export TDAWN_XFLAGS='$Flags'`n" + ($Env -split ' ' | Where-Object { $_ } | % { "export $_`n" }) -join '' | Set-Content -NoNewline $f
$s = "build/rv_fewer/try.sh"
@"
. $f
before=`$(stat -c %Y `$HOME/agents/fewer/nm/work/src/tdawn-build/vanillatd)
sleep 1
bash experiments/rv_fewer/env.sh build > /dev/null
after=`$(stat -c %Y `$HOME/agents/fewer/nm/work/src/tdawn-build/vanillatd)
[ "`$before" != "`$after" ] || { echo NOT REBUILT; grep -E 'error|undefined' `$HOME/agents/fewer/nm/logs/tdawn-build.sh.log | head -12; exit 9; }
`$HOME/shaderemu-linux/toolchain/bin/riscv32-linux-size `$HOME/agents/fewer/nm/work/src/tdawn-build/vanillatd | tail -1
"@ -replace "`r`n","`n" | Set-Content -NoNewline $s
wsl -- bash $s
if ($LASTEXITCODE) { exit 9 }
pwsh experiments/rv_fewer/mkimg.ps1 $Name | Select-Object -Last 1
pwsh experiments/rv_fewer/count.ps1 "build/images/$Name"
