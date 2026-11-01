# The picture held late, with the swap not waiting and waiting: the same pixels, but for what real time shows. hold.ps1 IMAGE [-At 600]
param([string]$Image, [int]$At = 600)
$main = (git worktree list --porcelain | Select-Object -First 1).Substring(9)
$t = Join-Path $env:TEMP 'play_hold'
New-Item -ItemType Directory -Force $t | Out-Null
foreach ($how in 'early', 'wait') {
    $sw = if ($how -eq 'wait') { 'TDAWN_SWAP=wait ' } else { '' }
    $bmp = "$t\$how.bmp"
    Remove-Item $bmp -ErrorAction SilentlyContinue
    $cmd = "nano-X -p & sleep 2; ${sw}TDAWN_HOLD_LATE=$At TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL\n"
    pwsh -NoProfile -File tools\gpu_queue.ps1 "$main\bin\rvc_harness.exe" --cpu --warp --image $Image --gpu-capture $bmp --expect "/ # " --send $cmd --until "held late" --seconds 300 2>&1 | Select-String 'held|until|refused' | Select-Object -First 2 | % { $_.Line.Trim() }
}
python -c @"
import struct,sys
def rd(p):
    b=open(p,'rb').read();off,w,h,bits=struct.unpack_from('<I',b,10)[0],*struct.unpack_from('<ii',b,18),struct.unpack_from('<H',b,28)[0]
    return b[off:],w,abs(h),bits//8
a,w,h,s=rd(r'$t\early.bmp');c,w2,h2,s2=rd(r'$t\wait.bmp')
print(w,h,s,w2,h2)
r=(w*s+3)&~3
n=0;x0=y0=10**9;x1=y1=-1
for y in range(h):
    p,q=a[y*r:y*r+w*s],c[y*r:y*r+w*s]
    if p!=q:
        for x in range(w):
            if p[x*s:x*s+s]!=q[x*s:x*s+s]:
                n+=1;x0=min(x0,x);x1=max(x1,x);y0=min(y0,y);y1=max(y1,y)
print('pixels differ',n,'box',x0,y0,x1,y1)
"@
