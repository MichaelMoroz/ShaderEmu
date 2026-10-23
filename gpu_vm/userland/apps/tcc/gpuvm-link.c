/* gpu_vm's relocations for TinyCC (gpu_vm/userland/apps/tcc/README.md): lld's (lld/ELF/Arch/GPUVM.cpp), each at a
   fixed word of a 16-byte bundle, PC-relative ones counted from the bundle. A program is linked as a library at 0
   (tcc -shared) whose absolute words become R_GVM_RELATIVE, which elf2flt makes the flat program's relocations;
   every symbol is the program's own, so there is no GOT and no PLT. */
#ifdef TARGET_DEFS_ONLY

#define EM_GVM 0x4756
#define EM_TCC_TARGET EM_GVM

#define R_GVM_NONE 0
#define R_GVM_32 1         /* a data word: S + A */
#define R_GVM_REL32 2      /* S + A - P */
#define R_GVM_IMM32 3      /* word 2, a long immediate: S + A */
#define R_GVM_PCIMM32 4    /* word 2: S + A - P + 8 (from the bundle) */
#define R_GVM_BR9 5        /* the control word's off9, in bundles */
#define R_GVM_J18 6        /* the control word's off18 (j, call) */
#define R_GVM_IMM12 7      /* an op's imm12: S + A */
#define R_GVM_TPREL32 8
#define R_GVM_RELATIVE 9
#define R_GVM_NUM 10

#define R_DATA_32  R_GVM_32
#define R_DATA_PTR R_GVM_32
#define R_JMP_SLOT R_GVM_NONE
#define R_GLOB_DAT R_GVM_32
#define R_COPY     R_GVM_NONE
#define R_RELATIVE R_GVM_RELATIVE

#define R_NUM      R_GVM_NUM

#define ELF_START_ADDR 0x00010000
#define ELF_PAGE_SIZE 0x1000

#define PCRELATIVE_DLLPLT 1
#define RELOCATE_DLLPLT 1

#else /* !TARGET_DEFS_ONLY */

#include "tcc.h"

ST_FUNC int code_reloc(int reloc_type)
{
    switch (reloc_type) {
    case R_GVM_BR9:
    case R_GVM_J18:
        return 1;
    case R_GVM_NONE:
    case R_GVM_32:
    case R_GVM_REL32:
    case R_GVM_IMM32:
    case R_GVM_PCIMM32:
    case R_GVM_IMM12:
    case R_GVM_TPREL32:
    case R_GVM_RELATIVE:
        return 0;
    }
    return -1;
}

/* No GOT or PLT: a program links every library it needs (a static link), so each symbol is resolved where it is */
ST_FUNC int gotplt_entry_type(int reloc_type)
{
    if ((unsigned)reloc_type < R_GVM_NUM)
        return NO_GOTPLT_ENTRY;
    return -1;
}

ST_FUNC unsigned create_plt_entry(TCCState *s1, unsigned got_offset, struct sym_attr *attr)
{
    tcc_error_noabort("gpu_vm: no PLT");
    return 0;
}

ST_FUNC void relocate_plt(TCCState *s1)
{
}

/* the TLS block's start for %tprel: musl's (gpu_vm/userland/musl: tp at the block, TLS_ABOVE_TP with no gap) */
static addr_t gvm_tls_start(TCCState *s1)
{
    int i;
    for (i = 1; i < s1->nb_sections; i++) {
        Section *s = s1->sections[i];
        if (s->sh_flags & SHF_TLS)
            return s->sh_addr;
    }
    return 0;
}

ST_FUNC void relocate(TCCState *s1, ElfW_Rel *rel, int type, unsigned char *ptr, addr_t addr, addr_t val)
{
    int sym_index = ELFW(R_SYM)(rel->r_info);
    ElfW(Sym) *sym = &((ElfW(Sym) *)symtab_section->data)[sym_index];
    int32_t off;

    /* a symbol no file defines: an error unless weak (then 0, as lld makes it) */
    if (sym_index && sym->st_shndx == SHN_UNDEF && ELFW(ST_BIND)(sym->st_info) != STB_WEAK
        && (s1->output_type & TCC_OUTPUT_DYN) && type != R_GVM_NONE)
        tcc_error_noabort("undefined symbol '%s'", (char *)symtab_section->link->data + sym->st_name);

    switch (type) {
    case R_GVM_NONE:
        return;
    case R_GVM_32:
    case R_GVM_IMM32:
        /* a data word adds to what is there (TinyCC's debug info keeps its addends there; clang's words are 0) */
        if (type == R_GVM_32)
            add32le(ptr, val);
        else
            write32le(ptr, val);
        /* an absolute word: the flat program's loader adds where it was loaded */
        if ((s1->output_type & TCC_OUTPUT_DYN) && !(sym_index && sym->st_shndx == SHN_ABS)
            && !(sym_index && sym->st_shndx == SHN_UNDEF)) {
            qrel->r_offset = rel->r_offset;
            qrel->r_info = ELFW(R_INFO)(0, R_GVM_RELATIVE);
            qrel->r_addend = (int)read32le(ptr);
            qrel++;
        }
        return;
    case R_GVM_REL32:
        add32le(ptr, val - addr);
        return;
    case R_GVM_PCIMM32:
        write32le(ptr, val - addr + 8);
        return;
    case R_GVM_IMM12:
        off = val;
        if (off < -2048 || off > 2047)
            tcc_error_noabort("R_GVM_IMM12 out of range (%d)", (int)off);
        write32le(ptr, (read32le(ptr) & 0x000fffff) | ((uint32_t)off & 0xfff) << 20);
        return;
    case R_GVM_BR9:
        off = val - addr + 12;
        if ((off & 15) || off / 16 < -256 || off / 16 > 255)
            tcc_error_noabort("R_GVM_BR9 out of range (val=%x, addr=%x)", (unsigned)val, (unsigned)addr);
        write32le(ptr, (read32le(ptr) & ~(0x1ffu << 17)) | ((uint32_t)(off >> 4) & 0x1ff) << 17);
        return;
    case R_GVM_J18:
        off = val - addr + 12;
        if ((off & 15) || off / 16 < -(1 << 17) || off / 16 >= (1 << 17))
            tcc_error_noabort("R_GVM_J18 out of range (val=%x, addr=%x)", (unsigned)val, (unsigned)addr);
        write32le(ptr, (read32le(ptr) & 0x3fffu) | ((uint32_t)(off >> 4) & 0x3ffff) << 14);
        return;
    case R_GVM_TPREL32:
        write32le(ptr, val - gvm_tls_start(s1));
        return;
    case R_GVM_RELATIVE:
        return;
    default:
        tcc_error_noabort("gpu_vm: relocation %d at %x", type, (unsigned)addr);
        return;
    }
}
#endif
