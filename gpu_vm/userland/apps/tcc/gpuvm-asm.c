/* The little of gpu_vm's assembly TinyCC needs (gpu_vm/userland/apps/tcc/README.md): GCC's inline asm with the
   ops the machine's headers use (mc.h: wait, sync, mfsr) and a few more, an op to a bundle as the code generator
   makes them. Registers are r0-r63 or the ABI's names; asm operands are given a0-a7. */
#ifdef TARGET_DEFS_ONLY

#define CONFIG_TCC_ASM
#define NB_ASM_REGS 64

ST_FUNC void g(int c);
ST_FUNC void gen_le16(int c);
ST_FUNC void gen_le32(int c);

#else

#define USING_GLOBALS
#include "tcc.h"

ST_FUNC void gvm_bundle(uint32_t w0, uint32_t w2, int long0, uint32_t ctl);

#define A_CTRL_NONE (5 << 7)
#define A_WORD_R(opc, rd, ra, rb, rc) ((uint32_t)(opc) << 12 | ((rd) & 63u) << 6 | ((ra) & 63u) | ((rc) & 63u) << 20 | ((rb) & 63u) << 26)

ST_FUNC void g(int c)
{
    int ind1;
    if (nocode_wanted)
        return;
    ind1 = ind + 1;
    if (ind1 > cur_text_section->data_allocated)
        section_realloc(cur_text_section, ind1);
    cur_text_section->data[ind] = c;
    ind = ind1;
}

ST_FUNC void gen_le16(int i)
{
    g(i);
    g(i >> 8);
}

ST_FUNC void gen_le32(int i)
{
    gen_le16(i);
    gen_le16(i >> 16);
}

ST_FUNC void gen_expr32(ExprValue *pe)
{
    gen_le32(pe->v);
}

static const char *const sr_names[] = {
    "status", "epc", "cause", "tval", "tvec", "scratch", "ien", "ipend", "tcmp_lo", "tcmp_hi",
    "time_lo", "time_hi", "cycle", "coreid", "player", "random", "left", "ops", "xr", "npair", NULL
};

/* a register's number from its name, or -1 */
static int reg_of_name(const char *s)
{
    int n;
    char *e;
    if (!strcmp(s, "zero")) return 0;
    if (!strcmp(s, "ra")) return 1;
    if (!strcmp(s, "sp")) return 2;
    if (!strcmp(s, "gp")) return 3;
    if (!strcmp(s, "tp")) return 4;
    if (!strcmp(s, "fp")) return 32;
    if ((s[0] == 'r' || s[0] == 'x' || s[0] == 'a' || s[0] == 't' || s[0] == 's') && s[1] >= '0' && s[1] <= '9') {
        n = strtol(s + 1, &e, 10);
        if (*e)
            return -1;
        switch (s[0]) {
        case 'r': case 'x': return n < 64 ? n : -1;
        case 'a': return n < 8 ? 5 + n : -1;
        case 't': return n <= 18 ? 13 + n : n <= 26 ? 56 + n - 19 : -1;
        case 's': return n <= 23 ? 32 + n : -1;
        }
    }
    return -1;
}

static int parse_reg(void)
{
    int r = tok >= TOK_IDENT ? reg_of_name(get_tok_str(tok, NULL)) : -1;
    if (r < 0)
        expect("register");
    next();
    return r;
}

static int parse_sr(TCCState *s1)
{
    int i;
    ExprValue e;
    if (tok >= TOK_IDENT)
        for (i = 0; sr_names[i]; i++)
            if (!strcmp(get_tok_str(tok, NULL), sr_names[i])) {
                next();
                return i;
            }
    asm_expr(s1, &e);
    return e.v;
}

static void comma(void)
{
    skip(',');
}

/* an R form with a register, or the I form with an immediate (the long one when it does not fit 12 bits) */
static void op3(TCCState *s1, unsigned opc_r, unsigned opc_i)
{
    int rd, ra, rb;
    ExprValue e;
    rd = parse_reg(); comma();
    ra = parse_reg(); comma();
    if (tok >= TOK_IDENT && reg_of_name(get_tok_str(tok, NULL)) >= 0) {
        rb = parse_reg();
        gvm_bundle(A_WORD_R(opc_r, rd, ra, rb, 0), 0, 0, A_CTRL_NONE);
        return;
    }
    asm_expr(s1, &e);
    if ((int)e.v >= -2048 && (int)e.v <= 2047)
        gvm_bundle((uint32_t)opc_i << 12 | rd << 6 | ra | ((uint32_t)e.v & 0xfff) << 20, 0, 0, A_CTRL_NONE);
    else
        gvm_bundle((uint32_t)opc_i << 12 | rd << 6 | ra, e.v, 1, A_CTRL_NONE);
}

ST_FUNC void asm_opcode(TCCState *s1, int token)
{
    static const struct { const char *name; unsigned f; } ints[] = {
        {"add", 0}, {"and", 1}, {"or", 2}, {"xor", 3}, {"sub", 4}, {"slt", 8}, {"sltu", 9}, {"min", 10}, {"minu", 11},
        {"max", 14}, {"maxu", 15}, {"sll", 16}, {"srl", 17}, {"sra", 18}, {"mul", 19}, {"seq", 22}, {"sne", 23}, {NULL, 0}
    };
    const char *name = get_tok_str(token, NULL);
    int i, rd, ra;
    ExprValue e;

    if (!strcmp(name, "nop")) {
        gvm_bundle(0, 0, 0, A_CTRL_NONE);
        return;
    }
    /* the extended class's ops of no operands */
    if (!strcmp(name, "wait") || !strcmp(name, "sync") || !strcmp(name, "fence") || !strcmp(name, "break")
        || !strcmp(name, "syscall")) {
        unsigned opc = !strcmp(name, "wait") ? 0x12 : !strcmp(name, "sync") ? 0x13 : !strcmp(name, "fence") ? 0x14
            : !strcmp(name, "break") ? 0x11 : 0x10;
        gvm_bundle(opc << 12, 0, 0, A_CTRL_NONE);
        return;
    }
    if (!strcmp(name, "mfsr")) {
        rd = parse_reg(); comma();
        i = parse_sr(s1);
        gvm_bundle(0x18u << 12 | rd << 6 | ((uint32_t)i & 0xfff) << 20, 0, 0, A_CTRL_NONE);
        return;
    }
    if (!strcmp(name, "mtsr")) {
        i = parse_sr(s1); comma();
        ra = parse_reg();
        gvm_bundle(0x19u << 12 | ra | ((uint32_t)i & 0xfff) << 20, 0, 0, A_CTRL_NONE);
        return;
    }
    if (!strcmp(name, "mv")) {
        rd = parse_reg(); comma();
        ra = parse_reg();
        gvm_bundle(0x60u << 12 | rd << 6 | ra, 0, 0, A_CTRL_NONE);
        return;
    }
    if (!strcmp(name, "li")) {
        rd = parse_reg(); comma();
        asm_expr(s1, &e);
        gvm_bundle(0x60u << 12 | rd << 6, e.v, 1, A_CTRL_NONE);
        return;
    }
    for (i = 0; ints[i].name; i++)
        if (!strcmp(name, ints[i].name)) {
            op3(s1, 0x40 + ints[i].f, 0x60 + ints[i].f);
            return;
        }
    tcc_error("gpu_vm: unknown instruction '%s'", name);
}

ST_FUNC void subst_asm_operand(CString *add_str, SValue *sv, int modifier)
{
    int r = sv->r;
    if ((r & VT_VALMASK) == VT_CONST) {
        if (r & VT_SYM) {
            const char *name = get_tok_str(sv->sym->v, NULL);
            if (sv->sym->v >= SYM_FIRST_ANOM)
                get_asm_sym(tok_alloc(name, strlen(name))->tok, sv->sym);
            cstr_cat(add_str, name, -1);
            if ((uint32_t)sv->c.i == 0)
                return;
            cstr_ccat(add_str, '+');
        }
        cstr_printf(add_str, "%d", (int)(modifier == 'n' ? -sv->c.i : sv->c.i));
    } else if ((r & VT_VALMASK) == VT_LOCAL) {
        cstr_printf(add_str, "%d", (int)sv->c.i);
    } else {
        int reg = r & VT_VALMASK;
        if (reg >= VT_CONST)
            tcc_internal_error("");
        cstr_printf(add_str, "r%d", reg);
    }
}

/* TinyCC's register of a machine register (a0-a7, t0-t7 are its 0-15) */
static int tcc_reg(int r)
{
    if (r < 5 || r > 20)
        tcc_error("gpu_vm: asm operand in r%d", r);
    return r - 5;
}

/* the callee-saved registers an asm clobbers are kept on the stack around it */
static int saved_reg(int r)
{
    return r >= 33 && r <= 55;
}

ST_FUNC void asm_gen_code(ASMOperand *operands, int nb_operands, int nb_outputs, int is_output,
                          uint8_t *clobber_regs, int out_reg)
{
    uint8_t regs_allocated[NB_ASM_REGS];
    ASMOperand *op;
    int i, reg;

    memcpy(regs_allocated, clobber_regs, sizeof(regs_allocated));
    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        if (op->reg >= 0)
            regs_allocated[op->reg] = 1;
    }
    if (!is_output) {
        for (reg = 0; reg < NB_ASM_REGS; reg++)
            if (saved_reg(reg) && regs_allocated[reg]) {
                gvm_bundle(0x60u << 12 | 2 << 6 | 2 | (uint32_t)(-16 & 0xfff) << 20, 0, 0, A_CTRL_NONE);   /* addi sp, sp, -16 */
                gvm_bundle(0x88u << 12 | reg << 6 | 2, 0, 0, A_CTRL_NONE);                                 /* sw reg, 0(sp) */
            }
        for (i = 0; i < nb_operands; i++) {
            op = &operands[i];
            if (op->reg < 0)
                continue;
            if ((op->vt->r & VT_VALMASK) == VT_LLOCAL && op->is_memory) {
                SValue sv = *op->vt;
                sv.r = (sv.r & ~VT_VALMASK) | VT_LOCAL | VT_LVAL;
                sv.type.t = VT_PTR;
                load(tcc_reg(op->reg), &sv);
            } else if (i >= nb_outputs || op->is_rw) {
                load(tcc_reg(op->reg), op->vt);
                if (op->is_llong)
                    tcc_error("gpu_vm: a long long asm operand");
            }
        }
    } else {
        for (i = 0; i < nb_outputs; i++) {
            op = &operands[i];
            if (op->reg < 0)
                continue;
            if ((op->vt->r & VT_VALMASK) == VT_LLOCAL) {
                if (!op->is_memory) {
                    SValue sv = *op->vt;
                    sv.r = (sv.r & ~VT_VALMASK) | VT_LOCAL;
                    sv.type.t = VT_PTR;
                    load(tcc_reg(out_reg), &sv);
                    sv = *op->vt;
                    sv.r = (sv.r & ~VT_VALMASK) | tcc_reg(out_reg);
                    store(tcc_reg(op->reg), &sv);
                }
            } else {
                store(tcc_reg(op->reg), op->vt);
                if (op->is_llong)
                    tcc_error("gpu_vm: a long long asm operand");
            }
        }
        for (reg = NB_ASM_REGS - 1; reg >= 0; reg--)
            if (saved_reg(reg) && regs_allocated[reg]) {
                gvm_bundle(0x80u << 12 | reg << 6 | 2, 0, 0, A_CTRL_NONE);                     /* lw reg, 0(sp) */
                gvm_bundle(0x60u << 12 | 2 << 6 | 2 | 16u << 20, 0, 0, A_CTRL_NONE);          /* addi sp, sp, 16 */
            }
    }
}

static inline int constraint_priority(const char *str)
{
    int priority = 0, c, pr;
    for (;;) {
        c = *str;
        if (c == '\0')
            break;
        str++;
        switch (c) {
        case 'r': case 'f': case 'p': case 'A':
            pr = 3;
            break;
        case 'I': case 'i': case 'n': case 'm': case 'g':
            pr = 4;
            break;
        default:
            tcc_error("gpu_vm: unknown asm constraint '%c'", c);
        }
        if (pr > priority)
            priority = pr;
    }
    return priority;
}

static const char *skip_constraint_modifiers(const char *p)
{
    while (*p == '=' || *p == '&' || *p == '+' || *p == '%')
        p++;
    return p;
}

#define REG_OUT_MASK 0x01
#define REG_IN_MASK 0x02
#define is_reg_allocated(reg) (regs_allocated[reg] & reg_mask)

ST_FUNC void asm_compute_constraints(ASMOperand *operands, int nb_operands, int nb_outputs,
                                    const uint8_t *clobber_regs, int *pout_reg)
{
    ASMOperand *op;
    int sorted_op[MAX_ASM_OPERANDS];
    int i, j, k, p1, p2, tmp, reg, c, reg_mask;
    const char *str;
    uint8_t regs_allocated[NB_ASM_REGS];

    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        op->input_index = -1;
        op->ref_index = -1;
        op->reg = -1;
        op->is_memory = 0;
        op->is_rw = 0;
    }
    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        str = skip_constraint_modifiers(op->constraint);
        if (isnum(*str) || *str == '[') {
            k = find_constraint(operands, nb_operands, str, NULL);
            if ((unsigned)k >= i || i < nb_outputs)
                tcc_error("invalid reference in constraint %d ('%s')", i, str);
            op->ref_index = k;
            if (operands[k].input_index >= 0)
                tcc_error("cannot reference twice the same operand");
            operands[k].input_index = i;
            op->priority = 5;
        } else if ((op->vt->r & VT_VALMASK) == VT_LOCAL && op->vt->sym
                   && (reg = op->vt->sym->r & VT_VALMASK) < VT_CONST) {
            op->priority = 1;
            op->reg = reg + 5;
        } else {
            op->priority = constraint_priority(str);
        }
    }
    for (i = 0; i < nb_operands; i++)
        sorted_op[i] = i;
    for (i = 0; i < nb_operands - 1; i++)
        for (j = i + 1; j < nb_operands; j++) {
            p1 = operands[sorted_op[i]].priority;
            p2 = operands[sorted_op[j]].priority;
            if (p2 < p1) {
                tmp = sorted_op[i];
                sorted_op[i] = sorted_op[j];
                sorted_op[j] = tmp;
            }
        }
    for (i = 0; i < NB_ASM_REGS; i++)
        regs_allocated[i] = clobber_regs[i] ? REG_IN_MASK | REG_OUT_MASK : 0;

    for (i = 0; i < nb_operands; i++) {
        j = sorted_op[i];
        op = &operands[j];
        str = op->constraint;
        if (op->ref_index >= 0)
            continue;
        if (op->input_index >= 0)
            reg_mask = REG_IN_MASK | REG_OUT_MASK;
        else if (j < nb_outputs)
            reg_mask = REG_OUT_MASK;
        else
            reg_mask = REG_IN_MASK;
        if (op->reg >= 0) {
            if (is_reg_allocated(op->reg))
                tcc_error("asm regvar requests register that's taken already");
            reg = op->reg;
        }
    try_next:
        c = *str++;
        switch (c) {
        case '=':
            goto try_next;
        case '+':
            op->is_rw = 1;
            /* fall through */
        case '&':
            if (j >= nb_outputs)
                tcc_error("'%c' modifier can only be applied to outputs", c);
            reg_mask = REG_IN_MASK | REG_OUT_MASK;
            goto try_next;
        case 'r': case 'f': case 'p': case 'A':
            if ((reg = op->reg) >= 0)
                goto reg_found;
            for (reg = 5; reg <= 12; reg++)
                if (!is_reg_allocated(reg))
                    goto reg_found;
            goto try_next;
        reg_found:
            op->is_llong = 0;
            op->reg = reg;
            regs_allocated[reg] |= reg_mask;
            break;
        case 'I': case 'i': case 'n':
            if (!((op->vt->r & (VT_VALMASK | VT_LVAL)) == VT_CONST))
                goto try_next;
            break;
        case 'm': case 'g':
            if (j < nb_outputs || c == 'm') {
                if ((op->vt->r & VT_VALMASK) == VT_LLOCAL) {
                    for (reg = 5; reg <= 12; reg++)
                        if (!(regs_allocated[reg] & REG_IN_MASK))
                            goto reg_found1;
                    goto try_next;
                reg_found1:
                    regs_allocated[reg] |= REG_IN_MASK;
                    op->reg = reg;
                    op->is_memory = 1;
                }
            }
            break;
        default:
            tcc_error("asm constraint %d ('%s') could not be satisfied", j, op->constraint);
            break;
        }
        if (op->input_index >= 0) {
            operands[op->input_index].reg = op->reg;
            operands[op->input_index].is_llong = op->is_llong;
        }
    }
    *pout_reg = -1;
    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        if (op->reg >= 0 && (op->vt->r & VT_VALMASK) == VT_LLOCAL && !op->is_memory) {
            for (reg = 5; reg <= 12; reg++)
                if (!(regs_allocated[reg] & REG_OUT_MASK))
                    goto reg_found2;
            tcc_error("could not find free output register for reloading");
        reg_found2:
            *pout_reg = reg;
            break;
        }
    }
}

ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str)
{
    int reg;
    if (!strcmp(str, "memory") || !strcmp(str, "cc") || !strcmp(str, "flags"))
        return;
    reg = reg_of_name(str);
    if (reg < 0)
        tcc_error("invalid clobber register '%s'", str);
    clobber_regs[reg] = 1;
}

ST_FUNC int asm_parse_regvar(int t)
{
    return t >= TOK_IDENT ? reg_of_name(get_tok_str(t, NULL)) : -1;
}
#endif
