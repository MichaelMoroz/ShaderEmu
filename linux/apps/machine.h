/* What the processor and its Linux are, found by asking them: an instruction is run and is an
 * illegal one or not, and /proc/cpuinfo names the MMU only when there is one. */
#ifndef SHADEREMU_APPS_MACHINE_H
#define SHADEREMU_APPS_MACHINE_H

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static sigjmp_buf machine_no_instruction;

static void
machine_trap(int signal_number)
{
	(void)signal_number;
	siglongjmp(machine_no_instruction, 1);
}

/* Runs instruction 1 or 2 (a word each); true if it was not illegal. */
static int
machine_runs(int which)
{
	void (*before)(int) = signal(SIGILL, machine_trap);
	volatile int there = 0;

	if (!sigsetjmp(machine_no_instruction, 1)) {
		if (which == 1)
			__asm__ volatile(".word 0xd0007053");	/* fcvt.s.w ft0, zero */
		else
			__asm__ volatile(".word 0x20002033");	/* sh1add zero, zero, zero (Zba) */
		there = 1;
	}
	signal(SIGILL, before);
	return there;
}

/* Float instructions (F); the address and bit instructions (Zba, Zbb: the machine has both or neither). */
static int machine_float(void) { return machine_runs(1); }
static int machine_zb(void) { return machine_runs(2); }

/* Sv32 paging: Linux's cpuinfo has an "mmu" line only when it was built with one (docs/system/nommu.md). */
static int
machine_mmu(void)
{
	FILE *file = fopen("/proc/cpuinfo", "r");
	char line[160];
	int found = 0;

	while (file && !found && fgets(line, sizeof line, file))
		found = !strncmp(line, "mmu", 3);
	if (file)
		fclose(file);
	return found;
}

/* "RV32IMAF" and what more the processor has, for a line of text. */
static const char *
machine_isa(void)
{
	static int known, zb, fl;

	if (!known)
		fl = machine_float(), zb = machine_zb(), known = 1;
	return zb ? (fl ? "RV32IMAF + Zba, Zbb" : "RV32IMA + Zba, Zbb") : fl ? "RV32IMAF" : "RV32IMA";
}

#endif
