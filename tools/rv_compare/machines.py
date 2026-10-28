"""The machines the comparison runs (tools/rv_compare/cmp.py): each a harness, the folder it runs in (its tick is
ROOT/experiments/rvc_opt), the image it boots and the shell snapshot every part starts from.

    root   the harness's working folder: experiments/rvc_opt (the tick) and build/shadercache are read from there
    exe    the harness, under root
    image  the image's folder (linux_payload.bin, rootfs.bin, dts.bin): --image DIR
    shell  the snapshot at the shell with `nano-X -p` up, made by `cmp.py shell MACHINE` on the processor
    args   more arguments for the harness (a define the machine needs, say)

A snapshot is bound to its image and to the RAM's layout: a machine whose image or memory map changed makes its own
shell snapshot (`cmp.py shell MACHINE`), never another's. RVCMP_BASE is the backport's checkout (the frozen "before"
lies in its build/before, docs: build/before/README.md); RVCMP_AFTER_ROOT moves "after" to another checkout."""
import os

BASE = os.environ.get('RVCMP_BASE', r'E:\Development\Emu\ShaderEmu-rvbp')
BEFORE = os.path.join(BASE, 'build', 'before')
AFTER = os.environ.get('RVCMP_AFTER_ROOT', BASE)

MACHINES = {
    # the RISC-V machine at riscv-backport's base, 7624f55 (frozen: build/before/README.md)
    'before': dict(root=BEFORE, exe=os.path.join('bin', 'rvc_harness.exe'),
                   image=os.path.join(BEFORE, 'build', 'images', 'nommu'),
                   shell=os.path.join(BEFORE, 'snap', 'shell.snap'), args=[]),
    # the backport's tree as built in its checkout: bin\rvc_harness.exe, build/images/nommu
    'after': dict(root=AFTER, exe=os.path.join('bin', 'rvc_harness.exe'),
                  image=os.environ.get('RVCMP_AFTER_IMAGE', os.path.join(AFTER, 'build', 'images', 'nommu')),
                  shell=os.path.join(AFTER, 'build', 'rv_compare', 'after_shell.snap'), args=[]),
}
