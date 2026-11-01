# Make a bundle tick and write its stamp (a test of cost on the card; each is run on the guest and its output looked at).
#   pwsh experiments\rv_fuse\mk3.ps1 NAME [make_w3's arguments]
$name = $args[0]; $rest = $args[1..($args.Count)]
$t = "experiments\rvc_opt_$name"
python experiments\rv_fuse\make_w3.py $t @rest | Out-Null
$h = (& bin\rvc_harness.exe --rvc $t --stamp 2>&1 | Select-Object -Last 1)
Set-Content -NoNewline -Path "$t\warp_ok" -Value ("$h" -split ' ')[1]
"$name $h"
