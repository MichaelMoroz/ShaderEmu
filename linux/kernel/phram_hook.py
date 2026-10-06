"""Makes the kernel's ROM driver (drivers/mtd/devices/phram.c) read through the machine's
parallel copy: the texel-aligned middle of a read into the linear map is one operation in the
commit pass, whose source may be the ROM. Every file read from the root file system comes
through here. Idempotent."""
import sys
path = sys.argv[1] + '/drivers/mtd/devices/phram.c'
s = open(path).read()
if 'ShaderEmu' not in s:
    old = '''	u_char *start = mtd->priv;

	memcpy(buf, start + from, len);
	*retlen = len;
'''
    new = '''	u_char *start = mtd->priv, *src = start + from;
	size_t head = -(unsigned long)buf & 15, body;

	/*
	 * ShaderEmu: CSRs 0x0b1-0x0b3 take source, destination and length, and writing 1 to
	 * 0x0b0 copies in the machine's commit pass. Both ends must be at the same place in a
	 * 16-byte texel and contiguous in physical memory: the linear map and this mapping are.
	 */
	if (len >= 1024 && (unsigned long)buf >= PAGE_OFFSET && !(((unsigned long)buf ^ (unsigned long)src) & 15)) {
		body = (len - head) & ~(size_t)15;
		memcpy(buf, src, head);
		csr_write(0x0b1, (unsigned long)src + head);
		csr_write(0x0b2, (unsigned long)buf + head);
		csr_write(0x0b3, body);
		csr_write(0x0b0, 1);
		memcpy(buf + head + body, src + head + body, len - head - body);
	} else {
		memcpy(buf, src, len);
	}
	*retlen = len;
'''
    assert s.count(old) == 1
    s = s.replace(old, new)
    s = s.replace('#include <linux/mtd/mtd.h>\n', '#include <linux/mtd/mtd.h>\n#include <asm/csr.h>\n#include <asm/page.h>\n', 1)
    open(path, 'w').write(s)
    print('phram hook added')
