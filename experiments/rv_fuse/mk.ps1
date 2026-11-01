# Make a wide tick and write its stamp (a test of cost on the card: only ticks that ran a machine on WARP are checked by that).
#   pwsh experiments\rv_fuse\mk.ps1 NAME [make_wide's arguments]
$name = $args[0]; $rest = $args[1..($args.Count)]
$t = "experiments\rvc_opt_$name"
python experiments\rv_fuse\make_wide.py $t @rest | Out-Null
$h = (& bin\rvc_harness.exe --rvc $t --stamp 2>&1 | Select-Object -Last 1)
Set-Content -NoNewline -Path "$t\warp_ok" -Value ("$h" -split ' ')[1]
