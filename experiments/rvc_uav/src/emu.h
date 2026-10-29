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
    // unnecessary? (a worker core's way to park itself until its next start: mc.h)
    MC_PARK(MC_PARKED_EBREAK)
})
static uint pass_mtime_lo, pass_mtime_hi;
#ifdef CSR_FIELDS
// The supervisor's timer interrupt is pending from the pass its deadline is reached in until
// the next deadline is set; the time is the same for a whole pass.
bool timer_due() {
    return (cpu.clint.mtimecmp_lo != 0 || cpu.clint.mtimecmp_hi != 0) && (pass_mtime_hi > cpu.clint.mtimecmp_hi || (pass_mtime_hi == cpu.clint.mtimecmp_hi && pass_mtime_lo >= cpu.clint.mtimecmp_lo));
}
#define TIMER_PREPARE { cpu.clint.mtime_lo = pass_mtime_lo; cpu.clint.mtime_hi = pass_mtime_hi; if (timer_due()) cpu.mip |= MIP_STIP; }
#else
#define TIMER_PREPARE
#endif
#ifdef SBI_HLE
// The supervisor's calls to firmware (SBI), answered by the machine itself: with SBI_HLE it
// starts the kernel in supervisor mode and runs no machine-mode firmware (docs/machine/boot.md).
void sbi_call() {
    uint ext = xr[17], a0 = xr[10];
    uint error = 0, value = 0;
#ifdef CSR_FIELDS
    if (ext == 0x54494D45 || ext == 0) {
        cpu.clint.mtimecmp_lo = a0;
        cpu.clint.mtimecmp_hi = xr[11];
        cpu.mip = timer_due() ? cpu.mip | MIP_STIP : cpu.mip & ~MIP_STIP;
    }   // (the console is memory the kernel writes itself: docs/machine/console.md)
#else
    if (ext == 0x54494D45 || ext == 0) {
        // set_timer: the timer interrupt stays pending until the next deadline is set
        cpu.clint.mtimecmp_lo = a0;
        cpu.clint.mtimecmp_hi = xr[11];
        write_csr_raw(CSR_MIP, read_mip() & ~MIP_STIP);
    } else if (ext == 1) {
        put_byte_to_fb(a0 & 0xff);
    } else if (ext == 2) {
        value = UART_GET1(RBR);
        if (value != 0) {
            UART_SET1(RBR, 0);
            UART_SET2(LSR, (UART_GET2(LSR) & ~LSR_DATA_AVAILABLE));
            uart_update_iir();
        } else {
            value = 0xffffffff;
        }
    }
#endif
    else if (ext == 0x10) {
        // base: specification 0.2, an implementation of our own, and which extensions exist
        uint fn = xr[16];
        value = fn == 0 ? 2 : fn == 1 ? 0x454d55 : fn == 2 ? 1 : (fn == 3 && (a0 == 0x54494D45 || a0 <= 2)) ? 1 : 0;
    } else {
        error = 0xfffffffe;   // not supported
    }
    // the legacy calls (below 0x10) return one value; the others an error and a value
    if (ext < 0x10) {
        xr[10] = value;
    } else {
        xr[10] = error;
        xr[11] = value;
    }
    irq_quiet = false;
}
#endif

// Not through DEF: conditional compilation inside a macro argument is not portable.
void emu_ecall(uint ins_word, inout ins_ret ret, FormatEmpty ins) { // system
#ifdef SBI_HLE
    if (cpu.csr.privilege == PRIV_SUPERVISOR) {
        sbi_call();
        return;
    }
#endif
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
}
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
// Not through DEF: conditional compilation inside a macro argument is not portable.
void emu_mret(uint ins_word, inout ins_ret ret, FormatEmpty ins) { // system
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
}
DEF(mul, FormatR, { // rv32m
    uint tmp = AS_SIGNED(xreg(ins.rs1)) * AS_SIGNED(xreg(ins.rs2));
    WR_RD(tmp)
})
// Exact high word of a 32x32 multiply from 16-bit partial products, for hosts whose double
// support cannot be trusted. Upstream's double version rounds once the product needs more
// than 53 bits, so results differ from it there.
//
//
// The GPU has an instruction for this (umul and imul return both halves of the product), and
// fxc2 can be asked for it (mulhi()). It is not used: Microsoft's compiler never writes it
// with a high result alone, and what turns the bytecode into SPIR-V on Linux (dxbc-spirv, in
// DXVK and vkd3d-proton) made an invalid module of it, which crashed VRChat there
// (docs/world/linux.md). It was worth nothing that could be measured.
uint mulhu32(uint a, uint b) {
    uint al = a & 0xffff, ah = a >> 16, bl = b & 0xffff, bh = b >> 16;
    uint lh = al * bh, hl = ah * bl;
    uint mid = ((al * bl) >> 16) + (lh & 0xffff) + (hl & 0xffff);
    return ah * bh + (lh >> 16) + (hl >> 16) + (mid >> 16);
}
// the high word of the signed product
uint mulhs32(uint a, uint b) {
    return mulhu32(a, b) - ((a >> 31) ? b : 0) - ((b >> 31) ? a : 0);
}
DEF(mulh, FormatR, { // rv32m
    uint a = xreg(ins.rs1);   // one declaration each: Unity's preprocessor splits macro arguments at this comma
    uint b = xreg(ins.rs2);
    WR_RD(mulhs32(a, b))
})
DEF(mulhsu, FormatR, { // rv32m
    uint a = xreg(ins.rs1);   // one declaration each: Unity's preprocessor splits macro arguments at this comma
    uint b = xreg(ins.rs2);
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
    // rs1 names the one page to flush, x0 means all of them (rs2, the address space, is ignored:
    // flushing more than asked is allowed)
    uint sf_rs1 = (ins_word >> 15) & 0x1f;
    if (sf_rs1 != 0) {
        tlb2_flush_page(xreg(sf_rs1));
    } else {
        tlb2_flush();
    }
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


#ifdef FPU
// The F extension (docs/machine/fpu.md): an instruction of opcode 0x53 on two register values, for the
// fast step and the general path alike. Every result is worked out and one is chosen.
// to_float says which register file the result is for; ok is false for an encoding that
// does not exist.
uint fp_exec(uint w, uint ra, uint rb, out bool to_float, out bool ok) {
    uint f7 = w >> 25, f3 = (w >> 12) & 7, sel = (w >> 20) & 0x1f;
    precise float a = asfloat(ra);
    precise float b = asfloat(rb);
    bool a_nan = (ra & 0x7fffffff) > 0x7f800000, b_nan = (rb & 0x7fffffff) > 0x7f800000;
    precise float sum = a + b;
    precise float dif = a - b;
    precise float prod = a * b;
    // The card divides by multiplying with 1 / b, which is nothing when b is large and makes
    // a quotient of two large numbers zero. So two ordinary numbers are divided with their
    // exponents moved towards the middle, the difference shared between them.
    uint ea = (ra >> 23) & 0xff, eb = (rb >> 23) & 0xff;
    int de = (int)ea - (int)eb;
    int ha = clamp(de, -126, 127);
    precise float da = asfloat((ra & 0x807fffff) | ((uint)(127 + ha) << 23));
    precise float db = asfloat((rb & 0x807fffff) | ((uint)(127 - (de - ha)) << 23));
    precise float quot = (ea - 1 < 254 && eb - 1 < 254) ? da / db : a / b;
    precise float root = sqrt(a);
    uint arith = asuint(f7 == 0x00 ? sum : f7 == 0x04 ? dif : f7 == 0x08 ? prod : f7 == 0x0c ? quot : root);
    if ((arith & 0x7fffffff) > 0x7f800000) arith = 0x7fc00000;   // the one NaN RISC-V makes
    // fsgnj, fsgnjn, fsgnjx: the first operand with another sign
    uint sgn = (ra & 0x7fffffff) | ((f3 == 0 ? rb : f3 == 1 ? ~rb : ra ^ rb) & 0x80000000);
    // fmin, fmax: a NaN loses to a number, and -0 is less than +0
    bool less = a < b || (ra == 0x80000000 && rb == 0);
    uint pick = a_nan ? (b_nan ? 0x7fc00000 : rb) : b_nan ? ra : ((less == (f3 == 0)) ? ra : rb);
    // feq, flt, fle: false when either is a NaN
    uint cmp = (!a_nan && !b_nan && (f3 == 2 ? a == b : f3 == 1 ? a < b : a <= b)) ? 1 : 0;
    // fcvt.w.s, fcvt.wu.s: rounded as the instruction says (a C cast truncates), then clamped
    float whole = f3 == 1 ? trunc(a) : f3 == 2 ? floor(a) : f3 == 3 ? ceil(a) : round(a);
    uint to_s = (a_nan || whole >= 2147483648.0) ? 0x7fffffff : whole <= -2147483648.0 ? 0x80000000 : AS_UNSIGNED((int)whole);
    // (from 2^31 up in two parts, and a signed integer by its size and its sign: fxc2 made the
    // first conversion a signed one and the second no conversion at all)
    uint to_u = (a_nan || whole >= 4294967296.0) ? 0xffffffff : whole <= 0.0 ? 0
              : whole >= 2147483648.0 ? 0x80000000 + (uint)(whole - 2147483648.0) : (uint)whole;
    // fcvt.s.w, fcvt.s.wu
    bool from_neg = sel == 0 && (ra >> 31) != 0;
    uint from = asuint((float)(from_neg ? 0 - ra : ra)) | (from_neg ? 0x80000000 : 0);
    // fclass
    uint ex = (ra >> 23) & 0xff, man = ra & 0x7fffff;
    bool neg = (ra >> 31) != 0;
    uint cls = ex == 0xff ? (man != 0 ? ((man & 0x400000) ? 0x200 : 0x100) : (neg ? 0x001 : 0x080))
             : ex == 0 ? (man != 0 ? (neg ? 0x004 : 0x020) : (neg ? 0x008 : 0x010))
             : (neg ? 0x002 : 0x040);

    bool is_arith = (f7 & 0x73) == 0 || f7 == 0x2c;   // 0x00, 0x04, 0x08, 0x0c, and the root
    to_float = is_arith || f7 == 0x10 || f7 == 0x14 || f7 == 0x68 || f7 == 0x78;
    ok = is_arith ? (f7 != 0x2c || sel == 0)
       : f7 == 0x10 ? f3 < 3
       : f7 == 0x14 ? f3 < 2
       : f7 == 0x50 ? f3 < 3
       : (f7 == 0x60 || f7 == 0x68) ? sel < 2
       : f7 == 0x70 ? (sel == 0 && f3 < 2)
       : (f7 == 0x78 && sel == 0 && f3 == 0);
    return is_arith ? arith
         : f7 == 0x10 ? sgn
         : f7 == 0x14 ? pick
         : f7 == 0x50 ? cmp
         : f7 == 0x60 ? (sel == 0 ? to_s : to_u)
         : f7 == 0x68 ? from
         : f7 == 0x70 ? (f3 == 0 ? ra : cls)
         : ra;                                        // fmv.w.x
}
// The same for funct7 0x10, 0x14, 0x50, 0x78 and fmv.x.w alone (signs, the lesser and the
// greater, comparisons, moves): fp_exec's results for them, and none of its other work.
uint fp_light(uint w, uint ra, uint rb, out bool to_float, out bool ok) {
    uint f7 = w >> 25, f3 = (w >> 12) & 7, sel = (w >> 20) & 0x1f;
    precise float a = asfloat(ra);
    precise float b = asfloat(rb);
    bool a_nan = (ra & 0x7fffffff) > 0x7f800000, b_nan = (rb & 0x7fffffff) > 0x7f800000;
    uint sgn = (ra & 0x7fffffff) | ((f3 == 0 ? rb : f3 == 1 ? ~rb : ra ^ rb) & 0x80000000);
    bool less = a < b || (ra == 0x80000000 && rb == 0);
    uint pick = a_nan ? (b_nan ? 0x7fc00000 : rb) : b_nan ? ra : ((less == (f3 == 0)) ? ra : rb);
    uint cmp = (!a_nan && !b_nan && (f3 == 2 ? a == b : f3 == 1 ? a < b : a <= b)) ? 1 : 0;
    to_float = f7 == 0x10 || f7 == 0x14 || f7 == 0x78;
    ok = f7 == 0x10 ? f3 < 3
       : f7 == 0x14 ? f3 < 2
       : f7 == 0x50 ? f3 < 3
       : (sel == 0 && f3 == 0);
    return f7 == 0x10 ? sgn : f7 == 0x14 ? pick : f7 == 0x50 ? cmp : ra;
}
#endif

#ifdef ZB
// Zba and Zbb (RV32) on op and op-imm: true with the result in r for one of them.
bool zb_exec(uint w, uint a, uint b, inout uint r) {
    uint f7 = w >> 25, f3 = (w >> 12) & 7, s = (w >> 20) & 0x1f;
    bool ok = true;
    [branch]
    if ((w & 0x20) != 0) {
        if (f7 == 0x10 && f3 != 0 && (f3 & 1) == 0) r = (a << (f3 >> 1)) + b;
        else if (f7 == 0x20 && f3 == 7) r = a & ~b;
        else if (f7 == 0x20 && f3 == 6) r = a | ~b;
        else if (f7 == 0x20 && f3 == 4) r = ~(a ^ b);
        else if (f7 == 0x05 && (f3 & 4) != 0) {
            uint t = (f3 & 1) ? 0 : 0x80000000;
            bool lt = (a ^ t) < (b ^ t);
            r = (((f3 & 2) != 0) != lt) ? a : b;
        }
        else if (f7 == 0x30 && f3 == 1) r = (a << (b & 31)) | (a >> ((32 - (b & 31)) & 31));
        else if (f7 == 0x30 && f3 == 5) r = (a >> (b & 31)) | (a << ((32 - (b & 31)) & 31));
        else if (f7 == 0x04 && f3 == 4 && s == 0) r = a & 0xffff;
        else ok = false;
    } else if (f7 == 0x30 && f3 == 1) {
        uint m = a | (a >> 1); m |= m >> 2; m |= m >> 4; m |= m >> 8; m |= m >> 16;
        if (s == 0) r = 32 - countbits(m);
        else if (s == 1) r = countbits((a & (0 - a)) - 1);
        else if (s == 2) r = countbits(a);
        else if (s == 4) r = AS_UNSIGNED(AS_SIGNED(a << 24) >> 24);
        else if (s == 5) r = AS_UNSIGNED(AS_SIGNED(a << 16) >> 16);
        else ok = false;
    } else if (f7 == 0x30 && f3 == 5) r = (a >> s) | (a << ((32 - s) & 31));
    else if ((w >> 20) == 0x287 && f3 == 5) {
        r = ((a & 0xff) ? 0xff : 0) | ((a & 0xff00) ? 0xff00 : 0) | ((a & 0xff0000) ? 0xff0000 : 0) | ((a & 0xff000000) ? 0xff000000 : 0);
    }
    else if ((w >> 20) == 0x698 && f3 == 5) r = (a >> 24) | ((a >> 8) & 0xff00) | ((a << 8) & 0xff0000) | (a << 24);
    else ok = false;
    return ok;
}
#endif

#define RUN(name, data, insf) case data : { \
    PROF(PROF_ins_##name) \
    emu_##name(ins_word, ret, insf); \
    return ret; \
}
ins_ret ins_select_l1(L1P uint ins_word, inout ins_ret ret) {
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
#ifdef FPU
    if ((ins_word & 0x0000707f) == 0x00002007 || (ins_word & 0x0000007f) == 0x00000003) {
        // lX memory read, and flw
#else
    if ((ins_word & 0x0000007f) == 0x00000003) {
        // lX memory read
#endif
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

#ifdef FPU
    if ((ins_word & 0x0000707f) == 0x00002007) {            // flw: the word read above, as it is
        ret.write_reg = XR_F + ((ins_word >> 7) & 0x1f);
        ret.write_val = prepared_mem_val;
        return ret;
    }
    if ((ins_word & 0x0000707f) == 0x00002027) {            // fsw
        ret.mem_wr_addr = ins_FormatS.addr; ret.mem_wr_size = 32; ret.mem_wr_value = xr[XR_F + ins_FormatS.rs2];
        return ret;
    }
    if ((ins_word & 0x06000073) == 0x00000043) {
        // fmadd.s, fmsub.s, fnmsub.s, fnmadd.s: a product and a sum, each rounded (a real one
        // rounds once). Three registers, so not for the fast step: compilers are told not to
        // make them (-ffp-contract=off) and they are here for code that asks by name.
        precise float fm_a = asfloat(xr[XR_F + ins_FormatR.rs1]);
        precise float fm_b = asfloat(xr[XR_F + ins_FormatR.rs2]);
        precise float fm_c = asfloat(xr[XR_F + ins_FormatR.rs3]);
        precise float fm_p = fm_a * fm_b;
        uint fm_kind = (ins_word >> 2) & 3;
        precise float fm_r = fm_kind == 0 ? fm_p + fm_c : fm_kind == 1 ? fm_p - fm_c : fm_kind == 2 ? fm_c - fm_p : (-fm_p) - fm_c;
        uint fm_bits = asuint(fm_r);
        ret.write_reg = XR_F + ins_FormatR.rd;
        ret.write_val = (fm_bits & 0x7fffffff) > 0x7f800000 ? 0x7fc00000 : fm_bits;
        return ret;
    }
    if ((ins_word & 0x0000007f) == 0x00000053) {
        bool fp_to_float, fp_ok;
        bool fp_int_source = ((ins_word >> 25) & 0x68) == 0x68;   // fcvt.s.w and fmv.w.x take an integer
        uint fp_val = fp_exec(ins_word, xr[ins_FormatR.rs1 + (fp_int_source ? 0 : XR_F)], xr[XR_F + ins_FormatR.rs2], fp_to_float, fp_ok);
        if (fp_ok) {
            // (an integer result for x0 goes nowhere: write_reg 0 means no write)
            ret.write_reg = fp_to_float ? XR_F + ins_FormatR.rd : ins_FormatR.rd;
            ret.write_val = fp_val;
            return ret;
        }
    }
#endif

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

#ifdef ZB
    if ((ins_word & 0x5f) == 0x13) {
        uint zr = 0;
        if (zb_exec(ins_word, xreg((ins_word >> 15) & 0x1f), xreg((ins_word >> 20) & 0x1f), zr)) {
            ret.write_reg = (ins_word >> 7) & 0x1f;
            ret.write_val = zr;
            return ret;
        }
    }
#endif
    PROF(PROF_illegal_ins)
    ret.trap.en = true;
    ret.trap.type = trap_IllegalInstruction;
    ret.trap.value = ins_word;
    return ret;
}


// _Time is constant for the whole pass, so the timer value is computed once.
#ifdef NO_DOUBLES
uniform uint _HostMtimeLo, _HostMtimeHi;  // the same value, computed by the host
void time_prepare() {
    pass_mtime_lo = _HostMtimeLo;
    pass_mtime_hi = _HostMtimeHi;
    TIMER_PREPARE
}
#else
void time_prepare() {
    double mtime = (double)_Time.x * 1000000.0 * 0.1;
    pass_mtime_lo = (uint)(floor(glsl_mod(mtime, 4294967296.0)));
    pass_mtime_hi = (uint)(mtime / 4294967296.0);
    TIMER_PREPARE
}
#endif

// The instruction word cpu_tick already fetched for this tick, if any.
static bool pre_valid = false;
static bool fast_same = false;   // the last fast instruction's next one is the next word of its texel
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
        uint ok##_pg = tlb2_pg[ok##_k]; \
        bool ok##_l2 = TLB2_HIT(ok##_k, TLB2_TAG(va, xl_ctx)); \
        [branch] if (!ok##_l2) { \
            if (tlb2_saved(ok##_k, TLB2_TAG(va, xl_ctx), ok##_pg)) { \
                tlb2_tag[ok##_k] = TLB2_TAG(va, xl_ctx); \
                tlb2_pg[ok##_k] = ok##_pg; \
                TLB2_OCC_SET(ok##_k) \
                ok##_l2 = true; \
            } \
        } \
        if (ok##_l2) { \
            ok = true; \
            pa = ok##_pg | ((va) & 0xfff); \
            set_idx_uint4(vpns, (va) >> 12, ok##_slot); \
            set_idx_uint4(pages, ok##_pg, ok##_slot); \
        } else { \
            uint ok##_m = TLBM_IDX(mode, va); \
            if (tlbm_tag[ok##_m] == TLBM_TAG(va, xl_ctx)) { \
                uint ok##_mp = tlbm_pg[ok##_m] | ((va) & 0x3ff000); \
                ok = true; \
                pa = ok##_mp | ((va) & 0xfff); \
                set_idx_uint4(vpns, (va) >> 12, ok##_slot); \
                set_idx_uint4(pages, ok##_mp, ok##_slot); \
            } \
        } \
    }
#endif

// fast_exec()'s kinds: op-imm and op, the M extension aside; flw, fsw and opcode 0x53
#define K_ALU ((w & 0x5f) == 0x13 && (w & 0xfe00007f) != 0x02000033)
#define K_FP ((w & 0x5f) == 0x07 || opc == 0x53)

// Runs one instruction that cannot trap, touch memory or change what interrupts see (integer
// ALU, branches, jumps) without the general path. Only called while irq_quiet holds, so the
// general path's interrupt/UART step would have done nothing. Returns false, having changed
// nothing, for anything else.
bool fast_exec_l1(L1P uint w) {
    uint opc = w & 0x7f;
    uint f3 = (w >> 12) & 0x7;
    uint rd = (w >> 7) & 0x1f;
    // rs1 and rs2 sit at the same bits in every format, so both registers are read before the
    // opcode is known: the reads overlap the decode instead of following it.
    // (Float instructions read their float operands themselves, in their own branch below:
    // choosing between the two register sets up here, and carrying flw and fsw in the integer
    // load and store, cost every instruction 5% for the sake of the few that are float.)
    uint rs1v = xreg((w >> 15) & 0x1f);
    uint rs2v = xreg((w >> 20) & 0x1f);
    uint pc = cpu.pc;
    uint npc = pc + 4;
    uint val = 0;
    bool wr = true;
    bool ok = true;
    // A switch is the costly thing here, and a test whose block is large the next: arithmetic
    // computes every result and selects one, and the kinds are told apart by a tree of tests
    // (arithmetic, loads, stores and floats a group, in it arithmetic and loads one). One after
    // another they cost each kind after the first two more (docs/machine/tick-costs.md).
    [branch]
#ifdef FPU
    if (K_ALU || opc == 0x03 || opc == 0x23 || K_FP) {
#else
    if (K_ALU || opc == 0x03 || opc == 0x23) {
#endif
        [branch]
        if (K_ALU || opc == 0x03) {
            [branch]
            if (K_ALU) {
                // bit 5 tells op-imm and op apart
                PROF(PROF_x_alu)
                bool reg = (w & 0x20) != 0;
                bool alt = (w & 0x40000000) != 0;   // subtract, or shift right arithmetic
                uint a = rs1v;
                uint imm = AS_UNSIGNED(AS_SIGNED(w) >> 20);
                uint b = reg ? rs2v : imm;
                uint sh = b & 0x1f;
                uint srl = a >> sh;
                uint sra = AS_UNSIGNED(AS_SIGNED(a) >> sh);
                uint r01 = (f3 & 1) ? a << sh : ((reg && alt) ? a - b : a + b);
                // signed is unsigned with the signs turned over
                uint turn = (f3 & 1) ? 0 : 0x80000000;
                uint r23 = (a ^ turn) < (b ^ turn) ? 1 : 0;
                uint r45 = (f3 & 1) ? (alt ? sra : srl) : a ^ b;
                uint r67 = (f3 & 1) ? a & b : a | b;
                val = (f3 & 4) ? ((f3 & 2) ? r67 : r45) : ((f3 & 2) ? r23 : r01);
                // Which of these exist: a bit for each of op (8 + funct3) and op-imm (funct3), by
                // funct7. With 0 all of them; with 0x20 sub and sra, srai, and every op-imm that is
                // not a shift (its funct7 is part of the number); with anything else only those.
                uint f7 = w >> 25;
                uint exist = f7 == 0 ? 0xffffu : f7 == 0x20 ? 0x21fdu : 0x00ddu;
                ok = ((exist >> (((w >> 2) & 8) | f3)) & 1) != 0;
#ifdef ZB
                [branch] if (!ok) ok = zb_exec(w, rs1v, rs2v, val);
#endif
            } else {                                                    // loads from RAM, translation available
                PROF(PROF_x_load)
                FormatI i = parse_FormatI(w);
                uint va = rs1v + i.imm;
                FAST_XL(xl_ident_d, MMU_ACCESS_READ, tlb_r_vpn, tlb_r_page, va, t_ok, pa)
                ok = false;
                // va == 0 is left to the general path, which treats it specially; so is a load that
                // straddles two words, which is rare and would double the code on this path
                uint off = pa & 0x3;
                [branch]
                // (funct3 0, 1, 2, 4 and 5 are loads, a bit each; and RAM is what is under RAM_MAX
                // once the address's top bit is turned over)
                if (va != 0 && ((0x37u >> f3) & 1) != 0 && t_ok && off + (1u << (f3 & 3)) <= 4 && (pa ^ 0x80000000) < RAM_MAX) {
                    uint v = mem_get_cached_or_tex(pa & 0x7ffffffc) >> (off * 8);
                    // a byte or a half to the top of the word, and back down with or without its sign
                    uint up = (f3 & 3) == 0 ? 24 : (f3 & 3) == 1 ? 16 : 0;
                    v <<= up;
                    val = (f3 & 4) ? v >> up : AS_UNSIGNED(AS_SIGNED(v) >> up);
                    ok = true;
                }
            }
        } else {
            // sb/sh/sw to RAM, translation available; and the F extension, all of it here: its
            // results go to the float registers from here too
#ifdef FPU
            [branch]
            if (opc == 0x23) {
#endif
                PROF(PROF_x_store)
                FormatS s;
                s.rs2 = (w >> 20) & 0x1f;
                s.addr = rs1v + ((AS_UNSIGNED(AS_SIGNED(w) >> 20) & 0xffffffe0) | ((w >> 7) & 0x1f));
                FAST_XL(xl_ident_d, MMU_ACCESS_WRITE, tlb_w_vpn, tlb_w_page, s.addr, t_ok, pa)
                ok = false;
                wr = false;
                // within one RAM word: a single read-modify-write (may set cpu.stall when the
                // cache is full). A store that straddles two words takes the general path.
                uint s_off = pa & 0x3;
                [branch]
                if (f3 < 3 && t_ok && (pa & 0x80000000) != 0 && s_off + (1u << f3) <= 4) {
                    uint s_mask = (f3 == 2 ? 0xffffffff : f3 == 1 ? 0xffff : 0xff) << (s_off * 8);
                    mem_set_ram(pa & 0x7ffffffc, rs2v << (s_off * 8), s_mask, false);
                    ok = true;
                }
#ifdef FPU
            } else {
                bool to_f = false;
                [branch]
                if (opc == 0x53) {
                    uint fp_a = rs1v;       // an integer being converted or moved, else a float register
                    [branch]
                    if (((w >> 25) & 0x68) != 0x68) {
                        fp_a = xr[XR_F + ((w >> 15) & 0x1f)];
                    }
                    // A sum, a difference or a product, and the moves, signs and comparisons, are nearly
                    // all a program runs (Quake: 8 in 9): each by itself, fp_exec's every result for the rest.
                    uint fp_b = xr[XR_F + ((w >> 20) & 0x1f)];
                    uint fp7 = w >> 25;
                    [branch]
                    if ((fp7 & 0x73) == 0 && fp7 != 0x0c) {
                        precise float fa = asfloat(fp_a);
                        precise float fb = asfloat(fp_b);
                        precise float f_sum = fa + fb;
                        precise float f_dif = fa - fb;
                        precise float f_prod = fa * fb;
                        val = asuint(fp7 == 0x00 ? f_sum : fp7 == 0x04 ? f_dif : f_prod);
                        if ((val & 0x7fffffff) > 0x7f800000) val = 0x7fc00000;
                        to_f = true;
                    } else if (((0x50100030u >> (fp7 >> 2)) & 1) != 0 && (fp7 & 3) == 0 && (fp7 != 0x70 || f3 == 0)) {
                        val = fp_light(w, fp_a, fp_b, to_f, ok);
                    } else {
                        val = fp_exec(w, fp_a, fp_b, to_f, ok);
                    }
                } else if (opc == 0x07) {
                    uint f_va = rs1v + ((w & 0x80000000 ? 0xfffff800 : 0) | ((w >> 20) & 0x7ff));
                    FAST_XL(xl_ident_d, MMU_ACCESS_READ, tlb_r_vpn, tlb_r_page, f_va, fl_ok, fl_pa)
                    ok = false;
                    [branch]
                    if (f_va != 0 && f3 == 2 && fl_ok && (fl_pa & 0x80000003) == 0x80000000 && (fl_pa & 0x7ffffffc) < RAM_MAX) {
                        val = mem_get_cached_or_tex(fl_pa & 0x7ffffffc);
                        to_f = true;
                        ok = true;
                    }
                } else {
                    uint f_sa = rs1v + ((w & 0x80000000 ? 0xfffff000 : 0) | ((w >> 20) & 0xfe0) | ((w >> 7) & 0x1f));
                    FAST_XL(xl_ident_d, MMU_ACCESS_WRITE, tlb_w_vpn, tlb_w_page, f_sa, fs_ok, fs_pa)
                    ok = false;
                    wr = false;
                    [branch]
                    if (f3 == 2 && fs_ok && (fs_pa & 0x80000003) == 0x80000000) {
                        mem_set_ram(fs_pa & 0x7ffffffc, xr[XR_F + ((w >> 20) & 0x1f)], 0xffffffff, false);
                        ok = true;
                    }
                }
                xr[(ok && to_f) ? XR_F + ((w >> 7) & 0x1f) : 32] = val;   // (or to the scratch slot)
                wr = wr && !to_f;
            }
#endif
        }
    } else if (opc == 0x63) {                                           // branches
        uint a = rs1v;
        uint b = rs2v;
        bool lt = (f3 & 2) ? a < b : AS_SIGNED(a) < AS_SIGNED(b);
        bool taken = ((f3 & 4) ? lt : a == b) != ((f3 & 1) != 0);
        FormatB br = parse_FormatB(w);
        ok = (f3 & 6) != 2;   // (funct3 2 and 3 are no branch)
        wr = false;
        PROF(PROF_x_branch)
        if (taken) { PROF(PROF_x_taken) npc = pc + br.imm; }
#ifdef FPU
    } else if ((w & 0x04) != 0 && ((w & 0x08) == 0 || (w & 0x40) != 0) && (w & 0x5f) != 0x07) {
        // (flw and fsw, 0x07 and 0x27, have bit 2 as well: they are the floats' above)
#else
    } else if ((w & 0x04) != 0 && ((w & 0x08) == 0 || (w & 0x40) != 0)) {
#endif
        // lui, auipc, jal and jalr share opcode bit 2 (fence and the atomics, which also have
        // it, are excluded above); bit 6 is a jump, then bit 3 says jal and bit 5 says lui
        PROF(PROF_x_jump)
        bool jump = (w & 0x40) != 0, jal = (w & 0x08) != 0;
        FormatJ j = parse_FormatJ(w);
        FormatI i = parse_FormatI(w);
        val = jump ? pc + 4 : (w & 0xfffff000) + ((w & 0x20) ? 0 : pc);
        npc = jump ? (jal ? pc + j.imm : rs1v + i.imm) : npc;
        // of opcode bits 6..3 only 0110 (lui), 0010 (auipc), 1101 (jal) and 1100 (jalr) exist
        ok = (w & 3) == 3 && ((0x3044u >> ((w >> 3) & 15)) & 1) != 0 && (jal || !jump || f3 == 0);
#ifndef X_NO_FAST_AMO
    } else if (opc == 0x2f) {
        // atomics on a word of RAM the write TLB knows (a writable page is readable too):
        // amoadd, amoswap, lr, sc, amoxor, amoor, amoand. The rest take the general path.
        uint f5 = w >> 27;
        bool is_lr = f5 == 2, is_sc = f5 == 3;
        uint va = rs1v;
        FAST_XL(xl_ident_d, MMU_ACCESS_WRITE, tlb_w_vpn, tlb_w_page, va, t_ok, pa)
        ok = false;
        [branch]
        if (f3 == 2 && va != 0 && t_ok && (pa & 0x80000003) == 0x80000000 && (pa & 0x7ffffffc) < RAM_MAX &&
            f5 < 16 && ((0x111fu >> f5) & 1) != 0) {
            uint at = pa & 0x7ffffffc;
            bool sc_ok = cpu.reservation_en && cpu.reservation_addr == va;
            uint old = 0;
            [branch]
            if (!is_sc) {
                old = mem_get_cached_or_tex(at);
            }
            uint next = f5 == 0 ? old + rs2v : f5 == 4 ? old ^ rs2v : f5 == 8 ? old | rs2v : f5 == 12 ? old & rs2v : rs2v;
            [branch]
            if (is_sc ? sc_ok : !is_lr) {
                mem_set_ram(at, next, 0xffffffff, false);
            }
            if (is_lr) {
                cpu.reservation_en = true;
                cpu.reservation_addr = va;
            }
            if (is_sc && sc_ok) {
                cpu.reservation_en = false;
            }
            val = is_sc ? (sc_ok ? 0 : 1) : old;
            ok = true;
        }
#endif
    } else {
        [forcecase]
        switch (opc) {
            case 0x33: {                                            // the M extension (f7 == 1)
                uint a = rs1v;
                uint b = rs2v;
                // multiply and divide: the same results as the general path's routines
                bool by_zero = b == 0, wraps = a == 0x80000000 && b == 0xFFFFFFFF;
                uint safe = by_zero ? 1 : b;
                int sa = AS_SIGNED(a), sb = AS_SIGNED((by_zero || wraps) ? 1 : b);
                [forcecase]
                switch (f3) {
                    case 0: val = a * b; break;
                    case 1: val = mulhs32(a, b); break;
                    case 2: val = mulhu32(a, b) - ((a >> 31) ? b : 0); break;
                    case 3: val = mulhu32(a, b); break;
                    case 4: val = by_zero ? 0xFFFFFFFF : wraps ? a : AS_UNSIGNED(sa / sb); break;
                    case 5: val = by_zero ? 0xFFFFFFFF : a / safe; break;
                    case 6: val = by_zero ? a : wraps ? 0 : AS_UNSIGNED(sa % sb); break;
                    default: val = by_zero ? a : a % safe; break;
                }
                break;
            }
            default:
                ok = false;
                break;
        }
    }
    if (!ok) {
        return false;
    }
    // Work added to every instruction, to see what it costs (docs/machine/tick-costs.md, "What things
    // cost"): X_P_ALU=n is 3n operations each on the last one's result; X_P_READ=b four reads
    // of an array's elements chosen at random among its first 2^b; X_P_TEX=n so many texels
    // of RAM read and used at once.
#ifdef X_P_ALU
    { for (uint pk = 0; pk < X_P_ALU; pk++) probe = (probe + w) ^ (probe >> 3); }
#endif
#ifdef X_P_READ
    { uint ph = ((w >> 7) ^ (pc >> 2)) * 0x9E3779B1u;
      probe += tlb2_tag[ph >> (32 - X_P_READ)] + tlb2_tag[(ph << 5) >> (32 - X_P_READ)] + tlb2_tag[(ph << 10) >> (32 - X_P_READ)] + tlb2_tag[(ph << 15) >> (32 - X_P_READ)]; }
#endif
#ifdef X_P_TEX
    { for (uint pk = 0; pk < X_P_TEX; pk++) probe += RAM_TEX(RAM_ADDR(((w >> 7) + pk * 977 + (probe & 1)) & 0xffff)).x; }
#endif
    xr[(wr && rd != 0) ? rd : 32] = val;   // branch-free: discarded writes go to the scratch slot
    // cpu.clint.mtime and cpu.debug_do_tick already hold this pass's values: the first tick of
    // every pass takes the general path (irq_quiet starts false), which sets both.
    cpu.pc = npc;
    // (the texel's next word is the next instruction; a jump within the texel is fetched anew)
    fast_same = npc == pc + 4 && (npc & 12) != 0;
    fw_cur = fw_cur.yzwx;
    return true;
}
#undef rs1v
#undef rs2v
#undef K_ALU
#undef K_FP

#ifdef LAZY_MEMOP
// The pages from RAM offset `lo` to `hi` leave every TLB the fast path reads: the first level,
// the megapages that hold them, and the second level's entries, made in this pass or left by
// the last (those are marked dead, as a flush marks them).
void pend_unmap_l1(L1P uint lo, uint hi) {
    uint p0 = 0x80000000 | (lo & ~0xfff), p1 = 0x80000000 | (hi & ~0xfff);
    tlb_f_vpn = TLB_EMPTY; tlb_r_vpn = TLB_EMPTY; tlb_w_vpn = TLB_EMPTY;
    uint k;
    [loop]
    for (k = 0; k < 3 * TLBM_N; k++) {
        uint m = tlbm_pg[k] >> 22;
        if (m >= (p0 >> 22) && m <= (p1 >> 22)) tlbm_tag[k] = 0;
    }
    [loop]
    for (k = 0; k < 3 * TLB2_N / 2; k++) {
        uint at = TLB_STATE_AT + k;
        uint4 t = tlb_wiped ? (uint4)0 : STATE_TEX(uint2(at & 63, at >> 6));
        uint ka = 2 * k, kb = 2 * k + 1;
        uint pa = TLB2_OCC(ka) ? tlb2_pg[ka] : (TLB2_DEAD(ka) || t.r == 0) ? 0 : t.g;
        uint pb = TLB2_OCC(kb) ? tlb2_pg[kb] : (TLB2_DEAD(kb) || t.b == 0) ? 0 : t.a;
        if (pa >= p0 && pa <= p1) {
            tlb2_occ[ka >> 5] &= ~(1u << (ka & 31));
            tlb2_dead[ka >> 5] |= 1u << (ka & 31);
        }
        if (pb >= p0 && pb <= p1) {
            tlb2_occ[kb >> 5] &= ~(1u << (kb & 31));
            tlb2_dead[kb >> 5] |= 1u << (kb & 31);
        }
    }
}

// A fill or copy was asked for: of cpu.memop_n bytes to `dst` (an offset in RAM) from `src_p`
// (physical, RAM or ROM). True: it is left to the commit and the pass goes on (docs/machine/boot.md).
// Only whole texels, apart from each other and from a pending destination, none in the write
// cache (the texture is then the source as it is now), paging on, not the page being run.
bool memop_lazy_l1(L1P bool fill, uint src_p, uint dst) {
    uint n = cpu.memop_n, src = src_p & 0x7fffffff;
    bool ok = cpu.mmu.mode != MMU_MODE_OFF && cpu.csr.privilege != PRIV_MACHINE && (cpu.pc >> 12) == fetch_vpn;
#if CORES > 1
    ok = ok && hart == 0;
#endif
    ok = ok && n != 0 && ((n | dst) & 15) == 0 && dst < RAM_MAX && n <= RAM_MAX - dst;
    // a copy's source: RAM apart from the destination, or the ROM (which nothing writes)
    bool from_rom = (src_p & 0xc000000f) == 0x40000000;
    uint rom_at = src_p - 0x40000000, rom_bytes = m_dim.x * m_dim.y * 16;
    if (!fill && from_rom) {
        ok = ok && rom_at < rom_bytes && n <= rom_bytes - rom_at;
    } else if (!fill) {
        ok = ok && (src_p & 0x8000000f) == 0x80000000 && src < RAM_MAX && n <= RAM_MAX - src;
        ok = ok && (src + n <= dst || dst + n <= src);
    }
    uint dt = dst >> 4, nt = n >> 4, st = src >> 4, ft = (fetch_page & 0x7fffffff) >> 4;
    ok = ok && (ft + 256 <= dt || dt + nt <= ft);
    // the destination's rows as rectangles of the RAM texture, 2,048 texels a row
    uint row0 = dt >> 11, row1 = (dt + nt - 1) >> 11;
    uint rects = row0 == row1 ? 1 : row1 > row0 + 1 ? 3 : 2;
    uint first0 = pend_dst_t >> 11, first1 = (pend_dst_t + pend_n_t - 1) >> 11;
    uint rects_first = pend_n_t == 0 ? 0 : first0 == first1 ? 1 : first1 > first0 + 1 ? 3 : 2;
    ok = ok && rects_first + rects <= 3;   // (the commit's point for these has twelve vertices)
    // (pend_n_t 0: these hold; a source that is the first one's destination would need its result)
    ok = ok && (dt + nt <= pend_dst_t || pend_dst_t + pend_n_t <= dt);
    if (!fill && !from_rom) ok = ok && (st + nt <= pend_dst_t || pend_dst_t + pend_n_t <= st);
    [branch]
    if (ok) {
        ok = !l1_holds(dt, nt);
        [branch]
        if (ok && !fill && !from_rom) {
            ok = !l1_holds(st, nt);
        }
    }
    [branch]
    if (ok) {
        // what the commit needs is kept in words of the state nothing else uses (types.h)
        if (pend_n_t == 0) {
            cpu.debug_arb_0 = (fill ? 2 : 1) | (nt << 2);
            cpu.trap_count = dst;
            cpu.debug_last_ins = fill ? cpu.memop_src_v : src_p;
        } else {
            cpu.stall_count = (fill ? 2 : 1) | (nt << 2);
            cpu.memop_dst_p = dst;
            cpu.memop_src_p = fill ? cpu.memop_src_v : src_p;
        }
        // from here only the general path reaches the destination (mem.h)
        pend_unmap(dst, dst + n - 1);
    }
    return ok;
}
#endif


void emulate_l1(L1P0) {
    uint ins_word = 0;
    ins_ret ret = ins_ret_noop();
    bool mtip_reset = false;
    bool lazy_wait = false;
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
#ifdef LAZY_MEMOP
            // a page the pending fill or copy writes is not run before the commit has made it:
            // the pass ends and this instruction is the next one's first
            if (!ret.trap.en && (ins_addr & 0x80000000) != 0 && PEND_HAS((ins_addr & 0x7fffffff) >> 4)) {
                cpu.stall = STALL_FENCE;
                ret.pc_val = cpu.pc;
                lazy_wait = true;
            }
#endif
        }
#endif

        if (!ret.trap.en && !lazy_wait) {
            if (pre_valid) { ins_word = pre_word; } else { ins_word = mem_get_instruction(ins_addr); }

            ret = ins_select(ins_word, ret);
            // pause (the Zihintpause hint, a fence that does nothing): the program is waiting for
            // something only another pass can bring, and unlike wfi it may say so in any mode
            if (ins_word == 0x0100000f) {
                cpu.stall = STALL_WFI;
                MC_SLEEP   // (a worker core: until its job word changes, mc.h)
            }

            if (ret.csr_write && !ret.trap.en) {
                set_csr(ret.csr_write, ret.csr_val, ret);

                if (cpu.stall == STALL_MEMOP_COPY || cpu.stall == STALL_MEMOP_FILL) {
                    bool mo_fill = cpu.stall == STALL_MEMOP_FILL, mo_now = true;
                    uint mo_src = 0, mo_dst = 0;
#ifdef LAZY_MEMOP
                    // two a pass are left to the commit; a third ends the pass without being made
                    // and is asked for again when the next begins
                    mo_now = pend2_n_t == 0;
#endif
                    [branch]
                    if (mo_now) {
                        [loop]
                        for (uint imc = mo_fill ? 1 : 0; imc < 2; imc++) {   // a fill has no source address
                            uint phys = mmu_translate(ret,
                                imc == 0 ? cpu.memop_src_v : cpu.memop_dst_v,
                                imc == 0 ? MMU_ACCESS_READ : MMU_ACCESS_WRITE);
                            if (ret.trap.en) break;
                            if (imc == 0) {
                                mo_src = phys;
                            } else {
                                // Note: This only allows targets within RAM
                                mo_dst = phys & 0x7fffffff;
                            }
                        }
#ifdef LAZY_MEMOP
                        [branch]
                        if (!ret.trap.en) {
                            if (memop_lazy_l1(L1A mo_fill, mo_src, mo_dst)) {
                                cpu.stall = 0;
                            } else if (pend_n_t != 0) {
                                mo_now = false;   // one that ends the pass is not made in a pass that has others to make
                            }
                        }
#endif
                    }
                    if (!mo_now) {
                        cpu.stall = STALL_FENCE;
                        ret.pc_val = cpu.pc;
                    } else if (cpu.stall != 0 && !ret.trap.en) {
                        cpu.memop_src_p = mo_src;
                        cpu.memop_dst_p = mo_dst;
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

#ifdef CSR_FIELDS
    // Nothing below can have an effect while irq_quiet holds and no trap is pending.
    if (irq_quiet && !ret.trap.en) {
        PROF(PROF_irq_gated)
        cpu.pc = ret.pc_val;
        return;
    }
    MC_FAULT(ret)   // (a worker core in user mode stops where it is: mc.h)
    bool e_no_trap = !ret.trap.en;
    handle_irq_and_trap(ret, cpu.mip);
    irq_quiet = e_no_trap && !irq_last_handled && cpu.stall == 0;
    cpu.pc = ret.pc_val;
#else
    // handle CLINT IRQs
    cpu.clint.mtime_lo = pass_mtime_lo;
    cpu.clint.mtime_hi = pass_mtime_hi;

    // Nothing below can have an effect while irq_quiet holds, no trap is pending and this is
    // not a UART input poll tick.
    if (irq_quiet && !ret.trap.en && !(UART_INPUT_WAITING && (cpu.clock & 0xff) == 0xff)) {
        PROF(PROF_irq_gated)
        cpu.pc = ret.pc_val;
        return;
    }

    uint mip_override = read_mip();
    if (cpu.clint.msip) {
        mip_override |= MIP_MSIP;
    }

    if ((cpu.clint.mtimecmp_lo != 0 || cpu.clint.mtimecmp_hi != 0) && (cpu.clint.mtime_hi > cpu.clint.mtimecmp_hi || (cpu.clint.mtime_hi == cpu.clint.mtimecmp_hi && cpu.clint.mtime_lo >= cpu.clint.mtimecmp_lo))) {
#ifdef SBI_HLE
        mip_override |= MIP_STIP;   // no firmware in between: the supervisor's timer interrupt
#else
        mip_override |= MIP_MTIP;
#endif
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

    MC_FAULT(ret)   // (a worker core in user mode stops where it is: mc.h)
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
#endif

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
#ifdef LAZY_MEMOP
        // (without paging no page run is looked at: a pending fill or copy is made first)
        if (xl_ident_f && pend_n_t != 0 && cpu.stall == 0) cpu.stall = STALL_FENCE;
#endif
        xl_ident_d = cpu.mmu.mode == MMU_MODE_OFF || (machine && (((ms >> 17) & 0x1) == 0 || ((ms >> 11) & 0x3) == PRIV_MACHINE));
    }
#endif
}

#endif
