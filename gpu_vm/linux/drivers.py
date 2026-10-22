"""The RISC-V machine's drivers in gpu_vm's kernel (gpu_vm/platform.md, "The Linux drivers"):
linux/kernel/shaderemu_{gpu,sound,fb,input,net}.c copied into the tree given as argv[1] with their physical addresses
less 0x86000000 (the device window moved down) and the few RISC-V lines in them made gpu_vm's; their Kconfig and
Makefile entries (linux/kernel/install_drivers.py's); the no-MMU mmap at a device's own address
(linux/nommu/patch_drivers.py's); and the RISC-V kernel's generic hooks that apply here (the tick polls the input and
the network: tick_input_hook.py; a socket reader is not woken for room to write: unixwake_hook.py). Idempotent: the
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
     ['depends on FB', 'select FB_CFB_FILLRECT', 'select FB_CFB_COPYAREA', 'select FB_CFB_IMAGEBLIT']),
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


for source, directory, symbol, prompt, extra in DRIVERS:
    text = gvm(source, open(os.path.join(kernel, source)).read())
    if source == 'shaderemu_gpu.c':
        text = nommu_area(text, 'shaderemu_gpu', 'GPU_PHYS', 'shaderemu_gpu_fops')
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
    if symbol not in k:
        entry = 'config %s\n\tbool "%s"\n%s\n' % (symbol, prompt, ''.join('\t%s\n' % line for line in extra))
        body = k.rstrip()
        for closing in ('endmenu', 'endif'):
            if body.endswith(closing):
                at = body.rfind('\n' + closing)
                k = body[:at + 1] + entry + body[at + 1:] + '\n'
                break
        else:
            k = body + '\n\n' + entry
        open(kconfig, 'w').write(k)

for hook in ('tick_input_hook.py', 'unixwake_hook.py'):
    subprocess.run([sys.executable, os.path.join(kernel, hook), tree], check=True, stdout=subprocess.DEVNULL)
print('drivers.py: the machine\'s drivers in', tree)
