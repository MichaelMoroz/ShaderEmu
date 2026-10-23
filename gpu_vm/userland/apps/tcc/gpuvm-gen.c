/* TinyCC's code generator for gpu_vm (gpu_vm/GPU_VM.md, gpu_vm/userland/apps/tcc/README.md): the simplest that is
   right, one op to a 16-byte bundle, every address TinyCC keeps (ind, a jump's place) a bundle's start. Integers and
   floats in the 64 registers (a float is native, single precision); a double is compiler-rt's calls. The calling
   convention is clang's for gpu_vm (GPUVMISelLowering.cpp's CC_GPUVM, clang's Targets/GPUVM.cpp). Made from
   riscv32-gen.c, whose generic parts (a long long as two words, a double as two) it keeps. */
#ifdef TARGET_DEFS_ONLY

/* TinyCC's registers: a0-a7 (r5-r12), t0-t7 (r13-r20); then ra and sp, which it never allocates */
#define NB_REGS 19
#define CONFIG_TCC_ASM

#define TREG_R(x) (x)       /* x = 0..7: a0-a7 */
#define TREG_T(x) (8 + (x)) /* x = 0..7: t0-t7 */

#define RC_INT (1 << 0)
#define RC_FLOAT (1 << 1)
#define RC_R(x) (1 << (2 + (x)))
#define RC_T(x) (1 << (10 + (x)))

#define RC_IRET (RC_R(0))
#define RC_IRE2 (RC_R(1))
#define RC_FRET (RC_R(0))   /* a float is returned in a0 */

#define REG_IRET (TREG_R(0))
#define REG_IRE2 (TREG_R(1))
#define REG_FRET (TREG_R(0))

#define PTR_SIZE 4

#define LDOUBLE_SIZE 8
#define LDOUBLE_ALIGN 8

#define MAX_ALIGN 16

#else
#define USING_GLOBALS
#include "tcc.h"
#include <assert.h>

ST_DATA const char * const target_machine_defs =
    "__gpuvm__\0"
    "__gpuvm\0"
    ;

#define XLEN 4

#define TREG_RA 17
#define TREG_SP 18

ST_DATA const int reg_classes[NB_REGS] = {
  RC_INT | RC_FLOAT | RC_R(0),
  RC_INT | RC_FLOAT | RC_R(1),
  RC_INT | RC_FLOAT | RC_R(2),
  RC_INT | RC_FLOAT | RC_R(3),
  RC_INT | RC_FLOAT | RC_R(4),
  RC_INT | RC_FLOAT | RC_R(5),
  RC_INT | RC_FLOAT | RC_R(6),
  RC_INT | RC_FLOAT | RC_R(7),
  RC_INT | RC_FLOAT | RC_T(0),
  RC_INT | RC_FLOAT | RC_T(1),
  RC_INT | RC_FLOAT | RC_T(2),
  RC_INT | RC_FLOAT | RC_T(3),
  RC_INT | RC_FLOAT | RC_T(4),
  RC_INT | RC_FLOAT | RC_T(5),
  RC_INT | RC_FLOAT | RC_T(6),
  RC_INT | RC_FLOAT | RC_T(7),
  0,
  1 << TREG_RA,
  1 << TREG_SP
};

#if defined(CONFIG_TCC_BCHECK)
ST_DATA int func_bound_add_epilog;
#endif

/* gpu_vm's registers (gpu_vm/isa.h; the ABI: GPUVMRegisterInfo.td) */
#define R_ZERO 0
#define R_RA 1
#define R_SP 2
#define R_FP 32   /* s0 */
#define R_T1 21   /* scratch registers TinyCC does not allocate (t8-t10) */
#define R_T2 22
#define R_CARRY 23

/* opcodes (gpu_vm/isa.h) */
#define OP_I(f) (0x40u + (f))
#define OP_II(f) (0x60u + (f))
#define OP_M(mode, w) (0x80u + (mode) * 8u + (w))
#define OP_F(f) (0xc0u + (f))
#define I_ADD 0
#define I_AND 1
#define I_OR 2
#define I_XOR 3
#define I_SUB 4
#define I_SLT 8
#define I_SLTU 9
#define I_SGE 12
#define I_SGEU 13
#define I_SLL 16
#define I_SRL 17
#define I_SRA 18
#define I_MUL 19
#define I_SEQ 22
#define I_SNE 23
#define I_ADDCY 26
#define I_CARRY 27
#define I_SUBBY 28
#define I_BORROW 29
#define M_LOAD 0
#define M_STORE 1
#define W_W 0
#define W_H 2
#define W_B 3
#define W_HU 6
#define W_BU 7
#define F_ADD 0
#define F_MUL 1
#define F_SUB 4
#define F_EQ 8
#define F_LT 9
#define F_LE 10
#define F_ITOF 16
#define F_UTOF 17
#define F_FTOI 18
#define F_FTOU 19
#define F_DIV 24
#define X_MULHU 0x02
#define X_DIV 0x04
#define X_DIVU 0x05
#define X_REM 0x06
#define X_REMU 0x07
#define X_LPC 0x0e

/* control words */
#define CC_SPECIAL 0
#define CC_BLTU 2
#define CC_BGEU 3
#define CC_BEQ 4
#define CC_BNE 5
#define CC_BLT 6
#define CC_BGE 7
#define CC_BLTUI 10
#define CC_BGEUI 11
#define CC_BEQI 12
#define CC_BNEI 13
#define CC_BLTI 14
#define CC_BGEI 15
#define SUB_JR 2
#define SUB_CALLR 3
#define SUB_J 5
#define SUB_CALL 6
#define CTRL_NONE (CC_BNE << 7)
#define PATH_GENERAL 0
#define PATH_ALU 1
#define PATH_LOAD 3
#define PATH_STORE 5
#define PATH_INT 7

#define WORD_R(opc, rd, ra, rb, rc) ((uint32_t)(opc) << 12 | ((rd) & 63u) << 6 | ((ra) & 63u) | ((rc) & 63u) << 20 | ((rb) & 63u) << 26)
#define WORD_I(opc, rd, ra, imm) ((uint32_t)(opc) << 12 | ((rd) & 63u) << 6 | ((ra) & 63u) | ((uint32_t)(imm) & 0xfffu) << 20)

/* a branch's or a jump's cmp_r: the compared registers, or a register and imm6 (CMP_IMM) */
#define CMP_IMM 0x8000

static int ireg(int r)
{
    if (r == TREG_RA)
        return R_RA;
    if (r == TREG_SP)
        return R_SP;
    assert(r >= 0 && r < 16);
    return r + 5;
}

static int is_ireg(int r)
{
    return (unsigned)r < 16 || r == TREG_RA || r == TREG_SP;
}

/* The path of a bundle of one op (GPUVMBaseInfo.h's bundle::path, with the tick's fx1 and late branches off) */
static unsigned op_path(uint32_t w)
{
    unsigned opc = (w >> 12) & 255, cls = opc >> 6, f = opc & 31, mode, wd;
    if (!w)
        return PATH_INT;
    if (cls == 1) {
        if (f <= 19)
            return PATH_INT;
        if (f <= 23 || opc == OP_I(25))
            return PATH_ALU;
        return PATH_GENERAL;
    }
    if (cls == 3)
        return (opc & 0x20) && (f == 2 || f == 3 || f == 5) ? PATH_GENERAL : PATH_ALU;
    if (cls == 2) {
        mode = (opc >> 3) & 7, wd = opc & 7;
        if (mode <= 3 && (wd == W_W || wd == W_H || wd == W_B || wd == W_HU || wd == W_BU))
            return (mode & 1) ? PATH_STORE : PATH_LOAD;
        return PATH_GENERAL;
    }
    return opc == X_LPC ? PATH_ALU : PATH_GENERAL;
}

ST_FUNC void o(unsigned int c)
{
    int ind1 = ind + 4;
    if (nocode_wanted)
        return;
    if (ind1 > cur_text_section->data_allocated)
        section_realloc(cur_text_section, ind1);
    write32le(cur_text_section->data + ind, c);
    ind = ind1;
}

/* a bundle: op 0 (none if 0), word 2, the control word without its path and n */
ST_FUNC void gvm_bundle(uint32_t w0, uint32_t w2, int long0, uint32_t ctl)
{
    unsigned path, cc = (ctl >> 7) & 15;
    if (nocode_wanted)
        return;
    path = op_path(w0);
    if (cc == CC_SPECIAL)   /* path12: jr, callr, call, j */
        path = path == PATH_INT || path == PATH_ALU ? PATH_ALU : path == PATH_LOAD || path == PATH_STORE ? 2 : PATH_GENERAL;
    o(w0);
    o(0);
    o(w2);
    o(ctl | path | (w0 ? 1 << 3 : 0) | (long0 ? 1 << 5 : 0));
}

static int fits12(int32_t v)
{
    return v >= -2048 && v <= 2047;
}

static void emit_r(unsigned opc, int rd, int ra, int rb, int rc)
{
    gvm_bundle(WORD_R(opc, rd, ra, rb, rc), 0, 0, CTRL_NONE);
}

/* an I form: imm12 when it fits, else the long immediate */
static void emit_i(unsigned opc, int rd, int ra, int32_t imm)
{
    if (fits12(imm))
        gvm_bundle(WORD_I(opc, rd, ra, imm), 0, 0, CTRL_NONE);
    else
        gvm_bundle(WORD_I(opc, rd, ra, 0), imm, 1, CTRL_NONE);
}

static void emit_mv(int rd, int ra)
{
    if (rd != ra)
        emit_i(OP_II(I_ADD), rd, ra, 0);
}

static void emit_li(int rd, int32_t v)
{
    emit_i(OP_II(I_ADD), rd, R_ZERO, v);
}

/* rd = the symbol's address + addend (lpc, its immediate from the bundle) */
static void emit_lpc_sym(int rd, Sym *sym, int addend)
{
    greloca(cur_text_section, sym, ind + 8, R_GVM_PCIMM32, addend);
    gvm_bundle(WORD_I(X_LPC, rd, 0, 0), 0, 1, CTRL_NONE);
}

static void emit_mem(int mode, int w, int rd, int base, int32_t ofs)
{
    emit_i(OP_M(mode, w), rd, base, ofs);
}

/* a jump chain's next link is kept in word 2 of the jump's bundle (a j has no op) */
ST_FUNC void gsym_addr(int t_, int a_)
{
    uint32_t t = t_, a = a_;
    while (t) {
        unsigned char *ptr = cur_text_section->data + t;
        uint32_t next = read32le(ptr + 8);
        int32_t r = (int32_t)(a - t) >> 4;
        if (r < -(1 << 17) || r >= (1 << 17))
            tcc_error("out-of-range branch chain");
        write32le(ptr + 8, 0);
        write32le(ptr + 12, (read32le(ptr + 12) & 0x3fff) | ((uint32_t)r & 0x3ffff) << 14);
        t = next;
    }
}

/* the base register and offset of an lvalue's address; may use 'tmp' for it */
static int lval_base(SValue *sv, int tmp, int *ofs)
{
    int v = sv->r & VT_VALMASK;
    *ofs = sv->c.i;
    if (sv->r & VT_SYM) {
        assert(v == VT_CONST);
        emit_lpc_sym(tmp, sv->sym, sv->c.i);
        *ofs = 0;
        return tmp;
    }
    if (v == VT_LOCAL)
        return R_FP;
    if (v == VT_LLOCAL) {
        emit_mem(M_LOAD, W_W, tmp, R_FP, sv->c.i);
        *ofs = 0;
        return tmp;
    }
    if (v < VT_CONST) {
        *ofs = 0;   /* (as riscv32-gen.c: a register lvalue's offset is not kept) */
        return ireg(v);
    }
    if (v == VT_CONST)
        return R_ZERO;
    tcc_error("gpu_vm: an lvalue of kind %x", v);
    return 0;
}

static int mem_width(SValue *sv, int load)
{
    int align, size = type_size(&sv->type, &align);
    int bt = sv->type.t & VT_BTYPE;
    if (bt == VT_PTR || bt == VT_FUNC || size > 4)
        size = 4;
    if (size == 1)
        return load && (sv->type.t & VT_UNSIGNED) ? W_BU : W_B;
    if (size == 2)
        return load && (sv->type.t & VT_UNSIGNED) ? W_HU : W_H;
    return W_W;
}

/* a comparison into a register: 1 or 0 */
static void setcc(int rd, int op, int cmp_r)
{
    int a = cmp_r & 63, b = (cmp_r >> 8) & 63, f = 0, swap = 0;
    if (cmp_r & CMP_IMM) {
        int imm = ((b ^ 32) - 32);
        switch (op) {
        case TOK_EQ: f = I_SEQ; break;
        case TOK_NE: f = I_SNE; break;
        case TOK_LT: f = I_SLT; break;
        case TOK_GE: f = I_SGE; break;
        case TOK_ULT: f = I_SLTU; break;
        case TOK_UGE: f = I_SGEU; break;
        default: tcc_error("gpu_vm: setcc %d", op);
        }
        emit_i(OP_II(f), rd, a, imm);
        return;
    }
    switch (op) {
    case TOK_EQ: f = I_SEQ; break;
    case TOK_NE: f = I_SNE; break;
    case TOK_LT: f = I_SLT; break;
    case TOK_GE: f = I_SGE; break;
    case TOK_ULT: f = I_SLTU; break;
    case TOK_UGE: f = I_SGEU; break;
    case TOK_LE: f = I_SGE; swap = 1; break;
    case TOK_GT: f = I_SLT; swap = 1; break;
    case TOK_ULE: f = I_SGEU; swap = 1; break;
    case TOK_UGT: f = I_SLTU; swap = 1; break;
    default: tcc_error("gpu_vm: setcc %d", op);
    }
    if (swap)
        emit_r(OP_I(f), rd, b, a, 0);
    else
        emit_r(OP_I(f), rd, a, b, 0);
}

ST_FUNC void load(int r, SValue *sv)
{
    int fr = sv->r;
    int v = fr & VT_VALMASK;
    int rr = ireg(r);
    int fc = sv->c.i;
    if (fr & VT_LVAL) {
        int ofs, base = lval_base(sv, rr, &ofs);
        emit_mem(M_LOAD, mem_width(sv, 1), rr, base, ofs);
    } else if (v == VT_CONST) {
        if (fr & VT_SYM)
            emit_lpc_sym(rr, sv->sym, fc);
        else
            emit_li(rr, fc);
    } else if (v == VT_LOCAL) {
        emit_i(OP_II(I_ADD), rr, R_FP, fc);
    } else if (v < VT_CONST) {
        assert(is_ireg(v));
        emit_mv(rr, ireg(v));
    } else if (v == VT_CMP) {
        setcc(rr, vtop->cmp_op, vtop->cmp_r);
    } else if ((v & ~1) == VT_JMP) {
        int t = v & 1;
        emit_li(rr, t);
        gjmp_addr(ind + 32);
        gsym(fc);
        emit_li(rr, t ^ 1);
    } else
        tcc_error("gpu_vm: load of kind %x", v);
}

ST_FUNC void store(int r, SValue *sv)
{
    int rr = ireg(r), ofs, base;
    if ((sv->type.t & VT_BTYPE) == VT_STRUCT)
        tcc_error("gpu_vm: store of a struct");
    assert(sv->r & VT_LVAL);
    base = lval_base(sv, R_T1, &ofs);
    emit_mem(M_STORE, mem_width(sv, 0), rr, base, ofs);
}

/* call (docall) or jump to vtop */
static void gcall_or_jmp(int docall)
{
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && (vtop->r & VT_SYM) && vtop->c.i == (int)vtop->c.i) {
        greloca(cur_text_section, vtop->sym, ind + 12, R_GVM_J18, (int)vtop->c.i);
        gvm_bundle(0, 0, 0, (docall ? SUB_CALL : SUB_J) << 11);
    } else {
        int r;
        if (vtop->r < VT_CONST)
            r = ireg(vtop->r);
        else {
            load(TREG_RA, vtop);
            r = R_RA;
        }
        gvm_bundle(0, 0, 0, (docall ? SUB_CALLR : SUB_JR) << 11 | r << 14);
    }
}

/* An argument's words: a struct of up to 8 bytes as one or two words (i32, i64), a larger one by a pointer to a
   copy; each word takes the next free a-register, a value of two all in registers or all on the stack (without
   taking a register), a variadic argument always on the stack, 4-byte slots */
static int arg_words(CType *type, int *byref)
{
    int align, size = type_size(type, &align);
    *byref = 0;
    if ((type->t & VT_BTYPE) == VT_STRUCT) {
        if (size > 8) {
            *byref = 1;
            return 1;
        }
        return (size + 3) >> 2;
    }
    return size > 4 ? 2 : 1;
}

ST_FUNC void gfunc_call(int nb_args)
{
    int i, align, size, areg = 0, nwords, byref;
    int *info = tcc_malloc((nb_args + 1) * sizeof(int));
    int *temp = tcc_malloc((nb_args + 1) * sizeof(int));
    int stack_adj = 0, tempspace = 0, stack_add;
    Sym *ft = vtop[-nb_args].type.ref, *sa = ft->next;
    int old = ft->f.func_type == FUNC_OLD;
    int hidden_sret = 0;
    CType *rt = &ft->type;
    SValue *sv;

    /* a struct returned by reference: its buffer is a first argument that the prototype does not list */
    if ((rt->t & VT_BTYPE) == VT_STRUCT && type_size(rt, &align) > 2 * XLEN)
        hidden_sret = 1;
    /* info[i]: the first register (0-7) | the words << 4 | 256 on the stack (the slot's offset << 9) */
    for (i = 0; i < nb_args; i++) {
        int named = old || (hidden_sret && i == 0) || sa != NULL;
        sv = &vtop[1 + i - nb_args];
        sv->type.t &= ~VT_ARRAY;
        nwords = arg_words(&sv->type, &byref);
        size = type_size(&sv->type, &align);
        temp[i] = -1;
        if ((sv->type.t & VT_BTYPE) == VT_STRUCT && size > 0) {
            /* a struct is copied first: by reference to its copy, or its words read from it */
            if (align < XLEN)
                align = XLEN;
            tempspace = (tempspace + align - 1) & -align;
            temp[i] = tempspace;
            tempspace += (size + 3) & -4;
        }
        if (nwords == 0)
            info[i] = 0;
        else if (named && areg + nwords <= 8) {
            info[i] = areg | nwords << 4;
            areg += nwords;
        } else {
            info[i] = 256 | nwords << 4 | stack_adj << 9;
            stack_adj += 4 * nwords;
        }
        if (sa && !(hidden_sret && i == 0))
            sa = sa->next;
    }
    stack_adj = (stack_adj + 15) & -16;
    tempspace = (tempspace + 15) & -16;
    stack_add = stack_adj + tempspace;

    if (stack_add)
        emit_i(OP_II(I_ADD), R_SP, R_SP, -stack_add);
    /* structs to their copies, words for the stack to their slots */
    for (i = 0; i < nb_args; i++) {
        if (temp[i] < 0 && !(info[i] & 256))
            continue;
        vrotb(nb_args - i);
        if (temp[i] >= 0) {
            int words = (info[i] >> 4) & 15;
            CType ct = vtop->type;
            arg_words(&ct, &byref);
            /* the copy: sp + stack_adj + temp */
            vset(&char_pointer_type, TREG_SP, 0);
            vpushi(stack_adj + temp[i]);
            gen_op('+');
            vpushv(vtop);              /* the copy's address replaces the struct */
            vrott(3);
            indir();
            vtop->type = ct;
            vswap();
            vstore();
            vpop();
            /* now vtop is the copy's address */
            if (!byref) {
                /* its words: an int or a long long at the copy */
                CType wt;
                wt.t = words == 2 ? VT_LLONG : VT_INT;
                wt.ref = NULL;
                mk_pointer(&wt);
                vtop->type = wt;
                indir();
            }
        }
        if (info[i] & 256) {
            int ofs = info[i] >> 9;
            CType ct = vtop->type;
            vset(&char_pointer_type, TREG_SP, 0);
            vpushi(ofs);
            gen_op('+');
            indir();
            vtop->type = ct;
            vswap();
            vstore();
            vtop->r = vtop->r2 = VT_CONST;   /* this argument is done */
        }
        vrott(nb_args - i);
    }
    /* the registers, the last argument first */
    for (i = nb_args - 1; i >= 0; i--) {
        int r, words;
        if (info[i] & 256 || !info[i])
            continue;
        r = info[i] & 15;
        words = (info[i] >> 4) & 15;
        vrotb(nb_args - i);
        if (words == 2 && (vtop->type.t & VT_BTYPE) != VT_LLONG && (vtop->type.t & VT_BTYPE) != VT_DOUBLE
            && (vtop->type.t & VT_BTYPE) != VT_LDOUBLE)
            vtop->type.t = VT_LLONG;
        gv(RC_R(r));
        if (words == 2 && vtop->r2 != TREG_R(r + 1)) {
            save_reg_upstack(TREG_R(r + 1), 1);
            emit_mv(ireg(TREG_R(r + 1)), ireg(vtop->r2));
            vtop->r2 = TREG_R(r + 1);
        }
        vrott(nb_args - i);
    }
    vrotb(nb_args + 1);
    save_regs(nb_args + 1);
    gcall_or_jmp(1);
    vtop -= nb_args + 1;
    if (stack_add)
        emit_i(OP_II(I_ADD), R_SP, R_SP, stack_add);
    tcc_free(temp);
    tcc_free(info);
}

static int func_sub_sp_offset, func_va_list_ofs;

ST_FUNC void gfunc_prolog(Sym *func_sym)
{
    CType *func_type = &func_sym->type;
    int i, addr = 0, align, size, areg = 0, nwords, byref, param_addr;
    Sym *sym;
    CType *type;

    if (cur_text_section->sh_addralign < 16)
        cur_text_section->sh_addralign = 16;
    if (ind & 15)
        gen_fill_nops(16 - (ind & 15));
    sym = func_type->ref;
    loc = -8;   /* ra at s0 - 4, the caller's s0 at s0 - 8 */
    func_sub_sp_offset = ind;
    ind += 4 * 16;

    /* a struct returned by reference: its buffer's address in a0 */
    size = type_size(&func_vt, &align);
    if ((func_vt.t & VT_BTYPE) == VT_STRUCT && size > 2 * XLEN) {
        loc -= XLEN;
        func_vc = loc;
        emit_mem(M_STORE, W_W, 5 + areg++, R_FP, loc);
    }
    while ((sym = sym->next) != NULL) {
        type = &sym->type;
        nwords = arg_words(type, &byref);
        if (byref)
            type = &char_pointer_type;
        size = nwords * 4;
        if (areg + nwords <= 8) {
            loc -= size;
            loc &= -4;
            param_addr = loc;
            for (i = 0; i < nwords; i++)
                emit_mem(M_STORE, W_W, 5 + areg++, R_FP, loc + 4 * i);
        } else {
            param_addr = addr;
            addr += size;
        }
        gfunc_set_param(sym, param_addr, byref);
    }
    /* the variadic arguments follow the named ones on the stack */
    func_va_list_ofs = addr;
#ifdef CONFIG_TCC_BCHECK
    if (tcc_state->do_bounds_check)
        tcc_error("gpu_vm: no bounds checking");
#endif
}

ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret, int *ret_align, int *regsize)
{
    int align, size = type_size(vt, &align);
    *ret_align = 4;
    *regsize = XLEN;
    if (size > 2 * XLEN)
        return 0;
    ret->t = VT_INT;
    ret->ref = NULL;
    return (size + 3) >> 2;
}

ST_FUNC void arch_transfer_ret_regs(int aftercall)
{
    tcc_error("gpu_vm: arch_transfer_ret_regs");
}

ST_FUNC void gfunc_epilog(void)
{
    int d, saved_ind;

    emit_mv(R_SP, R_FP);
    emit_mem(M_LOAD, W_W, R_RA, R_SP, -4);
    emit_mem(M_LOAD, W_W, R_FP, R_SP, -8);
    gvm_bundle(0, 0, 0, SUB_JR << 11 | R_RA << 14);

    d = (-loc + 15) & -16;
    saved_ind = ind;
    ind = func_sub_sp_offset;
    emit_i(OP_II(I_ADD), R_SP, R_SP, -d);
    emit_mem(M_STORE, W_W, R_RA, R_SP, d - 4);
    emit_mem(M_STORE, W_W, R_FP, R_SP, d - 8);
    emit_i(OP_II(I_ADD), R_FP, R_SP, d);
    ind = saved_ind;
}

ST_FUNC void gen_va_start(void)
{
    vtop--;
    vset(&char_pointer_type, VT_LOCAL, func_va_list_ofs);
}

ST_FUNC void gen_fill_nops(int bytes)
{
    if (bytes & 15)
        tcc_error("gpu_vm: code aligned to less than a bundle (16 bytes)");
    while (bytes > 0) {
        gvm_bundle(0, 0, 0, CTRL_NONE);
        bytes -= 16;
    }
}

/* a jump forward (to be patched): the far j, its chain's next in word 2 */
ST_FUNC int gjmp(int t)
{
    int a;
    if (nocode_wanted)
        return t;
    a = ind;
    gvm_bundle(0, t, 0, SUB_J << 11);
    return a;
}

ST_FUNC void gjmp_addr(int a)
{
    int32_t r = (a - ind) >> 4;
    gvm_bundle(0, 0, 0, SUB_J << 11 | ((uint32_t)r & 0x3ffff) << 14);
}

static uint32_t branch_ctl(int op, int cmp_r, int off)
{
    int a = cmp_r & 63, b = (cmp_r >> 8) & 63, cc = 0, t;
    if (cmp_r & CMP_IMM) {
        switch (op) {
        case TOK_EQ: cc = CC_BEQI; break;
        case TOK_NE: cc = CC_BNEI; break;
        case TOK_LT: cc = CC_BLTI; break;
        case TOK_GE: cc = CC_BGEI; break;
        case TOK_ULT: cc = CC_BLTUI; break;
        case TOK_UGE: cc = CC_BGEUI; break;
        default: tcc_error("gpu_vm: branch %d", op);
        }
    } else {
        switch (op) {
        case TOK_EQ: cc = CC_BEQ; break;
        case TOK_NE: cc = CC_BNE; break;
        case TOK_LT: cc = CC_BLT; break;
        case TOK_GE: cc = CC_BGE; break;
        case TOK_ULT: cc = CC_BLTU; break;
        case TOK_UGE: cc = CC_BGEU; break;
        case TOK_LE: cc = CC_BGE; t = a; a = b; b = t; break;
        case TOK_GT: cc = CC_BLT; t = a; a = b; b = t; break;
        case TOK_ULE: cc = CC_BGEU; t = a; a = b; b = t; break;
        case TOK_UGT: cc = CC_BLTU; t = a; a = b; b = t; break;
        default: tcc_error("gpu_vm: branch %d", op);
        }
    }
    return cc << 7 | a << 11 | ((uint32_t)off & 0x1ff) << 17 | (uint32_t)b << 26;
}

/* a branch when the comparison is false over a jump to t when it is true */
ST_FUNC int gjmp_cond(int op, int t)
{
    if (nocode_wanted)
        return t;
    gvm_bundle(0, 0, 0, branch_ctl(op ^ 1, vtop->cmp_r, 2));
    return gjmp(t);
}

ST_FUNC int gjmp_append(int n, int t)
{
    if (n) {
        uint32_t n1 = n, n2;
        unsigned char *p;
        while ((n2 = read32le(p = cur_text_section->data + n1 + 8)))
            n1 = n2;
        write32le(p, t);
        t = n;
    }
    return t;
}

/* a comparison of vtop[-1] with the constant fc, as a branch's register and imm6 when it fits */
static int cmp_imm(int op, int a, int fc, int *pop)
{
    switch (op) {
    case TOK_LE: if (fc == 31) return 0; fc++; op = TOK_LT; break;
    case TOK_GT: if (fc == 31) return 0; fc++; op = TOK_GE; break;
    case TOK_ULE: if (fc == -1 || fc == 31) return 0; fc++; op = TOK_ULT; break;
    case TOK_UGT: if (fc == -1 || fc == 31) return 0; fc++; op = TOK_UGE; break;
    }
    if (fc < -32 || fc > 31)
        return 0;
    *pop = op;
    return a | (fc & 63) << 8 | CMP_IMM;
}

static void gen_opil(int op)
{
    int a, b, d, f = -1;
    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
        int fc = vtop->c.i;
        switch (op) {
        case '+': f = I_ADD; break;
        case '-': f = I_SUB; break;
        case '&': f = I_AND; break;
        case '|': f = I_OR; break;
        case '^': f = I_XOR; break;
        case '*': f = I_MUL; break;
        case TOK_SHL: f = I_SLL; fc &= 31; break;
        case TOK_SHR: f = I_SRL; fc &= 31; break;
        case TOK_SAR: f = I_SRA; fc &= 31; break;
        }
        if (f >= 0) {
            vswap();
            gv(RC_INT);
            a = ireg(vtop->r);
            --vtop;
            d = get_reg(RC_INT);
            ++vtop;
            vswap();
            emit_i(OP_II(f), ireg(d), a, fc);
            --vtop;
            vtop->r = d;
            return;
        }
        if (op >= TOK_ULT && op <= TOK_GT) {
            int cop = op, cr;
            vswap();
            gv(RC_INT);
            a = ireg(vtop->r);
            vswap();
            cr = cmp_imm(op, a, fc, &cop);
            if (cr) {
                --vtop;
                vset_VT_CMP(cop);
                vtop->cmp_r = cr;
                return;
            }
        }
    }
    gv2(RC_INT, RC_INT);
    a = ireg(vtop[-1].r);
    b = ireg(vtop[0].r);
    vtop -= 2;
    d = get_reg(RC_INT);
    vtop++;
    vtop[0].r = d;
    d = ireg(d);
    switch (op) {
    default:
        if (op >= TOK_ULT && op <= TOK_GT) {
            vset_VT_CMP(op);
            vtop->cmp_r = a | b << 8;
            break;
        }
        tcc_error("gpu_vm: %s(%s)", __FUNCTION__, get_tok_str(op, NULL));
        break;
    case '+': emit_r(OP_I(I_ADD), d, a, b, 0); break;
    case '-': emit_r(OP_I(I_SUB), d, a, b, 0); break;
    case TOK_SAR: emit_r(OP_I(I_SRA), d, a, b, 0); break;
    case TOK_SHR: emit_r(OP_I(I_SRL), d, a, b, 0); break;
    case TOK_SHL: emit_r(OP_I(I_SLL), d, a, b, 0); break;
    case '*': emit_r(OP_I(I_MUL), d, a, b, 0); break;
    case '&': emit_r(OP_I(I_AND), d, a, b, 0); break;
    case '^': emit_r(OP_I(I_XOR), d, a, b, 0); break;
    case '|': emit_r(OP_I(I_OR), d, a, b, 0); break;
    case '/':
    case TOK_PDIV: emit_r(X_DIV, d, a, b, 0); break;
    case '%': emit_r(X_REM, d, a, b, 0); break;
    case TOK_UDIV: emit_r(X_DIVU, d, a, b, 0); break;
    case TOK_UMOD: emit_r(X_REMU, d, a, b, 0); break;
    /* a long long's words (tccgen.c's gen_opl): the carry or borrow kept in R_CARRY between them */
    case TOK_ADDC1:
        emit_r(OP_I(I_CARRY), R_CARRY, a, b, R_ZERO);
        emit_r(OP_I(I_ADD), d, a, b, 0);
        break;
    case TOK_ADDC2:
        emit_r(OP_I(I_ADDCY), d, a, b, R_CARRY);
        break;
    case TOK_SUBC1:
        emit_r(OP_I(I_BORROW), R_CARRY, a, b, R_ZERO);
        emit_r(OP_I(I_SUB), d, a, b, 0);
        break;
    case TOK_SUBC2:
        emit_r(OP_I(I_SUBBY), d, a, b, R_CARRY);
        break;
    }
}

ST_FUNC void gen_opi(int op)
{
    if (op == TOK_UMULL) {
        int a, b, dl, dh;
        gv2(RC_INT, RC_INT);
        a = ireg(vtop[-1].r);
        b = ireg(vtop[0].r);
        vtop--;
        dl = get_reg(RC_INT);
        vtop->r = dl;
        dh = get_reg(RC_INT);
        emit_r(X_MULHU, R_T1, a, b, 0);
        emit_r(OP_I(I_MUL), ireg(dl), a, b, 0);
        emit_mv(ireg(dh), R_T1);
        vtop->r = dl;
        vtop->r2 = dh;
        return;
    }
    gen_opil(op);
}

/* a float: the machine's ops; a double: compiler-rt's calls */
ST_FUNC void gen_opf(int op)
{
    int ft = vtop[0].type.t & VT_BTYPE, func = 0, cond = -1;
    CType type = vtop[0].type;
    if (ft == VT_FLOAT) {
        int a, b, d, f;
        gv2(RC_FLOAT, RC_FLOAT);
        a = ireg(vtop[-1].r);
        b = ireg(vtop[0].r);
        vtop -= 2;
        d = get_reg(RC_FLOAT);
        vtop++;
        switch (op) {
        case '+': f = F_ADD; break;
        case '-': f = F_SUB; break;
        case '*': f = F_MUL; break;
        case '/': f = F_DIV; break;
        default: f = -1; break;
        }
        if (f >= 0) {
            emit_r(OP_F(f), ireg(d), a, b, 0);
            vtop->r = d;
            vtop->type = type;
            return;
        }
        switch (op) {
        case TOK_EQ: emit_r(OP_F(F_EQ), ireg(d), a, b, 0); op = TOK_NE; break;
        case TOK_NE: emit_r(OP_F(F_EQ), ireg(d), a, b, 0); op = TOK_EQ; break;
        case TOK_LT: emit_r(OP_F(F_LT), ireg(d), a, b, 0); op = TOK_NE; break;
        case TOK_LE: emit_r(OP_F(F_LE), ireg(d), a, b, 0); op = TOK_NE; break;
        case TOK_GT: emit_r(OP_F(F_LT), ireg(d), b, a, 0); op = TOK_NE; break;
        case TOK_GE: emit_r(OP_F(F_LE), ireg(d), b, a, 0); op = TOK_NE; break;
        default: tcc_error("gpu_vm: float op %d", op);
        }
        vset_VT_CMP(op);
        vtop->cmp_r = ireg(d) | R_ZERO << 8;
        return;
    }
    switch (op) {
    case '*': func = TOK___muldf3; break;
    case '+': func = TOK___adddf3; break;
    case '-': func = TOK___subdf3; break;
    case '/': func = TOK___divdf3; break;
    case TOK_EQ: func = TOK___eqdf2; cond = 1; break;
    case TOK_NE: func = TOK___nedf2; cond = 0; break;
    case TOK_LT: func = TOK___ltdf2; cond = 10; break;
    case TOK_GE: func = TOK___gedf2; cond = 11; break;
    case TOK_LE: func = TOK___ledf2; cond = 12; break;
    case TOK_GT: func = TOK___gtdf2; cond = 13; break;
    default: tcc_error("gpu_vm: double op %d", op);
    }
    save_regs(1);
    /* arg 2 in a2:a3, arg 1 in a0:a1 */
    gv(RC_R(2));
    if (vtop->r2 != TREG_R(3)) {
        emit_mv(ireg(TREG_R(3)), ireg(vtop->r2));
        vtop->r2 = TREG_R(3);
    }
    vswap();
    gv(RC_R(0));
    if (vtop->r2 != TREG_R(1)) {
        emit_mv(ireg(TREG_R(1)), ireg(vtop->r2));
        vtop->r2 = TREG_R(1);
    }
    vpush_helper_func(func);
    gcall_or_jmp(1);
    vtop -= 3;
    vpushi(0);
    vtop->r = REG_IRET;
    vtop->r2 = VT_CONST;
    if (cond < 0) {
        vtop->type = type;
        vtop->r2 = TREG_R(1);
    } else {
        vpushi(0);
        gen_opil(op);
    }
}

/* a value in a register of its own, converted by one op */
static void cvt_op(unsigned opc, int t)
{
    int a, d;
    gv(RC_INT);
    a = ireg(vtop->r);
    vtop--;
    d = get_reg(RC_INT);
    vtop++;
    emit_r(opc, ireg(d), a, 0, 0);
    vtop->r = d;
    vtop->r2 = VT_CONST;
    vtop->type.t = t;
}

/* a library call of one argument (two words for a long long or a double), its result in a0 (and a1) */
static void cvt_call(int func, int t)
{
    int st = vtop->type.t & VT_BTYPE;
    save_regs(1);
    gv(RC_R(0));
    if ((st == VT_LLONG || st == VT_DOUBLE || st == VT_LDOUBLE) && vtop->r2 != TREG_R(1)) {
        emit_mv(ireg(TREG_R(1)), ireg(vtop->r2));
        vtop->r2 = TREG_R(1);
    }
    vpush_helper_func(func);
    gcall_or_jmp(1);
    vtop -= 2;
    vpushi(0);
    vtop->type.t = t;
    vtop->r = REG_IRET;
    if ((t & VT_BTYPE) == VT_LLONG || (t & VT_BTYPE) == VT_DOUBLE || (t & VT_BTYPE) == VT_LDOUBLE)
        vtop->r2 = TREG_R(1);
}

ST_FUNC void gen_cvt_itof(int t)
{
    int u = vtop->type.t & VT_UNSIGNED;
    int l = (vtop->type.t & VT_BTYPE) == VT_LLONG;
    if (t == VT_FLOAT) {
        if (!l)
            cvt_op(OP_F(u ? F_UTOF : F_ITOF), t);
        else
            cvt_call(u ? TOK___floatundisf : TOK___floatdisf, t);
    } else if (l)
        cvt_call(u ? TOK___floatundidf : TOK___floatdidf, t);
    else
        cvt_call(u ? TOK___floatunsidf : TOK___floatsidf, t);
}

ST_FUNC void gen_cvt_ftoi(int t)
{
    int ft = vtop->type.t & VT_BTYPE;
    int l = (t & VT_BTYPE) == VT_LLONG;
    int u = t & VT_UNSIGNED;
    if (ft == VT_FLOAT) {
        if (!l)
            cvt_op(OP_F(u ? F_FTOU : F_FTOI), t);
        else
            cvt_call(u ? TOK___fixunssfdi : TOK___fixsfdi, t);
    } else if (l)
        cvt_call(u ? TOK___fixunsdfdi : TOK___fixdfdi, t);
    else
        cvt_call(u ? TOK___fixunsdfsi : TOK___fixdfsi, t);
}

ST_FUNC void gen_cvt_ftof(int dt)
{
    int st = vtop->type.t & VT_BTYPE;
    dt &= VT_BTYPE;
    if (st == dt || ((st == VT_DOUBLE || st == VT_LDOUBLE) && (dt == VT_DOUBLE || dt == VT_LDOUBLE)))
        return;
    cvt_call(dt == VT_FLOAT ? TOK___truncdfsf2 : TOK___extendsfdf2, dt);
}

ST_FUNC void gen_increment_tcov(SValue *sv)
{
    int r1;
    vpushv(sv);
    vtop->r = r1 = get_reg(RC_INT);
    r1 = ireg(r1);
    emit_lpc_sym(R_T2, sv->sym, sv->c.i);
    emit_mem(M_LOAD, W_W, r1, R_T2, 0);
    emit_i(OP_II(I_ADD), r1, r1, 1);
    emit_mem(M_STORE, W_W, r1, R_T2, 0);
    vpop();
}

ST_FUNC void ggoto(void)
{
    gcall_or_jmp(0);
    vtop--;
}

ST_FUNC void gen_vla_sp_save(int addr)
{
    emit_mem(M_STORE, W_W, R_SP, R_FP, addr);
}

ST_FUNC void gen_vla_sp_restore(int addr)
{
    emit_mem(M_LOAD, W_W, R_SP, R_FP, addr);
}

ST_FUNC void gen_vla_alloc(CType *type, int align)
{
    int rr = ireg(gv(RC_INT));
    emit_i(OP_II(I_ADD), R_T1, rr, 15);
    emit_i(OP_II(I_AND), R_T1, R_T1, -16);
    emit_r(OP_I(I_SUB), R_SP, R_SP, R_T1, 0);
    vpop();
}
#endif
