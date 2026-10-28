"""No MMU on RV32: the MMU not forced on, the kernel still in supervisor mode (the machine's SBI), the
CLINT timer only in machine mode, and the kernel's address where it is loaded.   python3 patch_kconfig.py K"""
import sys
p = sys.argv[1] + '/arch/riscv/Kconfig'
s = open(p).read()
changes = [
    ('\tselect CLINT_TIMER if !MMU\n', '\tselect CLINT_TIMER if RISCV_M_MODE\n'),
    ('config RISCV_M_MODE\n\tbool\n\tdefault !MMU\n', 'config RISCV_M_MODE\n\tbool\n\tdefault n\n'),
    ('config PAGE_OFFSET\n\thex\n\tdefault 0xC0000000 if 32BIT\n',
     'config PAGE_OFFSET\n\thex\n\tdefault 0x82000000 if 32BIT && !MMU\n\tdefault 0xC0000000 if 32BIT\n'),
    ('\tselect GENERIC_LIB_UCMPDI2\n\tselect MMU\n\nconfig ARCH_RV64I\n', '\tselect GENERIC_LIB_UCMPDI2\n\nconfig ARCH_RV64I\n'),
]
for old, new in changes:
    if new in s:
        continue
    assert s.count(old) == 1, old
    s = s.replace(old, new)
open(p, 'w').write(s)
print('Kconfig patched')
