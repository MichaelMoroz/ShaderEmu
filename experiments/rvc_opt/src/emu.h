#ifndef EMU_H
#define EMU_H

#include "mmu.h"

#define AS_SIGNED(val) (asint(val))
#define AS_UNSIGNED(val) (asuint(val))

// this is just a leftover from C, where AS_SIGNED/AS_UNSIGNED needed this as consts
// please don't judge me
#define ZERO 0
#define ONE 1

#define DEF(name, fmt_t, code) \
    void emu_##name(uint ins_word, inout ins_ret ret, fmt_t ins) { code }

#define WR_RD(code) { ret.write_reg = ins.rd; ret.write_val = AS_UNSIGNED(code); }
#define WR_PC(code) { ret.pc_val = AS_UNSIGNED(code); }
#define WR_CSR(code) { ret.csr_write = ins.csr; ret.csr_val = AS_UNSIGNED(code); }
#define WR_MEM(addr, code, size) { ret.mem_wr_addr = addr; ret.mem_wr_size = size; ret.mem_wr_value = AS_UNSIGNED(code); }

/*
 *   BEGIN INSTRUCTIONS
 */

static uint prepared_mem_val;

DEF(add, FormatR, { // rv32i
    WR_RD(AS_SIGNED(xreg(ins.rs1)) + AS_SIGNED(xreg(ins.rs2)));
})
DEF(addi, FormatI, { // rv32i
    WR_RD(AS_SIGNED(xreg(ins.rs1)) + AS_SIGNED(ins.imm));
})
DEF(amoswap_w, FormatR, { // rv32a
    uint tmp = prepared_mem_val;
    WR_MEM(xreg(ins.rs1), (xreg(ins.rs2)), 32)
    WR_RD(tmp)
})
DEF(amoadd_w, FormatR, { // rv32a
    WR_MEM(xreg(ins.rs1), (xreg(ins.rs2) + prepared_mem_val), 32)
    WR_RD(prepared_mem_val)
})
DEF(amoxor_w, FormatR, { // rv32a
    WR_MEM(xreg(ins.rs1), (xreg(ins.rs2) ^ prepared_mem_val), 32)
    WR_RD(prepared_mem_val)
})
DEF(amoand_w, FormatR, { // rv32a
    WR_MEM(xreg(ins.rs1), (xreg(ins.rs2) & prepared_mem_val), 32)
    WR_RD(prepared_mem_val)
})
DEF(amoor_w, FormatR, { // rv32a
    WR_MEM(xreg(ins.rs1), (xreg(ins.rs2) | prepared_mem_val), 32)
    WR_RD(prepared_mem_val)
})
DEF(amomin_w, FormatR, { // rv32a
    uint sec = xreg(ins.rs2);
    WR_MEM(xreg(ins.rs1), (AS_SIGNED(sec) < AS_SIGNED(prepared_mem_val) ? sec : prepared_mem_val), 32)
    WR_RD(prepared_mem_val)
})
DEF(amomax_w, FormatR, { // rv32a
    uint sec = xreg(ins.rs2);
    WR_MEM(xreg(ins.rs1), (AS_SIGNED(sec) > AS_SIGNED(prepared_mem_val) ? sec : prepared_mem_val), 32)
    WR_RD(prepared_mem_val)
})
DEF(amominu_w, FormatR, { // rv32a
    uint sec = xreg(ins.rs2);
    WR_MEM(xreg(ins.rs1), (AS_UNSIGNED(sec) < AS_UNSIGNED(prepared_mem_val) ? sec : prepared_mem_val), 32)
    WR_RD(prepared_mem_val)
})
DEF(amomaxu_w, FormatR, { // rv32a
    uint sec = xreg(ins.rs2);
    WR_MEM(xreg(ins.rs1), (AS_UNSIGNED(sec) > AS_UNSIGNED(prepared_mem_val) ? sec : prepared_mem_val), 32)
    WR_RD(prepared_mem_val)
})
DEF(and, FormatR, { // rv32i
    WR_RD(xreg(ins.rs1) & xreg(ins.rs2))
})
DEF(andi, FormatI, { // rv32i
    WR_RD(xreg(ins.rs1) & ins.imm)
})
DEF(auipc, FormatU, { // rv32i
    WR_RD(cpu.pc + ins.imm)
})
DEF(beq, FormatB, { // rv32i
    if (xreg(ins.rs1) == xreg(ins.rs2)) {
        WR_PC(cpu.pc + ins.imm);
    }
})
DEF(bge, FormatB, { // rv32i
    if (AS_SIGNED(xreg(ins.rs1)) >= AS_SIGNED(xreg(ins.rs2))) {
        WR_PC(cpu.pc + ins.imm);
    }
})
DEF(bgeu, FormatB, { // rv32i
    if (AS_UNSIGNED(xreg(ins.rs1)) >= AS_UNSIGNED(xreg(ins.rs2))) {
        WR_PC(cpu.pc + ins.imm);
    }
})
DEF(blt, FormatB, { // rv32i
    if (AS_SIGNED(xreg(ins.rs1)) < AS_SIGNED(xreg(ins.rs2))) {
        WR_PC(cpu.pc + ins.imm);
    }
})
DEF(bltu, FormatB, { // rv32i
    if (AS_UNSIGNED(xreg(ins.rs1)) < AS_UNSIGNED(xreg(ins.rs2))) {
        WR_PC(cpu.pc + ins.imm);
    }
})
DEF(bne, FormatB, { // rv32i
    if (xreg(ins.rs1) != xreg(ins.rs2)) {
        WR_PC(cpu.pc + ins.imm);
    }
})
DEF(csrrc, FormatCSR, { // system
    uint rs = xreg(ins.rs);
    if (rs != 0) {
        WR_CSR(ins.value & ~rs);
    }
    WR_RD(ins.value)
})
DEF(csrrci, FormatCSR, { // system
    if (ins.rs != 0) {
        WR_CSR(ins.value & (~ins.rs));
    }
    WR_RD(ins.value)
})
DEF(csrrs, FormatCSR, { // system
    uint rs = xreg(ins.rs);
    if (rs != 0) {
        WR_CSR(ins.value | rs);
    }
    WR_RD(ins.value)
})
DEF(csrrsi, FormatCSR, { // system
    if (ins.rs != 0) {
        WR_CSR(ins.value | ins.rs);
    }
    WR_RD(ins.value)
})
DEF(csrrw, FormatCSR, { // system
    WR_CSR(xreg(ins.rs));
    WR_RD(ins.value)
})
DEF(csrrwi, FormatCSR, { // system
    WR_CSR(ins.rs);
    WR_RD(ins.value)
})
DEF(div, FormatR, { // rv32m
    uint dividend = xreg(ins.rs1);
    uint divisor = xreg(ins.rs2);
    uint result;
    if (divisor == 0) {
        result = 0xFFFFFFFF;
    } else if (dividend == 0x80000000 && divisor == 0xFFFFFFFF) {
        result = dividend;
    } else {
        int tmp = AS_SIGNED(dividend) / AS_SIGNED(divisor);
        result = AS_UNSIGNED(tmp);
    }
    WR_RD(result)
})
DEF(divu, FormatR, { // rv32m
    uint dividend = xreg(ins.rs1);
    uint divisor = xreg(ins.rs2);
    uint result;
    if (divisor == 0) {
        result = 0xFFFFFFFF;
    } else {
        result = dividend / divisor;
    }
    WR_RD(result)
})
DEF(ebreak, FormatEmpty, { // system
    // unnecessary?
})
DEF(ecall, FormatEmpty, { // system
    /* if (xreg(17) == 93) { */
    /*     // EXIT CALL - only used for self-tests */
    /*     uint status = xreg(10); */
    /*     cpu.stall = STALL_EXIT_CALL; */
    /*     write_csr_raw(0x200, status); */
    /* } else { */
    ret.trap.en = true;
    ret.trap.value = cpu.pc;
#ifdef M_MODE_ONLY
    if (true) {
        ret.trap.type = trap_EnvironmentCallFromMMode;
    } else
#endif
    if (cpu.csr.privilege == PRIV_USER) {
        ret.trap.type = trap_EnvironmentCallFromUMode;
    } else if (cpu.csr.privilege == PRIV_SUPERVISOR) {
        ret.trap.type = trap_EnvironmentCallFromSMode;
    } else { // PRIV_MACHINE
        ret.trap.type = trap_EnvironmentCallFromMMode;
    }
    /* } */
})
DEF(fence, FormatEmpty, { // rv32i
    // skip
})
DEF(fence_i, FormatEmpty, { // rv32i
    cpu.stall = STALL_FENCE;
})
DEF(jal, FormatJ, { // rv32i
    WR_RD(cpu.pc + 4);
    WR_PC(cpu.pc + ins.imm);
})
DEF(jalr, FormatI, { // rv32i
    WR_RD(cpu.pc + 4);
    WR_PC(xreg(ins.rs1) + ins.imm);
})
DEF(lb, FormatI, { // rv32i
    uint tmp = sign_extend(prepared_mem_val & 0xff, 8);
    WR_RD(tmp)
})
DEF(lbu, FormatI, { // rv32i
    uint tmp = prepared_mem_val & 0xff;
    WR_RD(tmp)
})
DEF(lh, FormatI, { // rv32i
    uint tmp = sign_extend(prepared_mem_val & 0xffff, 16);
    WR_RD(tmp)
})
DEF(lhu, FormatI, { // rv32i
    uint tmp = prepared_mem_val & 0xffff;
    WR_RD(tmp)
})
DEF(lr_w, FormatR, { // rv32a
    uint addr = xreg(ins.rs1);
    uint tmp = prepared_mem_val;
    cpu.reservation_en = true;
    cpu.reservation_addr = addr;
    WR_RD(tmp)
})
DEF(lui, FormatU, { // rv32i
    WR_RD(ins.imm)
})
DEF(lw, FormatI, { // rv32i
    // would need sign extend for xlen > 32
    WR_RD(prepared_mem_val)
})
DEF(mret, FormatEmpty, { // system
    uint newpc = get_csr(CSR_MEPC, ret);
    if (!ret.trap.en) {
        uint status = read_csr_raw(CSR_MSTATUS);
        uint mpie = (status >> 7) & 1;
        uint mpp = (status >> 11) & 0x3;
        uint mprv = mpp == PRIV_MACHINE ? ((status >> 17) & 1) : 0;
        uint new_status = (status & ~0x21888) | (mprv << 17) | (mpie << 3) | (1 << 7);
#ifdef M_MODE_ONLY
        write_csr_raw(CSR_MSTATUS, new_status | (PRIV_MACHINE << 11));   // MPP reads as machine
#else
        write_csr_raw(CSR_MSTATUS, new_status);
        cpu.csr.privilege = mpp; hot_flush();
#endif
        WR_PC(newpc)
    }
})
DEF(mul, FormatR, { // rv32m
    uint tmp = AS_SIGNED(xreg(ins.rs1)) * AS_SIGNED(xreg(ins.rs2));
    WR_RD(tmp)
})
// Exact high word of a 32x32 multiply from 16-bit partial products, for hosts whose double
// support cannot be trusted. Upstream's double version rounds once the product needs more
// than 53 bits, so results differ from it there.
uint mulhu32(uint a, uint b) {
    uint al = a & 0xffff, ah = a >> 16, bl = b & 0xffff, bh = b >> 16;
    uint lh = al * bh, hl = ah * bl;
    uint mid = ((al * bl) >> 16) + (lh & 0xffff) + (hl & 0xffff);
    return ah * bh + (lh >> 16) + (hl >> 16) + (mid >> 16);
}
DEF(mulh, FormatR, { // rv32m
    uint a = xreg(ins.rs1), b = xreg(ins.rs2);
    WR_RD(mulhu32(a, b) - ((a >> 31) ? b : 0) - ((b >> 31) ? a : 0))
})
DEF(mulhsu, FormatR, { // rv32m
    uint a = xreg(ins.rs1), b = xreg(ins.rs2);
    WR_RD(mulhu32(a, b) - ((a >> 31) ? b : 0))
})
DEF(mulhu, FormatR, { // rv32m
    WR_RD(mulhu32(xreg(ins.rs1), xreg(ins.rs2)))
})
DEF(or, FormatR, { // rv32i
    WR_RD(xreg(ins.rs1) | xreg(ins.rs2))
})
DEF(ori, FormatI, { // rv32i
    WR_RD(xreg(ins.rs1) | ins.imm)
})
DEF(rem, FormatR, { // rv32m
    uint dividend = xreg(ins.rs1);
    uint divisor = xreg(ins.rs2);
    uint result;
    if (divisor == 0) {
        result = dividend;
    } else if (dividend == 0x80000000 && divisor == 0xFFFFFFFF) {
        result = 0;
    } else {
        int tmp = AS_SIGNED(dividend) % AS_SIGNED(divisor);
        result = AS_UNSIGNED(tmp);
    }
    WR_RD(result)
})
DEF(remu, FormatR, { // rv32m
    uint dividend = xreg(ins.rs1);
    uint divisor = xreg(ins.rs2);
    uint result;
    if (divisor == 0) {
        result = dividend;
    } else {
        result = dividend % divisor;
    }
    WR_RD(result)
})
DEF(sb, FormatS, { // rv32i
    WR_MEM(ins.addr, xreg(ins.rs2), 8)
})
DEF(sc_w, FormatR, { // rv32a
    // I'm pretty sure this is not it chief, but it does the trick for now
    uint addr = xreg(ins.rs1);
    if (cpu.reservation_en && cpu.reservation_addr == addr) {
        WR_MEM(addr, xreg(ins.rs2), 32);
        cpu.reservation_en = false;
        WR_RD(ZERO)
    } else {
        WR_RD(ONE)
    }
})
DEF(sfence_vma, FormatEmpty, { // system
    hot_flush();
    tlb2_flush();
    /* cpu.stall = STALL_FENCE; */
})
DEF(sh, FormatS, { // rv32i
    WR_MEM(ins.addr, xreg(ins.rs2), 16)
})
DEF(sll, FormatR, { // rv32i
    WR_RD(xreg(ins.rs1) << xreg(ins.rs2))
})
DEF(slli, FormatR, { // rv32i
    uint shamt = (ins_word >> 20) & 0x1F;
    WR_RD(xreg(ins.rs1) << shamt)
})
DEF(slt, FormatR, { // rv32i
    if (AS_SIGNED(xreg(ins.rs1)) < AS_SIGNED(xreg(ins.rs2))) {
        WR_RD(ONE)
    } else {
        WR_RD(ZERO)
    }
})
DEF(slti, FormatI, { // rv32i
    if (AS_SIGNED(xreg(ins.rs1)) < AS_SIGNED(ins.imm)) {
        WR_RD(ONE)
    } else {
        WR_RD(ZERO)
    }
})
DEF(sltiu, FormatI, { // rv32i
    if (AS_UNSIGNED(xreg(ins.rs1)) < AS_UNSIGNED(ins.imm)) {
        WR_RD(ONE)
    } else {
        WR_RD(ZERO)
    }
})
DEF(sltu, FormatR, { // rv32i
    if (AS_UNSIGNED(xreg(ins.rs1)) < AS_UNSIGNED(xreg(ins.rs2))) {
        WR_RD(ONE)
    } else {
        WR_RD(ZERO)
    }
})
DEF(sra, FormatR, { // rv32i
    uint msr = xreg(ins.rs1) & 0x80000000;
    WR_RD(msr ? ~(~xreg(ins.rs1) >> xreg(ins.rs2)) :
                   xreg(ins.rs1) >> xreg(ins.rs2))
})
DEF(srai, FormatR, { // rv32i
    uint msr = xreg(ins.rs1) & 0x80000000;
    uint shamt = (ins_word >> 20) & 0x1F;
    WR_RD(msr ? ~(~xreg(ins.rs1) >> shamt) :
                   xreg(ins.rs1) >> shamt)
})
DEF(sret, FormatEmpty, { // system
    uint newpc = get_csr(CSR_SEPC, ret);
    if (!ret.trap.en) {
        uint status = read_csr_raw(CSR_SSTATUS);
        uint spie = (status >> 5) & 1;
        uint spp = (status >> 8) & 1;
        uint mprv = spp == PRIV_MACHINE ? ((status >> 17) & 1) : 0;
        uint new_status = (status & ~0x20122) | (mprv << 17) | (spie << 1) | (1 << 5);
        write_csr_raw(CSR_SSTATUS, new_status);
        cpu.csr.privilege = spp; hot_flush();
        WR_PC(newpc)
    }
})
DEF(srl, FormatR, { // rv32i
    WR_RD(xreg(ins.rs1) >> xreg(ins.rs2))
})
DEF(srli, FormatR, { // rv32i
    uint shamt = (ins_word >> 20) & 0x1F;
    WR_RD(xreg(ins.rs1) >> shamt)
})
DEF(sub, FormatR, { // rv32i
    WR_RD(AS_SIGNED(xreg(ins.rs1)) - AS_SIGNED(xreg(ins.rs2)));
})
DEF(sw, FormatS, { // rv32i
    WR_MEM(ins.addr, xreg(ins.rs2), 32)
})
DEF(uret, FormatEmpty, { // system
    // unnecessary?
})
DEF(wfi, FormatEmpty, { // system
    // Ends this pass's run of instructions: interrupts, input and the GPU device can only
    // change anything between passes, so there is nothing to wait for inside one.
    cpu.stall = STALL_WFI;
})
DEF(xor, FormatR, { // rv32i
    WR_RD(xreg(ins.rs1) ^ xreg(ins.rs2))
})
DEF(xori, FormatI, { // rv32i
    WR_RD(xreg(ins.rs1) ^ ins.imm)
})

/*
 *   END INSTRUCTIONS
 */

#define RUN(name, data, insf) case data : { \
    PROF(PROF_ins_##name) \
    emu_##name(ins_word, ret, insf); \
    return ret; \
}
ins_ret ins_select(uint ins_word, inout ins_ret ret) {
    uint ins_masked;

    FormatEmpty ins_FormatEmpty = parse_FormatEmpty(ins_word);
    FormatCSR ins_FormatCSR = parse_FormatCSR(ins_word);
    FormatI ins_FormatI = parse_FormatI(ins_word);
    FormatU ins_FormatU = parse_FormatU(ins_word);

    // One dispatch on the major opcode for the instructions that make up most of any workload
    // (integer ALU, branches, jumps, loads, stores). Anything it does not fully recognise
    // falls through to the original decoder below, so behaviour is unchanged.
    uint f_opc = ins_word & 0x7f;
    uint f_f3 = (ins_word >> 12) & 0x7;
    uint f_rd = (ins_word >> 7) & 0x1f;
    bool f_done = true;
    [forcecase]
    switch (f_opc) {
        case 0x37: { // lui
            PROF(PROF_fast_upper)
            ret.write_reg = f_rd; ret.write_val = ins_word & 0xfffff000;
            break;
        }
        case 0x17: { // auipc
            PROF(PROF_fast_upper)
            ret.write_reg = f_rd; ret.write_val = cpu.pc + (ins_word & 0xfffff000);
            break;
        }
        case 0x6f: { // jal
            PROF(PROF_fast_jump)
            FormatJ f_j = parse_FormatJ(ins_word);
            ret.write_reg = f_rd; ret.write_val = cpu.pc + 4;
            ret.pc_val = cpu.pc + f_j.imm;
            break;
        }
        case 0x67: { // jalr
            if (f_f3 == 0) {
                PROF(PROF_fast_jump)
                ret.write_reg = f_rd; ret.write_val = cpu.pc + 4;
                ret.pc_val = xreg(ins_FormatI.rs1) + ins_FormatI.imm;
            } else { f_done = false; }
            break;
        }
        case 0x13: { // addi slti sltiu xori ori andi slli srli srai
            uint a = xreg(ins_FormatI.rs1);
            uint b = ins_FormatI.imm;
            uint sh = (ins_word >> 20) & 0x1f;
            uint f6 = ins_word >> 26;
            uint f_sra = (a & 0x80000000) ? ~(~a >> sh) : a >> sh;
            uint r;
            bool ok = true;
            [forcecase]
            switch (f_f3) {
                case 0: r = a + b; break;
                case 1: r = a << sh; ok = f6 == 0; break;
                case 2: r = AS_SIGNED(a) < AS_SIGNED(b) ? 1 : 0; break;
                case 3: r = a < b ? 1 : 0; break;
                case 4: r = a ^ b; break;
                case 5: r = f6 == 0x10 ? f_sra : a >> sh; ok = f6 == 0 || f6 == 0x10; break;
                case 6: r = a | b; break;
                default: r = a & b; break;
            }
            if (ok) { PROF(PROF_fast_opimm) ret.write_reg = f_rd; ret.write_val = r; } else { f_done = false; }
            break;
        }
        case 0x33: { // add sub sll slt sltu xor srl sra or and (M extension: original decoder)
            uint a = xreg((ins_word >> 15) & 0x1f);
            uint b = xreg((ins_word >> 20) & 0x1f);
            uint f7 = ins_word >> 25;
            uint f_sra = (a & 0x80000000) ? ~(~a >> b) : a >> b;
            uint r;
            bool ok = f7 == 0 || (f7 == 0x20 && (f_f3 == 0 || f_f3 == 5));
            [forcecase]
            switch (f_f3) {
                case 0: r = f7 ? a - b : a + b; break;
                case 1: r = a << b; break;
                case 2: r = AS_SIGNED(a) < AS_SIGNED(b) ? 1 : 0; break;
                case 3: r = a < b ? 1 : 0; break;
                case 4: r = a ^ b; break;
                case 5: r = f7 ? f_sra : a >> b; break;
                case 6: r = a | b; break;
                default: r = a & b; break;
            }
            if (ok) { PROF(PROF_fast_op) ret.write_reg = f_rd; ret.write_val = r; } else { f_done = false; }
            break;
        }
        case 0x63: { // beq bne blt bge bltu bgeu
            uint a = xreg((ins_word >> 15) & 0x1f);
            uint b = xreg((ins_word >> 20) & 0x1f);
            bool lt = AS_SIGNED(a) < AS_SIGNED(b), ltu = a < b;
            bool taken = f_f3 == 0 ? a == b : f_f3 == 1 ? a != b : f_f3 == 4 ? lt : f_f3 == 5 ? !lt : f_f3 == 6 ? ltu : !ltu;
            if (f_f3 != 2 && f_f3 != 3) {
                PROF(PROF_fast_branch)
                FormatB f_b = parse_FormatB(ins_word);
                if (taken) { ret.pc_val = cpu.pc + f_b.imm; }
            } else { f_done = false; }
            break;
        }
        case 0x23: { // sb sh sw
            if (f_f3 < 3) {
                PROF(PROF_fast_store)
                FormatS f_s = parse_FormatS(ins_word);
                ret.mem_wr_addr = f_s.addr; ret.mem_wr_size = 8 << f_f3; ret.mem_wr_value = xreg(f_s.rs2);
            } else { f_done = false; }
            break;
        }
        default:
            f_done = false;
            break;
    }
    if (f_done) {
        return ret;
    }

    /*
       NOTE: The switch statements below can't all use [forcecase].
       While this would be best for performance (according to my testing),
       it breaks compilation very badly, so only do it for the bigger ones.
    */

    ins_masked = ins_word & 0x0000007f;
    [branch]
    switch (ins_masked) {
        RUN(auipc, 0x00000017, ins_FormatU)
        RUN(jal, 0x0000006f, parse_FormatJ(ins_word))
        RUN(lui, 0x00000037, ins_FormatU)
    }

    FormatR ins_FormatR = parse_FormatR(ins_word);

    ins_masked = ins_word & 0xfe00707f;
    [forcecase]
    switch (ins_masked) {
        RUN(add, 0x00000033, ins_FormatR)
        RUN(and, 0x00007033, ins_FormatR)
        RUN(div, 0x02004033, ins_FormatR)
        RUN(divu, 0x02005033, ins_FormatR)
        RUN(mul, 0x02000033, ins_FormatR)
        RUN(mulh, 0x02001033, ins_FormatR)
        RUN(mulhsu, 0x02002033, ins_FormatR)
        RUN(mulhu, 0x02003033, ins_FormatR)
        RUN(or, 0x00006033, ins_FormatR)
        RUN(rem, 0x02006033, ins_FormatR)
        RUN(remu, 0x02007033, ins_FormatR)
        RUN(sll, 0x00001033, ins_FormatR)
        RUN(slt, 0x00002033, ins_FormatR)
        RUN(sltu, 0x00003033, ins_FormatR)
        RUN(sra, 0x40005033, ins_FormatR)
        RUN(srl, 0x00005033, ins_FormatR)
        RUN(sub, 0x40000033, ins_FormatR)
        RUN(xor, 0x00004033, ins_FormatR)
    }

    FormatS ins_FormatS = parse_FormatS(ins_word);
    FormatB ins_FormatB = parse_FormatB(ins_word);

    // since function calls are all inlined we perform memory accesses
    // all together, this way mem_get_word only has to exist once
    // (though this might cause some unnecessary accesses too)
    uint do_mem_read = 0;
    if ((ins_word & 0x0000007f) == 0x00000003) {
        // lX memory read
        do_mem_read = xreg(ins_FormatI.rs1) + ins_FormatI.imm;
    } else if ((ins_word & 0x0000202f) == 0x0000202f) {
        // atomic memory read
        do_mem_read = xreg(ins_FormatR.rs1);
    }
    if (do_mem_read) {
        /* cpu.debug_arb_3 = do_mem_read; */

        uint prepared_read_addr = mmu_translate(ret, do_mem_read, MMU_ACCESS_READ);
        if (ret.trap.en) {
            return ret;
        }
        // take care of unaligned memory reads using a loop
        // so mem_get_word isn't inlined twice
        uint w1 = 0, w2 = 0;
        [loop]
        for (uint ui = 0; ui < ((prepared_read_addr & 0x3) ? 2 : 1); ui++) {
            uint tmp = mem_get_word((prepared_read_addr & (~0x3)) + 0x4 * ui);
            [flatten]
            if (ui) { w2 = tmp; }
            else { w1 = tmp; }
        }
        prepared_mem_val = w1 >> ((do_mem_read & 0x3) * 8);
        prepared_mem_val |= w2 << ((4 - (do_mem_read & 0x3)) * 8);

        /* cpu.debug_arb_4 = w1; */
        /* cpu.debug_arb_5 = w2; */
        /* cpu.debug_arb_6 = prepared_read_addr; */
        /* cpu.debug_arb_7 = prepared_mem_val; */
    }

    if ((ins_word & 0x7f) == 0x03) { // lb lh lw lbu lhu, value already read above
        uint f_l3 = (ins_word >> 12) & 0x7;
        if (f_l3 < 3 || f_l3 == 4 || f_l3 == 5) {
            PROF(PROF_fast_load)
            uint v = prepared_mem_val;
            ret.write_reg = (ins_word >> 7) & 0x1f;
            ret.write_val = f_l3 == 0 ? sign_extend(v & 0xff, 8) : f_l3 == 1 ? sign_extend(v & 0xffff, 16) :
                            f_l3 == 2 ? v : f_l3 == 4 ? v & 0xff : v & 0xffff;
            return ret;
        }
    }

    if ((ins_word & 0x00000073) == 0x00000073) {
        // could be CSR instruction
        ins_FormatCSR.value = get_csr(ins_FormatCSR.csr, ret);
    }

    ins_masked = ins_word & 0x0000707f;
    [forcecase]
    switch (ins_masked) {
        RUN(addi, 0x00000013, ins_FormatI)
        RUN(andi, 0x00007013, ins_FormatI)
        RUN(beq, 0x00000063, ins_FormatB)
        RUN(bge, 0x00005063, ins_FormatB)
        RUN(bgeu, 0x00007063, ins_FormatB)
        RUN(blt, 0x00004063, ins_FormatB)
        RUN(bltu, 0x00006063, ins_FormatB)
        RUN(bne, 0x00001063, ins_FormatB)
        RUN(csrrc, 0x00003073, ins_FormatCSR)
        RUN(csrrci, 0x00007073, ins_FormatCSR)
        RUN(csrrs, 0x00002073, ins_FormatCSR)
        RUN(csrrsi, 0x00006073, ins_FormatCSR)
        RUN(csrrw, 0x00001073, ins_FormatCSR)
        RUN(csrrwi, 0x00005073, ins_FormatCSR)
        RUN(fence, 0x0000000f, ins_FormatEmpty)
        RUN(fence_i, 0x0000100f, ins_FormatEmpty)
        RUN(jalr, 0x00000067, ins_FormatI)
        RUN(lb, 0x00000003, ins_FormatI)
        RUN(lbu, 0x00004003, ins_FormatI)
        RUN(lh, 0x00001003, ins_FormatI)
        RUN(lhu, 0x00005003, ins_FormatI)
        RUN(lw, 0x00002003, ins_FormatI)
        RUN(ori, 0x00006013, ins_FormatI)
        RUN(sb, 0x00000023, ins_FormatS)
        RUN(sh, 0x00001023, ins_FormatS)
        RUN(slti, 0x00002013, ins_FormatI)
        RUN(sltiu, 0x00003013, ins_FormatI)
        RUN(sw, 0x00002023, ins_FormatS)
        RUN(xori, 0x00004013, ins_FormatI)
    }

    ins_masked = ins_word & 0xf800707f;
    [forcecase]
    switch (ins_masked) {
        RUN(amoswap_w, 0x0800202f, ins_FormatR)
        RUN(amoadd_w, 0x0000202f, ins_FormatR)
        RUN(amoxor_w, 0x2000202f, ins_FormatR)
        RUN(amoand_w, 0x6000202f, ins_FormatR)
        RUN(amoor_w, 0x4000202f, ins_FormatR)
        RUN(amomin_w, 0x8000202f, ins_FormatR)
        RUN(amomax_w, 0xa000202f, ins_FormatR)
        RUN(amominu_w, 0xc000202f, ins_FormatR)
        RUN(amomaxu_w, 0xe000202f, ins_FormatR)
        RUN(sc_w, 0x1800202f, ins_FormatR)
    }
    ins_masked = ins_word & 0xf9f0707f;
    [branch]
    switch (ins_masked) {
        RUN(lr_w, 0x1000202f, ins_FormatR)
    }
    ins_masked = ins_word & 0xfc00707f;
    [branch]
    switch (ins_masked) {
        RUN(slli, 0x00001013, ins_FormatR)
        RUN(srai, 0x40005013, ins_FormatR)
        RUN(srli, 0x00005013, ins_FormatR)
    }
    ins_masked = ins_word & 0xfe007fff;
    [branch]
    switch (ins_masked) {
        RUN(sfence_vma, 0x12000073, ins_FormatEmpty)
    }
    ins_masked = ins_word & 0xffffffff;
    [branch]
    switch (ins_masked) {
        RUN(ebreak, 0x00100073, ins_FormatEmpty)
        RUN(ecall, 0x00000073, ins_FormatEmpty)
        RUN(mret, 0x30200073, ins_FormatEmpty)
#ifndef M_MODE_ONLY
        RUN(sret, 0x10200073, ins_FormatEmpty)
#endif
        RUN(uret, 0x00200073, ins_FormatEmpty)
        RUN(wfi, 0x10500073, ins_FormatEmpty)
    }

    PROF(PROF_illegal_ins)
    ret.trap.en = true;
    ret.trap.type = trap_IllegalInstruction;
    ret.trap.value = ins_word;
    return ret;
}


// _Time is constant for the whole pass, so the timer value is computed once.
static uint pass_mtime_lo, pass_mtime_hi;
#ifdef NO_DOUBLES
uniform uint _HostMtimeLo, _HostMtimeHi;  // the same value, computed by the host
void time_prepare() {
    pass_mtime_lo = _HostMtimeLo;
    pass_mtime_hi = _HostMtimeHi;
}
#else
void time_prepare() {
    double mtime = (double)_Time.x * 1000000.0 * 0.1;
    pass_mtime_lo = (uint)(floor(glsl_mod(mtime, 4294967296.0)));
    pass_mtime_hi = (uint)(mtime / 4294967296.0);
}
#endif

// The instruction word cpu_tick already fetched for this tick, if any.
static bool pre_valid = false;
static uint pre_word;

void xreg_set(uint r, uint v) {
    xr[r] = v;
}

// Fast-step address translation is branch-free: identity when paging does not apply, else a
// TLB hit. The two "paging does not apply" flags are recomputed at the end of every general-path
// instruction, which is where paging mode, privilege and mstatus can change.
#ifdef NO_PAGING
#define FAST_XL(ident, mode, vpns, pages, va, ok, pa) \
    bool ok = true; \
    uint pa = (va);
#else
static bool xl_ident_f = false;  // fetches: paging off, or machine mode
static bool xl_ident_d = false;  // loads/stores: paging off, or machine mode without MPRV redirection
// A first-level miss tries the second-level TLB and, on a hit, refills the first level.
#define FAST_XL(ident, mode, vpns, pages, va, ok, pa) \
    uint ok##_slot = ((va) >> 12) & 3; \
    bool ok = ident || idx_uint4(vpns, ok##_slot) == ((va) >> 12); \
    uint pa = ident ? (va) : (idx_uint4(pages, ok##_slot) | ((va) & 0xfff)); \
    [branch] if (!ok) { \
        uint ok##_k = TLB2_IDX(mode, va); \
        if (tlb2_tag[ok##_k] == TLB2_TAG(va, xl_ctx)) { \
            uint ok##_pg = tlb2_pg[ok##_k]; \
            ok = true; \
            pa = ok##_pg | ((va) & 0xfff); \
            set_idx_uint4(vpns, (va) >> 12, ok##_slot); \
            set_idx_uint4(pages, ok##_pg, ok##_slot); \
        } \
    }
#endif

// Runs one instruction that cannot trap, touch memory or change what interrupts see (integer
// ALU, branches, jumps) without the general path. Only called while irq_quiet holds, so the
// general path's interrupt/UART step would have done nothing. Returns false, having changed
// nothing, for anything else.
bool fast_exec(uint w) {
    uint opc = w & 0x7f;
    uint f3 = (w >> 12) & 0x7;
    uint rd = (w >> 7) & 0x1f;
    // rs1 and rs2 sit at the same bits in every format, so both registers are read before the
    // opcode is known: the reads overlap the decode instead of following it.
    uint rs1v = xreg((w >> 15) & 0x1f);
    uint rs2v = xreg((w >> 20) & 0x1f);
    uint pc = cpu.pc;
    uint npc = pc + 4;
    uint val = 0;
    bool wr = true;
    bool ok = true;
    [forcecase]
    switch (opc) {
        case 0x37: val = w & 0xfffff000; break;                 // lui
        case 0x17: val = pc + (w & 0xfffff000); break;          // auipc
        case 0x6f: {                                            // jal
            FormatJ j = parse_FormatJ(w);
            val = pc + 4;
            npc = pc + j.imm;
            break;
        }
        case 0x67: {                                            // jalr
            FormatI i = parse_FormatI(w);
            ok = f3 == 0;
            val = pc + 4;
            npc = rs1v + i.imm;
            break;
        }
        case 0x13: {                                            // op-imm
            FormatI i = parse_FormatI(w);
            uint a = rs1v;
            uint b = i.imm;
            uint sh = (w >> 20) & 0x1f;
            uint f6 = w >> 26;
            uint f_sra = (a & 0x80000000) ? ~(~a >> sh) : a >> sh;
            [forcecase]
            switch (f3) {
                case 0: val = a + b; break;
                case 1: val = a << sh; ok = f6 == 0; break;
                case 2: val = AS_SIGNED(a) < AS_SIGNED(b) ? 1 : 0; break;
                case 3: val = a < b ? 1 : 0; break;
                case 4: val = a ^ b; break;
                case 5: val = f6 == 0x10 ? f_sra : a >> sh; ok = f6 == 0 || f6 == 0x10; break;
                case 6: val = a | b; break;
                default: val = a & b; break;
            }
            break;
        }
        case 0x33: {                                            // op (M extension: general path)
            uint a = rs1v;
            uint b = rs2v;
            uint f7 = w >> 25;
            uint f_sra = (a & 0x80000000) ? ~(~a >> b) : a >> b;
            ok = f7 == 0 || (f7 == 0x20 && (f3 == 0 || f3 == 5));
            [forcecase]
            switch (f3) {
                case 0: val = f7 ? a - b : a + b; break;
                case 1: val = a << b; break;
                case 2: val = AS_SIGNED(a) < AS_SIGNED(b) ? 1 : 0; break;
                case 3: val = a < b ? 1 : 0; break;
                case 4: val = a ^ b; break;
                case 5: val = f7 ? f_sra : a >> b; break;
                case 6: val = a | b; break;
                default: val = a & b; break;
            }
            break;
        }
        case 0x63: {                                            // branches
            uint a = rs1v;
            uint b = rs2v;
            bool lt = AS_SIGNED(a) < AS_SIGNED(b), ltu = a < b;
            bool taken = f3 == 0 ? a == b : f3 == 1 ? a != b : f3 == 4 ? lt : f3 == 5 ? !lt : f3 == 6 ? ltu : !ltu;
            FormatB br = parse_FormatB(w);
            ok = f3 != 2 && f3 != 3;
            wr = false;
            if (taken) { npc = pc + br.imm; }
            break;
        }
        case 0x03: {                                            // loads from RAM, translation available
            FormatI i = parse_FormatI(w);
            uint va = rs1v + i.imm;
            FAST_XL(xl_ident_d, MMU_ACCESS_READ, tlb_r_vpn, tlb_r_page, va, t_ok, pa)
            ok = false;
            // va == 0 is left to the general path, which treats it specially
            [branch]
            if (va != 0 && (f3 < 3 || f3 == 4 || f3 == 5) && t_ok && (pa & 0x80000000) != 0) {
                {
                    // Same two-word read as the general path: the second word is read whenever
                    // the address is not word aligned, at physical address + 4.
                    uint off = pa & 0x3;
                    uint a1 = pa & 0x7ffffffc;
                    uint a2 = (pa & ~0x3) + 4;
                    uint w1 = 0, w2 = 0;
                    [branch]
                    if (a1 < RAM_MAX) {
                        w1 = mem_get_cached_or_tex(a1);
                    }
                    [branch]
                    if (off != 0 && (a2 & 0x80000000) != 0 && (a2 & 0x7fffffff) < RAM_MAX) {
                        w2 = mem_get_cached_or_tex(a2 & 0x7fffffff);
                    }
                    uint v = off != 0 ? ((w1 >> (off * 8)) | (w2 << ((4 - off) * 8))) : w1;
                    prepared_mem_val = v;
                    val = f3 == 0 ? sign_extend(v & 0xff, 8) : f3 == 1 ? sign_extend(v & 0xffff, 16) :
                          f3 == 2 ? v : f3 == 4 ? v & 0xff : v & 0xffff;
                    ok = true;
                }
            }
            break;
        }
        case 0x23: {                                            // sb/sh/sw to RAM, translation available
            FormatS s;
            s.rs2 = (w >> 20) & 0x1f;
            s.addr = rs1v + ((w & 0x80000000 ? 0xfffff000 : 0) | ((w >> 20) & 0xfe0) | ((w >> 7) & 0x1f));
            FAST_XL(xl_ident_d, MMU_ACCESS_WRITE, tlb_w_vpn, tlb_w_page, s.addr, t_ok, pa)
            ok = false;
            wr = false;
            [branch]
            if (f3 < 3 && t_ok && (pa & 0x80000000) != 0) {
                {
                    mem_set(pa, rs2v, 1u << f3);          // may set cpu.stall when the cache is full
                    ok = true;
                }
            }
            break;
        }
        default:
            ok = false;
            break;
    }
    if (!ok) {
        return false;
    }
    xr[(wr && rd != 0) ? rd : 32] = val;   // branch-free: discarded writes go to the scratch slot
    cpu.debug_last_ins = w;
    // cpu.clint.mtime and cpu.debug_do_tick already hold this pass's values: the first tick of
    // every pass takes the general path (irq_quiet starts false), which sets both.
    cpu.pc = npc;
    return true;
}
#undef rs1v
#undef rs2v

void emulate() {
    uint ins_word = 0;
    ins_ret ret = ins_ret_noop();
    bool mtip_reset = false;
    if ((cpu.pc & 0x3) == 0) {
        // Same page as the last translated fetch: reuse it without re-checking mode and privilege.
        uint ins_addr = 0;
        if (pre_valid) {
            // cpu_tick already fetched this instruction through the same check
        } else
#ifdef NO_PAGING
        {
            ins_addr = cpu.pc;
        }
#else
        if ((cpu.pc >> 12) == fetch_vpn) {
            PROF(PROF_fast_fetch)
            ins_addr = fetch_page | (cpu.pc & 0xfff);
        } else {
            ins_addr = mmu_translate(ret, cpu.pc, MMU_ACCESS_FETCH);
        }
#endif

        if (!ret.trap.en) {
            if (pre_valid) { ins_word = pre_word; } else { ins_word = mem_get_instruction(ins_addr); }
            cpu.debug_last_ins = ins_word;

            ret = ins_select(ins_word, ret);

            if (ret.csr_write && !ret.trap.en) {
                set_csr(ret.csr_write, ret.csr_val, ret);

                if (cpu.stall == STALL_MEMOP_COPY || cpu.stall == STALL_MEMOP_FILL) {
                    [loop]
                    for (uint imc = cpu.stall == STALL_MEMOP_FILL ? 1 : 0; imc < 2; imc++) {   // a fill has no source address
                        uint phys = mmu_translate(ret,
                            imc == 0 ? cpu.memop_src_v : cpu.memop_dst_v,
                            imc == 0 ? MMU_ACCESS_READ : MMU_ACCESS_WRITE);
                        if (ret.trap.en) break;
                        if (imc == 0) {
                            cpu.memop_src_p = phys;
                        } else {
                            // Note: This only allows targets within RAM
                            cpu.memop_dst_p = phys & 0x7fffffff;
                        }
                    }
                }
            }

            if (ret.mem_wr_size && !ret.trap.en) {
                uint write_addr = mmu_translate(ret, ret.mem_wr_addr, MMU_ACCESS_WRITE);
                if (!ret.trap.en) {
                    mem_set(write_addr, ret.mem_wr_value, ret.mem_wr_size / 8);

                    // MTIP (machine timer interrupt pending) resets when mtimecmp is written
                    if (write_addr == 0x02004000) {
                        mtip_reset = true;
                    }
                }
            }

            if (!ret.trap.en && ret.write_reg) {
                xr[ret.write_reg] = ret.write_val;
            }
        }
    } else {
        ret.trap.en = true;
        ret.trap.type = trap_InstructionAddressMisaligned;
        ret.trap.value = cpu.pc;
    }

    // handle CLINT IRQs
    cpu.clint.mtime_lo = pass_mtime_lo;
    cpu.clint.mtime_hi = pass_mtime_hi;

    // Nothing below can have an effect while irq_quiet holds, no trap is pending and this is
    // not a UART input poll tick.
    if (irq_quiet && !ret.trap.en && (cpu.clock & 0xff) != 0xff) {
        PROF(PROF_irq_gated)
        cpu.pc = ret.pc_val;
        return;
    }

    uint mip_override = read_mip();
    if (cpu.clint.msip) {
        mip_override |= MIP_MSIP;
    }

    if ((cpu.clint.mtimecmp_lo != 0 || cpu.clint.mtimecmp_hi != 0) && (cpu.clint.mtime_hi > cpu.clint.mtimecmp_hi || (cpu.clint.mtime_hi == cpu.clint.mtimecmp_hi && cpu.clint.mtime_lo >= cpu.clint.mtimecmp_lo))) {
        mip_override |= MIP_MTIP;
    }
    [flatten]
    if (mtip_reset) {
        mip_override &= ~MIP_MTIP;
    }

    uart_tick();
    //if (cpu.uart.interrupting) {
    //    mip_override |= MIP_SEIP;
    //}

    /* if (ret.trap.en && cpu.clock <= 1) { */
    /*     cpu.stall = STALL_ILLEGAL_ENTRY_POINT; */
    /*     cpu.stall_count = ins_word; */
    /*     cpu.trap_count = cpu.pc; */
    /* } */

    bool e_no_trap = !ret.trap.en;

    // will write CSR_MIP if necessary
    handle_irq_and_trap(ret, mip_override);

    // If this step took no trap, repeating it with the same inputs does nothing: either no
    // interrupt is pending-and-enabled, or one is pending but masked, in which case the only
    // effect was writing mip, and writing the same value again changes nothing. Every input
    // (CSRs, CLINT, privilege) clears the flag when it changes.
    irq_quiet = e_no_trap && !irq_last_handled && cpu.stall == 0;

    /* if (ret.pc_val & 0x3) { */
    /*     cpu.debug_arb_0 = ret.pc_val; */
    /*     cpu.debug_arb_1 = ret.trap.en; */
    /*     cpu.debug_arb_2 = ret.trap.type; */
    /*     cpu.debug_arb_3 = ret.trap.value; */
    /*     cpu.debug_do_tick = 0xffffffff; */
    /*     _DoTick = cpu.debug_do_tick; */
    /*     cpu.stall = STALL_FENCE; */
    /* } */

    // ret.pc_val should be set to pc+4 by default
    cpu.pc = ret.pc_val;

#ifndef NO_PAGING
    {
        uint xs, xm;
        uint xp = get_effective_privilege(xs, xm);
        xl_ctx = xp | (xs << 2) | (xm << 3);
    }
    {
        bool machine = cpu.csr.privilege == PRIV_MACHINE;
        uint ms = read_mstatus();
        xl_ident_f = cpu.mmu.mode == MMU_MODE_OFF || machine;
        xl_ident_d = cpu.mmu.mode == MMU_MODE_OFF || (machine && (((ms >> 17) & 0x1) == 0 || ((ms >> 11) & 0x3) == PRIV_MACHINE));
    }
#endif
}

#endif
