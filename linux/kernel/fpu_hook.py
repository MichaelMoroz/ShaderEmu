"""Makes the kernel's FPU support fit the machine's float instructions (docs/fpu.md). Idempotent.

Linux 5.17 saves a task's float registers as doubles and refuses a CPU with single precision
only: here they are saved as single words, and the feature is taken as present.
"""
import sys

root = sys.argv[1]

path = root + '/arch/riscv/kernel/fpu.S'
s = open(path).read()
if '\tfsd ' in s or '\tfld ' in s:
    assert s.count('\tfsd ') == 32 and s.count('\tfld ') == 32
    s = s.replace('\tfsd ', '\tfsw ').replace('\tfld ', '\tflw ')
    open(path, 'w').write(s)
    print('fpu.S: float registers saved as single words')

path = root + '/arch/riscv/kernel/cpufeature.c'
s = open(path).read()
old = '''	if ((elf_hwcap & COMPAT_HWCAP_ISA_F) && !(elf_hwcap & COMPAT_HWCAP_ISA_D)) {
		pr_info("This kernel does not support systems with F but not D\\n");
		elf_hwcap &= ~COMPAT_HWCAP_ISA_F;
	}
'''
if old in s:
    s = s.replace(old, '''	/* ShaderEmu: the machine has F without D, and fpu.S here saves single words */
	elf_hwcap |= COMPAT_HWCAP_ISA_F;
''')
    open(path, 'w').write(s)
    print('cpufeature.c: F without D is supported, and present')
else:
    assert 'ShaderEmu: the machine has F without D' in s
