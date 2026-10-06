"""Adds the ShaderEmu fill hook to the kernel's memset (arch/riscv/lib/memset.S). Idempotent."""
import sys
path = sys.argv[1] + '/arch/riscv/lib/memset.S'
s = open(path).read()
if 'ShaderEmu' not in s:
    hook = '''	/*
	 * ShaderEmu: a large, texel-aligned fill of linear-mapped memory is one parallel
	 * operation in the machine's commit pass. CSRs 0x0b1-0x0b3 take the fill word,
	 * destination and length; writing 2 to 0x0b0 runs it and ends the CPU's frame.
	 * Addresses below PAGE_OFFSET (early boot, vmalloc, ioremap) take the normal path:
	 * only the linear map is contiguous in physical memory.
	 */
	li t0, 1024
	bltu a2, t0, 9f
	or t1, a0, a2
	andi t1, t1, 15
	bnez t1, 9f
	li t0, PAGE_OFFSET
	bltu a0, t0, 9f
	andi t1, a1, 0xff
	slli t2, t1, 8
	or t1, t1, t2
	slli t2, t1, 16
	or t1, t1, t2
	csrw 0x0b1, t1
	csrw 0x0b2, a0
	csrw 0x0b3, a2
	li t0, 2
	csrw 0x0b0, t0
	ret
9:
'''
    s = s.replace('#include <asm/asm.h>\n', '#include <asm/asm.h>\n#include <asm/page.h>\n', 1)
    mark = 'WEAK(memset)\n'
    assert s.count(mark) == 1
    s = s.replace(mark, mark + hook)
    open(path, 'w').write(s)
    print('memset hook added')
