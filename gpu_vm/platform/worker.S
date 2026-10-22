// gpu_vm's worker firmware (gpu_vm/platform.md, "Workers"): the RISC-V machine's worker protocol
// (docs/machine/multicore.md, programs/mc/mc.h; rvc_cpu's worker_wake and mc_fault) as a supervisor program on each
// worker core. The host puts it into the boot area (read only to the cores) at GVM_WORKER_FW and starts worker core k
// at fw_entry in supervisor mode. It needs nothing of the kernel: its words are the mailboxes' and its own save area.
//
//   parked:   its start texel is polled once a pass (sync ends the pass); MCSU with a pc it has not started at (or any
//             pc once it was stopped by MCST) starts it: registers 0, r2 = sp, r5 = its number, user mode at pc
//   running:  the timer is set for the next pass (TCMP = TIME + 1), so the firmware looks at the start word as every
//             pass begins: MCST parks the core wherever it is
//   break:    parks it
//   syscall:  the call record (r12, r5-r10), the fault record {resume + 1, 8, 0, pc}; when core 0 changes the resume
//             word's low 24 bits, r5 = the resume texel's second word and it goes on after the syscall
//   a fault:  the fault record with RISC-V's cause (1 fetch, 2 illegal, 5 load, 7 store); it goes on at the op again
//             when the resume word changes
//
// The save area, 64 bytes a core at 0x00c0c000 + 64k (device memory between the mailboxes and the counters): the
// user's r56-r62 (and in word 7 the area's own address), then start_seen, started, seen (the resume word at the
// trap), kind (1: a call). While a job runs SCRATCH holds the area's address; in a trap the user's r63 is there.

#define MBOX      0x00c00000
#define START(r)  MBOX(r)           // + 16k: { magic, pc, sp, root }
#define CALLREC   0x00c02000        // + 32k: { a7, a0 .. a5 }
#define FAULTREC  0x00c04020        // + 64k: { count, cause, address, pc }
#define RESUME    0x00c04030        // + 64k: { resume, answer, taken, - }
#define SAVE      0x00c0c000        // + 64k
#define MCSU      0x5553434d
#define MCST      0x5453434d
#define S_SEEN    32
#define S_STARTED 36
#define S_RESUME  40
#define S_KIND    44

    .text
    .globl fw_entry, fw_park, fw_park_end, fw_trap, fw_zero
    .p2align 4
fw_entry:
    mfsr r40, coreid
    slli r41, r40, 6
    addi r41, r41, SAVE             // r41 = the save area
    lpc r42, fw_trap
    mtsr tvec, r42
    li r42, 2                       // the timer (IEN bit 1); IE stays 0 in supervisor mode
    mtsr ien, r42
    sw zero, S_SEEN(r41)
    sw zero, S_STARTED(r41)
    sw r41, 28(r41)                 // (word 7: lq of r60 brings it back into r63)

// Parked: r40 = the core's number, r41 = its save area
fw_park:
    slli r43, r40, 4
    lq r44, MBOX(r43)               // r44 magic, r45 pc, r46 sp
    li r48, MCST
    bne r44, r48, 1f
    sw zero, S_STARTED(r41)         // stopped: a start at any pc starts it again
1:  li r48, MCSU
    bne r44, r48, 2f
    lw r49, S_STARTED(r41)
    lw r50, S_SEEN(r41)
    beqz r49, fw_start
    bne r45, r50, fw_start
2:  sync
    j fw_park
fw_park_end:

fw_start:
    sw r45, S_SEEN(r41)
    li r49, 1
    sw r49, S_STARTED(r41)
    mtsr epc, r45
    li r49, 2                       // PIE: eret makes IE 1 and the mode the user's (PS 0)
    mtsr status, r49
    mtsr scratch, r41
    mfsr r49, time_lo
    mfsr r50, time_hi
    addi r49, r49, 1
    seqi r51, r49, 0
    add r50, r50, r51
    mtsr tcmp_hi, r50
    mtsr tcmp_lo, r49
    mv r2, r46
    mv r5, r40
    lpc r60, fw_zero
    lq r8, 0(r60)
    lq r12, 0(r60)
    lq r16, 0(r60)
    lq r20, 0(r60)
    lq r24, 0(r60)
    lq r28, 0(r60)
    lq r32, 0(r60)
    lq r36, 0(r60)
    lq r40, 0(r60)
    lq r44, 0(r60)
    lq r48, 0(r60)
    lq r52, 0(r60)
    lq r56, 0(r60)
    li r1, 0
    li r3, 0
    li r4, 0
    li r6, 0
    li r7, 0
    lq r60, 0(r60)
    eret

// A trap or an interrupt from the user's job
    .p2align 4
fw_trap:
    xsr r63, scratch, r63           // r63 = the save area, SCRATCH = the user's r63
    sq r56, 0(r63)
    sq r60, 16(r63)                 // r60-r62 (and r63, the area's address, in word 7)
    mfsr r56, cause
    bltz r56, fw_irq
    li r57, 2                       // break: parks
    beq r56, r57, fw_stopped
    mfsr r60, coreid
    slli r61, r60, 6
    lw r62, RESUME(r61)
    sw r62, S_RESUME(r63)
    li r57, 3
    bne r56, r57, fw_fault
    // a system call: its record for core 0
    slli r58, r60, 5
    sw r12, CALLREC(r58)
    sq r5, CALLREC+4(r58)           // r5-r8
    sd r9, CALLREC+20(r58)          // r9, r10
    li r57, 1
    sw r57, S_KIND(r63)
    li r57, 8
    li r58, 0
    j fw_record
fw_fault:
    sw zero, S_KIND(r63)
    lbu r57, fw_causes(r56)
    mfsr r58, tval
fw_record:                          // r57 cause, r58 address, r61 = 64k, r62 the resume word
    addi r56, r62, 1
    mfsr r59, epc
    sq r56, FAULTREC(r61)
fw_wait:
    sync
    mfsr r60, coreid
    slli r61, r60, 4
    lw r62, MBOX(r61)
    li r56, MCST
    beq r62, r56, fw_stop
    slli r61, r60, 6
    lw r62, RESUME(r61)
    lw r56, S_RESUME(r63)
    xor r57, r62, r56
    slli r57, r57, 8                // the low 24 bits
    beqz r57, fw_wait
    lw r56, S_KIND(r63)
    beqz r56, fw_back               // a fault: the op again
    lw r5, RESUME+4(r61)            // a call: its answer, and on after it
    mfsr r56, epc
    addi r56, r56, 4
    mtsr epc, r56
fw_back:                            // the timer for the next pass, the user's registers, and back
    mfsr r56, time_lo
    mfsr r57, time_hi
    addi r56, r56, 1
    seqi r58, r56, 0
    add r57, r57, r58
    mtsr tcmp_hi, r57
    mtsr tcmp_lo, r56
    lq r56, 0(r63)
    lq r60, 16(r63)                 // r63 = the area again
    xsr r63, scratch, r63           // the user's r63; SCRATCH = the area
    eret

fw_irq:                             // the timer, once a pass: MCST parks the core
    mfsr r60, coreid
    slli r61, r60, 4
    lw r62, MBOX(r61)
    li r56, MCST
    bne r62, r56, fw_back
fw_stop:
    sw zero, S_STARTED(r63)
fw_stopped:
    mtsr tcmp_lo, zero
    mtsr tcmp_hi, zero
    mfsr r40, coreid
    mv r41, r63
    j fw_park

    .p2align 4
fw_causes:                          // gpu_vm's cause -> RISC-V's (mcw.c's serve() reads RISC-V's)
    .byte 0, 2, 3, 8, 5, 7, 1, 2
    .p2align 4
fw_zero:
    .word 0, 0, 0, 0
