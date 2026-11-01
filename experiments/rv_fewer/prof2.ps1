# Profiles (whole, play) of a private image, attributed through inlining, into SCRATCH\NAME*: prof2.ps1 IMAGENAME SCRATCH
param([string]$Name, [string]$S)
pwsh experiments/rv_fewer/prof_td.ps1 "$S\$Name" "build/images/$Name" | Select-Object -First 2
pwsh experiments/rv_fewer/prof_td.ps1 "$S\${Name}_p" "build/images/$Name" -Play | Select-Object -First 2
$u = (wsl -- wslpath -a ($S -replace '\\','/'))
wsl -- python3 experiments/rv_fewer/attrib.py "$u/$Name" "~/agents/fewer/elfs/$Name" 85000040 0 110 > "$S\$Name.attr"
wsl -- python3 experiments/rv_fewer/attrib.py "$u/${Name}_p" "~/agents/fewer/elfs/$Name" 85000040 0 110 > "$S\${Name}_p.attr"
