#!/usr/bin/env python3
"""Core 0's loads and stores of a load on gvm_cpu, with no cache model, for sim.c. Runs in WSL; files in ~/stripes_trace.
    python trace.py gzip,png,mp3_1,jpeg,boot,tdawn"""
import os, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
GVM = '/mnt/d/Development/GPU-VM'
PRE = 'nano-X -p & sleep 2; '
TD = 'TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 '
A, B = 'echo H2H"" A; ', '; echo H2H"" B'
L = {'boot': None,
     'gzip': 'seq 1 20000 > /tmp/g; ' + A + 'gzip -c /tmp/g > /dev/null; gzip -c /tmp/g > /dev/null' + B,
     'jpeg': A + 'nxview --decode /usr/share/picture-fox.jpg /tmp/b.ppm' + B,
     'png': A + 'nxview --decode /usr/share/picture-fox.png /tmp/a.ppm' + B,
     'mp3_1': A + 'media_test audio /usr/share/play-test.mp3 /tmp/o.raw 0' + B,
     'tdawn': PRE + A + TD + 'TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL' + B}
def run(load):
    win = "--profile-from 0 --profile-until '@gpuvm:~# '" if load == 'boot' else "--profile-at 'H2H A' --profile-until 'H2H B'"
    send = '--expect ' + shlex.quote('@gpuvm:~# ') + ' --send ' + shlex.quote(((L[load] + '; ') if L[load] else '') + 'poweroff -f\n')
    c = (f'mkdir -p ~/stripes_trace; cd {GVM} && GVM_FAST=0 GVM_STLOG_LOADS=1 GVM_STLOG_MAX=150000000 GVM_STLOG=$HOME/stripes_trace/{load}.bin timeout 1700 build/gvm_cpu/gvm_cpu '
         f'--cores 256 --budget 8192 --ram 192 --rom build/gvm_apps/img/rootfs.romfs --dtb build/gvm_apps/img/gvm.dtb --stats {win} {send} build/gvm_apps/img/vmlinux </dev/null 2>&1 | grep -a "gvm_cpu: " | tail -12')
    w = subprocess.run(['wsl', 'bash', '-c', c], capture_output=True, text=True, errors='replace')
    print('==', load, w.stdout[-1200:], w.stderr[-200:], flush=True)
with ThreadPoolExecutor(6) as ex: list(ex.map(run, sys.argv[1].split(',')))
