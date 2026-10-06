"""Widens the kernel's memcpy hook (arch/riscv/lib/memcpy.S). pimaker's fork already sends
copies of exactly one page through the machine's parallel copy; this adds every large,
texel-aligned copy between linear-mapped addresses. Idempotent."""
import sys
path = sys.argv[1] + '/arch/riscv/lib/memcpy.S'
s = open(path).read()
if 'ShaderEmu' not in s:
    hook = '''	/*
	 * ShaderEmu: a large, texel-aligned copy within the linear map is one parallel
	 * operation in the machine's commit pass (CSRs 0x0b1-0x0b3: source, destination,
	 * length; writing 1 to 0x0b0 runs it). Other addresses are not contiguous in
	 * physical memory beyond a page and take the path below.
	 */
	li t0, 1024
	bltu a2, t0, 9f
	or t1, a0, a1
	or t1, t1, a2
	andi t1, t1, 15
	bnez t1, 9f
	li t0, PAGE_OFFSET
	bltu a0, t0, 9f
	bltu a1, t0, 9f
	csrw 0x0b1, a1
	csrw 0x0b2, a0
	csrw 0x0b3, a2
	li t0, 1
	csrw 0x0b0, t0
	ret
9:
'''
    s = s.replace('#include <asm/asm.h>\n', '#include <asm/asm.h>\n#include <asm/page.h>\n', 1)
    mark = 'WEAK(memcpy)\n'
    assert s.count(mark) == 1
    s = s.replace(mark, mark + hook)
    open(path, 'w').write(s)
    print('memcpy hook added')
