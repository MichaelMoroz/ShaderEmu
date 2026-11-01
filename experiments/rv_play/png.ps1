# Images in turn on the card by h2h's sum-free png load: png.ps1 -Images nommu,p5 -Rounds 3 [-Load png_ns]; "nommu" is the main checkout's.
param([string[]]$Images, [int]$Rounds = 3, [string]$Load = 'png_ns', [string]$Out = 'experiments\rv_play\card.jsonl', [string[]]$More = @())
$Images = $Images -split ','
$here = (Get-Location).Path
$main = (git worktree list --porcelain | Select-Object -First 1).Substring(9)
$show = "import sys,json;d=json.loads(sys.stdin.read());w=d['work'];print(d['tag'],w['wall_ms'],w['passes'],w['core0'],{k:(v['share'],v['a_pass']) for k,v in w['why'].items()},[p[-70:] for p in d['proof'][-1:]],d['oops'])"
for ($r = 0; $r -lt $Rounds; $r++) {
    foreach ($i in $Images) {
        $im = if ($i -eq 'nommu') { "$main\build\images\nommu" } else { "$here\build\images\$i" }
        python experiments/rv_vm/h2h.py rv $Load --tag $i --image $im --out $Out @More 2>&1 | Select-Object -Last 1 | python -c $show
    }
}
