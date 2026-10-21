"""The no-MMU kernel made for the machine's slots (ISA.md, "Linux"): what the kernel knows of instructions.
A system call returns past a slot of 16 bytes, the signal trampoline is two slots, a break is a slot, the
linker keeps the relocations (xlat_kernel.c reads them) and room for the kernel's slots before _end, and
binfmt_flat loads a program's slots (FLAT_FLAG_NATIVE).   python3 patch_native.py KERNEL_TREE"""
import os, re, sys

K = sys.argv[1]
HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE_ROOM = 0x00c00000   # the kernel's slots: xlat_kernel says how much of it they take


def g_index(name):
    """An op's number in isa.h's enum of general ops."""
    text = open(os.path.join(HERE, '..', 'isa.h')).read()
    body = text[text.index('G_ILLEGAL = 0'):text.index('G_COUNT')]
    names = re.findall(r'\b(G_[A-Z0-9]+)\b', re.sub(r'/\*.*?\*/', '', body, flags=re.S))
    return names.index(name)


def patch(rel, pairs):
    p = os.path.join(K, rel)
    s = open(p).read()
    for old, new in pairs:
        if new in s:
            continue
        assert s.count(old) >= 1, '%s: %r not found' % (rel, old)
        s = s.replace(old, new)
    open(p, 'w').write(s)


patch('arch/riscv/Makefile', [('LDFLAGS_vmlinux :=\n', 'LDFLAGS_vmlinux := --emit-relocs --no-relax\n')])
patch('arch/riscv/kernel/entry.S', [
    ('\taddi s2, s2, 0x4\n', '\taddi s2, s2, 0x10\t/* a slot */\n'),
    ('ENTRY(__user_rt_sigreturn)\n\tli a7, __NR_rt_sigreturn\n\tscall\nEND(__user_rt_sigreturn)\n',
     '\t.section .rodata\n\t.balign 16\n'
     '/* the machine\'s two slots: a7 = __NR_rt_sigreturn, ecall (experiments/isa_proto/isa.h) */\n'
     'ENTRY(__user_rt_sigreturn)\n'
     '\t.word 0x40000000, 0x8000, 17, __NR_rt_sigreturn\n'
     '\t.word 0x40000002, 0x8000, 0x%x, 0\n'
     'END(__user_rt_sigreturn)\n\t.text\n' % (32 | g_index('G_ECALL') << 16))])
patch('arch/riscv/kernel/signal.c', [
    ('extern u32 __user_rt_sigreturn[2];', 'extern u32 __user_rt_sigreturn[8];'),
    ('\tu32 sigreturn_code[2];', '\tu32 sigreturn_code[8] __aligned(16);'),
    ('regs->epc -= 0x4;', 'regs->epc -= 0x10;')])
patch('arch/riscv/kernel/traps.c', [
    ('\treturn GET_INSN_LENGTH(insn);\n}', '\treturn 16;\t/* a slot */\n}'),
    ('\tif ((insn & __INSN_LENGTH_MASK) == __INSN_LENGTH_32)\n\t\treturn (insn == __BUG_INSN_32);\n\telse\n'
     '\t\treturn ((insn & __COMPRESSED_INSN_MASK) == __BUG_INSN_16);\n',
     '\treturn 1;\t/* (a slot: the bug table says whether it is one) */\n')])
patch('arch/riscv/kernel/vmlinux.lds.S', [
    ('\t_end = .;', '\t. = ALIGN(16);\n\t__native_start = .;\t/* the kernel\'s slots (xlat_kernel.c) */\n'
     '\t. += 0x%08x;\n\t__native_end = .;\n\t_end = .;' % NATIVE_ROOM)])
patch('include/linux/flat.h', [
    ('#define FLAT_FLAG_KTRACE 0x0010 /* output useful kernel trace for debugging */',
     '#define FLAT_FLAG_KTRACE 0x0010 /* output useful kernel trace for debugging */\n'
     '#define FLAT_FLAG_NATIVE 0x0100 /* the machine\'s slots: filler[0] where, filler[1] how long */')])
patch('fs/binfmt_flat.c', [
    ('\tbss_len   = ntohl(hdr->bss_end) - ntohl(hdr->data_end);\n',
     '\tbss_len   = ntohl(hdr->bss_end) - ntohl(hdr->data_end);\n'
     '\tif (ntohl(hdr->flags) & FLAT_FLAG_NATIVE)\t/* the slots lie after the bss, as part of it */\n'
     '\t\tbss_len = ntohl(hdr->filler[0]) + ntohl(hdr->filler[1]) - ntohl(hdr->data_end);\n'),
    ('\tlibinfo->lib_list[id].entry = (0x00ffffff & ntohl(hdr->entry)) + textpos;',
     '\tlibinfo->lib_list[id].entry = ((flags & FLAT_FLAG_NATIVE ? 0x0fffffff : 0x00ffffff) & ntohl(hdr->entry)) + textpos;'),
    ('\tif (flags & FLAT_FLAG_RAM && !(flags & (FLAT_FLAG_GZIP | FLAT_FLAG_GZDATA)) && !id)\n'
     '\t\treturn clear_user((void __user *)(datapos + data_len),\n'
     '\t\t\t\t  max(textpos + copied, datapos + full_data) - (datapos + data_len)) ? -EFAULT : 0;\n',
     '\tif (flags & FLAT_FLAG_RAM && !(flags & (FLAT_FLAG_GZIP | FLAT_FLAG_GZDATA)) && !id) {\n'
     '\t\tif (clear_user((void __user *)(datapos + data_len),\n'
     '\t\t\t\t  max(textpos + copied, datapos + full_data) - (datapos + data_len)))\n'
     '\t\t\treturn -EFAULT;\n'
     '\t\tif (flags & FLAT_FLAG_NATIVE) {\t/* the slots follow the relocations in the file */\n'
     '\t\t\tresult = read_code(bprm->file, textpos + ntohl(hdr->filler[0]),\n'
     '\t\t\t\t\t   ntohl(hdr->reloc_start) + relocs * 4, ntohl(hdr->filler[1]));\n'
     '\t\t\tif (IS_ERR_VALUE(result))\n\t\t\t\treturn result;\n\t\t}\n'
     '\t\treturn 0;\n\t}\n')])
print('kernel patched for the machine\'s slots')
