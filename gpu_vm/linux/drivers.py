"""The RISC-V machine's drivers in gpu_vm's kernel (gpu_vm/platform.md, "The Linux drivers"):
linux/kernel/shaderemu_{gpu,sound,fb,input,net}.c copied into the tree given as argv[1] with their physical addresses
less 0x86000000 (the device window moved down) and the few RISC-V lines in them made gpu_vm's; their Kconfig and
Makefile entries (linux/kernel/install_drivers.py's); the no-MMU mmap at a device's own address
(linux/nommu/patch_drivers.py's); and the RISC-V kernel's generic hooks that apply here (the tick polls the input and
the network: tick_input_hook.py; a socket reader is not woken for room to write: unixwake_hook.py; romfs
looks a name up and reads a header or a name where they lie in the ROM, not through MTD: romfs_hook.py). Idempotent: the
drivers are copied anew each time. Run by build.sh's sync step.   python3 drivers.py KDIR"""
import os, re, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.normpath(os.path.join(here, '..', '..'))
kernel = os.path.join(repo, 'linux', 'kernel')
tree = sys.argv[1]

DRIVERS = [  # source, directory, config symbol, prompt, extra Kconfig lines
    ('shaderemu_gpu.c', 'drivers/misc', 'SHADEREMU_GPU', 'ShaderEmu GPU device', []),
    ('shaderemu_sound.c', 'drivers/misc', 'SHADEREMU_SOUND', 'ShaderEmu sound card', []),
    ('shaderemu_fb.c', 'drivers/video/fbdev', 'FB_SHADEREMU', 'ShaderEmu display',
     ['depends on FB', 'select FB_CFB_FILLRECT', 'select FB_CFB_COPYAREA', 'select FB_CFB_IMAGEBLIT',
      'select FB_PROVIDE_GET_FB_UNMAPPED_AREA if !MMU']),   # (no MMU: a mapping of fb0 is the pixels where they are)
    ('shaderemu_input.c', 'drivers/input/misc', 'INPUT_SHADEREMU', 'ShaderEmu keyboard and pointer', []),
    ('shaderemu_net.c', 'drivers/net', 'SHADEREMU_NET', 'ShaderEmu network device', ['depends on INET']),
]

HEADER = '''/*
 * gpu_vm (gpu_vm/linux/drivers.py, gpu_vm/platform.md): the RISC-V machine's driver, its addresses less 0x86000000
 * (the device window moved down) and its RISC-V lines made gpu_vm's. Do not edit here: linux/kernel/%s.
 */
#include <asm/gvm.h>
#define wait_for_interrupt() __asm__ __volatile__("wait" : : : "memory")	/* until an interrupt: the pass ends */
'''


def moved(m):
    return '0x%08x%s' % (int(m.group(1), 16) - 0x86000000, m.group(2))


def gvm(source, text):
    text = re.sub(r'0x(8[67][0-9a-fA-F]{6})(UL|u|U)\b', moved, text)
    for old, new in [
        ('csr_read(CSR_SATP) & 0x3fffff', '0'),                       # no MMU: a worker's root is 0
        ('csr_read(CSR_TIME)', 'gvm_mfsr(time_lo)'),                   # (one value a pass)
        ('__asm__ __volatile__(".word 0x0100000f");', 'gvm_sync();'),  # the pause hint: the pass ends
        ('\textern void zeroed_idle(void);\t/* mm/page_alloc.c, the zero pool */\n', ''),
        ('\tif (csr_read(CSR_TIME) == pass)\n\t\tzeroed_idle();\t/* (a block cleared ahead ends the pass too) */\n', ''),
        ('\tif (gvm_mfsr(time_lo) == pass)\n\t\tzeroed_idle();\t/* (a block cleared ahead ends the pass too) */\n', ''),
        # power off is the kernel's own (arch/gpuvm/kernel/reset.c: gvm_cpu's exit)
        ('\tpm_power_off = shaderemu_power_off;\n', '\t(void)shaderemu_power_off;\n'),
    ]:
        text = text.replace(old, new)
    # no page table: the mapping's bits as they are
    text = re.sub(r'__pgprot\(pgprot_val\(vma->vm_page_prot\) \| _PAGE_ACCESSED \|\s*\(\(vma->vm_flags & VM_WRITE\) \? _PAGE_DIRTY : 0\)\)',
                  'vma->vm_page_prot', text)
    # the devices lie below RAM: no page of theirs is RAM (linux/nommu/patch_drivers.py)
    text = re.sub(r'\bpfn_valid\(([A-Z_]+) >> PAGE_SHIFT\)', r'page_is_ram(\1 >> PAGE_SHIFT)', text)
    assert 'csr_' not in text and '.word' not in text and 'zeroed_idle' not in text, source
    return HEADER % source.replace('.c', '') + text


def nommu_area(text, fn, phys, fops):
    """linux/nommu/patch_drivers.py's patch(): mmap of the device at its physical address"""
    if 'nommu_area' in text:
        return text
    area = (f'\n#ifndef CONFIG_MMU\nstatic unsigned long {fn}_nommu_area(struct file *file, unsigned long addr, unsigned long len,\n'
            f'\t\tunsigned long pgoff, unsigned long flags)\n{{\n\treturn {phys} + (pgoff << PAGE_SHIFT);\n}}\n#endif\n')
    i = text.index(f'static const struct file_operations {fops}')
    text = text[:i] + area + '\n' + text[i:]
    text = text.replace(f'\t.mmap\t\t= {fn}_mmap,\n', f'\t.mmap\t\t= {fn}_mmap,\n#ifndef CONFIG_MMU\n\t.get_unmapped_area = {fn}_nommu_area,\n#endif\n', 1)
    assert 'get_unmapped_area' in text
    return text


REST_WORDS = '''
/* gpu_vm (gpu_vm/platform.md, "Resting"): the workers core 0 waits for while it rests, a bit a core (the tick gives
 * them their whole pass), and { REST_MARK, whether another task could run }: a program alone ends its pass at a rest
 * by itself (mcw_rest), with no system call */
#define REST_WAITING	0x1100
#define REST_DECLARED	0x1120	/* the pid of the program that says what it waits for (mcw_rest_for), 0 when it closes */
#define REST_CROWD	0x1130
#define REST_MARK	0x54534552
#define SHADEREMU_GPU_REST_MASK	_IOW('G', 12, __u32[8])
'''

REST_CROWD = '''/* Whether another task than the running one could run, said where a program reads it (from the timer's tick and
 * each rest; a store only when it changes). rest_alone=0 (/sys/module/shaderemu_gpu/parameters) takes the mark
 * away: every rest a system call, as before. */
static bool rest_alone = true;
module_param(rest_alone, bool, 0644);

void shaderemu_gpu_crowd(void)
{
	u32 crowded = nr_running() > 1, mark = rest_alone ? REST_MARK : 0;

	if (!workers)
		return;
	if (readl(workers + REST_CROWD + 4) != crowded)
		writel(crowded, workers + REST_CROWD + 4);
	if (readl(workers + REST_CROWD) != mark)
		writel(mark, workers + REST_CROWD);
}

'''

REST_MASK = '''	if (cmd == SHADEREMU_GPU_REST_MASK) {
		u32 mask[8];
		int i;

		if (copy_from_user(mask, (void __user *)arg, sizeof(mask)))
			return -EFAULT;
		for (i = 0; i < 8; i++)
			writel(mask[i], workers + REST_WAITING + 4 * i);
		shaderemu_gpu_rest();
		for (i = 0; i < 8; i++)
			writel(0, workers + REST_WAITING + 4 * i);
		return 0;
	}
'''


REST_RELEASE = '''		/* a program that said what it waits for no longer does */
		if (readl(workers + REST_DECLARED) == (u32)task_tgid_vnr(current))
			writel(0, workers + REST_DECLARED);
'''


def rest(text):
    """gpu_vm's rest: the words of the workers waited for, and whether a rest needs the kernel at all"""
    for old, new in [
        ('#define FIELD_JOB\t0x1900\n', '#define FIELD_JOB\t0x1900\n' + REST_WORDS),
        ('static noinline void shaderemu_gpu_rest(void)\n', REST_CROWD + 'static noinline void shaderemu_gpu_rest(void)\n'),
        ('\t\tyield();\n\tif (gvm_mfsr(time_lo) == pass)\n',
         '\t\tyield();\n\tshaderemu_gpu_crowd();\n\tif (gvm_mfsr(time_lo) == pass)\n'),
        ('\tif (cmd == SHADEREMU_GPU_REST) {\n', REST_MASK + '\tif (cmd == SHADEREMU_GPU_REST) {\n'),
        ("\t\t/* the field's job of a program that dies with it out",
         REST_RELEASE + "\t\t/* the field's job of a program that dies with it out"),
    ]:
        assert text.count(old) == 1, old
        text = text.replace(old, new)
    return text.replace('#include <asm/gvm.h>\n', '#include <asm/gvm.h>\n#include <linux/sched/stat.h>\n', 1)


for source, directory, symbol, prompt, extra in DRIVERS:
    text = gvm(source, open(os.path.join(kernel, source)).read())
    if source == 'shaderemu_gpu.c':
        text = rest(nommu_area(text, 'shaderemu_gpu', 'GPU_PHYS', 'shaderemu_gpu_fops'))
    if source == 'shaderemu_sound.c':
        text = nommu_area(text, 'shaderemu_sound', 'SND_REGS', 'shaderemu_sound_fops')
    out = os.path.join(tree, directory, source)
    if not os.path.exists(out) or open(out).read() != text:
        open(out, 'w').write(text)
    makefile = os.path.join(tree, directory, 'Makefile')
    if symbol not in open(makefile).read():
        open(makefile, 'a').write('obj-$(CONFIG_%s) += %s\n' % (symbol, source.replace('.c', '.o')))
    kconfig = os.path.join(tree, directory, 'Kconfig')
    k = open(kconfig).read()
    entry = 'config %s\n\tbool "%s"\n%s\n' % (symbol, prompt, ''.join('\t%s\n' % line for line in extra))
    old = re.search(r'config %s\n(\t.*\n)*\n' % symbol, k)
    if old and old.group(0) != entry:   # (an entry of an earlier drivers.py: made anew)
        k = k.replace(old.group(0), entry)
        open(kconfig, 'w').write(k)
    if symbol not in k:
        body = k.rstrip()
        for closing in ('endmenu', 'endif'):
            if body.endswith(closing):
                at = body.rfind('\n' + closing)
                k = body[:at + 1] + entry + body[at + 1:] + '\n'
                break
        else:
            k = body + '\n\n' + entry
        open(kconfig, 'w').write(k)

for hook in ('tick_input_hook.py', 'unixwake_hook.py', 'romfs_hook.py'):
    subprocess.run([sys.executable, os.path.join(kernel, hook), tree], check=True, stdout=subprocess.DEVNULL)

# The RISC-V machine's no-MMU kernel's generic patches (linux/nommu): overlayfs maps a ROM file where it lies, tmpfs
# renames and removes what comes from the ROM, /proc/PID/maps keeps no lock; and a mapping takes the first run of
# free pages long enough (patch_drivers.py's alloc_pages_run, from its source: FORCE_MAX_ZONEORDER is in gpuvm's
# Kconfig, the early allocations from the bottom in setup.c)
nommu = os.path.join(repo, 'linux', 'nommu')
src = open(os.path.join(nommu, 'patch_drivers.py')).read()
RUN = re.search(r"RUN = r'''(.*?)'''", src, re.S).group(1)
p = os.path.join(tree, 'mm/page_alloc.c')
s = open(p).read()
if 'what the run does not need goes back' not in s:
    open(p, 'w').write(s + RUN)
p = os.path.join(tree, 'mm/nommu.c')
s = open(p).read()
old = '\tbase = alloc_pages_exact(total << PAGE_SHIFT, GFP_KERNEL);\n'
if 'alloc_pages_run' not in s:
    assert s.count(old) == 1
    s = s.replace(old, '\tbase = total >= 16 ? alloc_pages_run(total) : NULL;\n\tif (!base)\n\t\tbase = alloc_pages_exact(total << PAGE_SHIFT, GFP_KERNEL);\n')
    s = s.replace('static int do_mmap_private(', 'void *alloc_pages_run(unsigned long nr);\n\nstatic int do_mmap_private(', 1)
    open(p, 'w').write(s)
for patch in ('patch_ovl.py', 'patch_ramfs.py', 'patch_procmaps.py'):
    subprocess.run([sys.executable, os.path.join(nommu, patch), tree], check=True, stdout=subprocess.DEVNULL)   # (after alloc_pages_run: patch_ovl.py changes its lines)
# binfmt_flat's copy of a program that lies in memory (patch_ovl.py): the RISC-V machine's copy CSRs there, gpu_vm's
# memcpy here (one operation of the commit from 4 KB up: arch/gpuvm/lib/string.S)
p = os.path.join(tree, 'fs/binfmt_flat.c')
s = open(p).read()
old = '\tcsr_write(0x0b1, from);\n\tcsr_write(0x0b2, to);\n\tcsr_write(0x0b3, size);\n\tcsr_write(0x0b0, 1);\n'
if old in s:
    open(p, 'w').write(s.replace(old, '\tmemcpy((void *)to, (const void *)from, size);\n'))
print('drivers.py: the machine\'s drivers in', tree)
