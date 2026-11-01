# What is left to measure for RESULTS.md: bundles against lean on all loads (whole runs in turn), D3D12 against D3D11 on one blob, nxray with no test's sums on both machines.
$env:RVC_CODE_FILE = ''
$py = 'experiments\rv_fuse\acct\run.py'
foreach ($r in 1..2) {
    foreach ($l in @(@('shell', 'rvc_lean_w3x', 'shell.3.2.code'), @('doom', 'rvc_lean_w3x', 'doom.3.2.code'), @('quake', 'rvc_lean_w3y', 'quake.ft.code'), @('nxray', 'rvc_lean_w3y', 'nxray.ft.code'))) {
        python $py "ab_$($l[0])" --load $l[0] --frames 7000
        python $py "ab_$($l[0])" --load $l[0] --tick $l[1] --code $l[2] --frames 7000
        Get-Content "logs\acct_ab_$($l[0]).uart" -Tail 2 | ForEach-Object { '   ' + "$_".Substring(0, [Math]::Min(150, "$_".Length)) }
    }
}
# one blob, both APIs: the tick's ms on D3D11 is in ladder_lean.log; here D3D12 at 8192 and at 2048
python experiments\rv_fuse\acct\pick_tick.py build/shadercache | Out-Null
$env:RVC_DXIL = '1'; $env:RVC_PS_CSO = (Resolve-Path logs\lean.cso).Path
foreach ($t in 8192, 2048) {
    $o = pwsh -NoProfile -File tools\gpu_queue.ps1 bin\rvc_trace12.exe --rvc experiments\rvc_lean --define NO_PAGING --no-doubles --payload C:\Users\micha\agents_isa\img --load-state C:\Users\micha\agents_isa\snap\shell\shell.snap --seconds 6 --ticks $t 2>&1 | ForEach-Object { "$_" }
    "D3D12 ticks $t : " + (($o | Select-Object -Last 3) -join ' | ')
}
$env:RVC_DXIL = ''; $env:RVC_PS_CSO = ''
foreach ($r in 1..2) {
    python experiments\rv_fuse\h2h\h2h.py rv nxray_free --image C:\Users\micha\agents_isa\img --out experiments\rv_fuse\acct\nxray.jsonl 2>&1 | Out-Null
    python experiments\rv_fuse\h2h\h2h.py gvm nxray_free --out experiments\rv_fuse\acct\nxray.jsonl 2>&1 | Out-Null
}
python -c "import json;[print(r['machine'],r['load'],r['figs'][-2:]) for r in map(json.loads,open(r'experiments\rv_fuse\acct\nxray.jsonl')) if r['load']=='nxray_free']"
