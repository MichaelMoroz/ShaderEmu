# passlog.ps1 under the card's model of the write caches and walks (rvc_cpu), where a worker's job takes as many passes as on the card: passlog2.ps1 IMAGE OUT [-Env "A=1"]
param([string]$Image, [string]$Out, [string]$Env = '')
$env:RVC_WCACHE = '1'
$env:RVC_ADFAULT = '1'
& pwsh experiments/rv_play/passlog.ps1 $Image $Out -Env $Env -Every 1
