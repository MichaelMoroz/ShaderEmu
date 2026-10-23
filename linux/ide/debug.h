/* debug.h: a program run a line at a time (docs/apps/ide.md, "The debugger"). What the lines,
 * functions and variables are comes from the stabs TinyCC writes with -g; the program is held
 * with ptrace, a line's first instruction changed for a stop (DBG_STOP) where it should stop. */
#ifndef IDE_DEBUG_H
#define IDE_DEBUG_H
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

/* A stop is an instruction that is none: the kernel stops the first core's thread with SIGILL
 * there. Not ebreak: on a worker core that is "park", and a worker that came to a marked line
 * went to sleep in the middle of its job with nobody told. This one is a trap there too. */
#ifdef __gpuvm__
/* gpu_vm (gpu_vm/isa.h): a line begins at a bundle of 16 bytes, and its stop is in the control word
 * (word 3): three ops on the general path, which is illegal before any op of the bundle runs. The
 * trap's pc is the control word's, so the program goes on from the bundle's start (dbg_rewind).
 * The registers are 64 words, the pc first; s0 (r32) is the frame's pointer; a call's return
 * address is the next bundle. */
#define DBG_STOP 0x00000018u
#define DBG_WORD 12u
#define DBG_CC(w) ((w) >> 7 & 15u)		/* 0 special, else a branch */
#define DBG_SUB(w) ((w) >> 11 & 7u)		/* cc 0: 2 jr, 3 callr, 5 j, 6 call */
#define DBG_RS(w) ((w) >> 14 & 63u)
#define DBG_OFF9(w) ((int32_t)((w) << 6) >> 23)	/* a branch's, in bundles */
#define DBG_OFF18(w) ((int32_t)(w) >> 14)	/* j's and call's */
#define DBG_REGS 64
#define DBG_FP 32
#define DBG_NEXT 16u
#define DBG_HERE(pc) ((pc) & ~15u)
/* The fetch reads RAM as the last commit left it, and the kernel's poke is a store of this pass: the
 * program let run in it would run the code as it was. A sync first (code stored runs after one). */
#define DBG_SETTLE() __asm__ volatile("sync" ::: "memory")
#else
#define DBG_SETTLE() ((void)0)
#define DBG_STOP 0x00000000u
#define DBG_WORD 0u
#define DBG_REGS 32
#define DBG_FP 8
#define DBG_NEXT 4u
#define DBG_HERE(pc) (pc)
#endif
enum { T_NONE, T_INT, T_FLOAT, T_POINTER, T_ARRAY, T_STRUCT, T_FUNCTION };
enum { V_LOCAL, V_FIXED };		/* a variable is at the frame's pointer plus `at`, or at `at` */
enum { DBG_RUN, DBG_OVER, DBG_INTO, DBG_OUT };
enum { DBG_GONE, DBG_RUNNING, DBG_STOPPED };

struct dbg_type { uint8_t kind, is_unsigned; int size, target, count, fields, field_count; };
struct dbg_field { char name[24]; int type, offset; };
struct dbg_var { char name[32]; int type, where, function; int32_t at; };
struct dbg_function { char name[40]; uint32_t start, end; };
struct dbg_line { uint32_t address; int line, function; };
struct dbg_stop { uint32_t address, word, was; int user; };	/* word: where the stop is (a bundle's word on gpu_vm) */

#define DBG_TYPES 1024
static struct dbg_type dbg_types[DBG_TYPES];
static struct dbg_field dbg_fields[1024];
static int dbg_field_count;
static struct dbg_var *dbg_vars;
static struct dbg_function *dbg_functions;
static struct dbg_line *dbg_lines;
static int dbg_var_count, dbg_function_count, dbg_line_count;

static pid_t dbg_pid;
static int dbg_state = DBG_GONE, dbg_mode;
static uint32_t dbg_regs[DBG_REGS];	/* pc, then x1 to x31 (r1 to r63 on gpu_vm) */
static uint32_t dbg_from_sp;		/* the stack where a step began */
static int dbg_line_now, dbg_function_now, dbg_signal, dbg_exit;
static int dbg_on_break;		/* it stands on a line whose stop was lifted: that instruction is still to run */
static int dbg_moving;			/* it has been let run since it was started (it is not at its very start) */
static int dbg_shared;			/* the program posts work to the worker cores (it has the library's mcw_post) */
static int dbg_fresh;			/* a step ended without the program being let run: dbg_poll() says so once */
static struct dbg_stop *dbg_stops;	/* the lines' stops while it runs */
static int dbg_stop_count;
static int dbg_marks[256], dbg_mark_count;	/* the user's breakpoints: lines */
static uint32_t dbg_elf_entry;		/* where the program's file says it starts (_start for a library's link) */

/* ---- what the program is made of: its stabs ---- */

static int dbg_type_at(const char **s);

/* A type's definition, after "N=": what kind it is, and how large. */
static void
dbg_define(int n, const char **s)
{
	struct dbg_type t = { T_NONE, 0, 4, 0, 0, 0, 0 };
	unsigned long long low, high;
	int other;

	switch (**s) {
	case 'r':
		for ((*s)++; **s && **s != ';'; (*s)++)
			continue;
		low = strtoull(*s + 1, (char **)s, 0);
		high = strtoull(*s + 1, (char **)s, 0);
		if (**s == ';')
			(*s)++;
		if (high == 0 && low > 0 && low <= 16)
			t.kind = T_FLOAT, t.size = (int)low;
		else {
			t.kind = T_INT;
			t.is_unsigned = low == 0;
			t.size = high <= 255 ? 1 : high <= 65535 ? 2 : high <= 0xffffffffull ? 4 : 8;
		}
		break;
	case '*':
		(*s)++;
		t.kind = T_POINTER, t.target = dbg_type_at(s);
		break;
	case 'a':
		*s += 2;	/* "ar": the index's type, its first and last, then what it is of */
		dbg_type_at(s);
		strtol(*s + 1, (char **)s, 10);
		t.count = (int)strtol(*s + 1, (char **)s, 10) + 1;
		(*s)++;
		t.kind = T_ARRAY, t.target = dbg_type_at(s);
		t.size = t.count * (t.target < DBG_TYPES ? dbg_types[t.target].size : 4);
		break;
	case 's':
	case 'u':
		t.kind = T_STRUCT;
		t.size = (int)strtol(*s + 1, (char **)s, 10);
		t.fields = dbg_field_count;
		while (**s && **s != ';') {
			struct dbg_field f = { "", 0, 0 };
			int k = 0;

			while (**s && **s != ':') {
				if (k < (int)sizeof f.name - 1)
					f.name[k++] = **s;
				(*s)++;
			}
			(*s)++;
			f.type = dbg_type_at(s);
			f.offset = (int)strtol(*s + 1, (char **)s, 10) / 8;
			strtol(*s + 1, (char **)s, 10);
			if (**s == ';')
				(*s)++;
			if (dbg_field_count < (int)(sizeof dbg_fields / sizeof dbg_fields[0]))
				dbg_fields[dbg_field_count++] = f, t.field_count++;
		}
		if (**s == ';')
			(*s)++;
		break;
	case 'f':
		(*s)++;
		t.kind = T_FUNCTION, t.target = dbg_type_at(s);
		break;
	case 'e':
		while (**s && **s != ';')
			(*s)++;
		t.kind = T_INT;
		break;
	default:
		if (**s >= '0' && **s <= '9') {	/* another name for a type */
			other = dbg_type_at(s);
			if (other != n && other < DBG_TYPES)
				t = dbg_types[other];
		}
	}
	if (n < DBG_TYPES)
		dbg_types[n] = t;
}

/* A type where a stab names one: its number, and its definition taken in if it follows. */
static int
dbg_type_at(const char **s)
{
	int n = (int)strtol(*s, (char **)s, 10);

	if (**s == '=') {
		(*s)++;
		dbg_define(n, s);
	}
	return n;
}

static uint32_t
dbg_symbol(const Elf32_Sym *symbols, int count, const char *names, const char *name)
{
	int i;

	for (i = 0; i < count; i++)
		if (!strcmp(names + symbols[i].st_name, name))
			return symbols[i].st_value;
	return 0;
}

/* Reads a program's stabs. False when it has none (it was not made with -g). */
static int
dbg_load(const char *path)
{
	struct stab { uint32_t name; uint8_t type, other; uint16_t desc; uint32_t value; } *stabs = NULL;
	FILE *file = fopen(path, "rb");
	Elf32_Ehdr head;
	Elf32_Shdr *sections;
	Elf32_Sym *symbols = NULL;
	char *names, *strings = NULL, *symbol_names = NULL, *image;
	long size;
	int i, count = 0, symbol_count = 0, function = -1;

	dbg_var_count = dbg_function_count = dbg_line_count = dbg_field_count = 0;
	memset(dbg_types, 0, sizeof dbg_types);
	if (!file)
		return 0;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	rewind(file);
	image = malloc((size_t)size);
	if (!image || fread(image, 1, (size_t)size, file) != (size_t)size || memcmp(image, ELFMAG, 4)) {
		fclose(file);
		free(image);
		return 0;
	}
	fclose(file);
	memcpy(&head, image, sizeof head);
	sections = (Elf32_Shdr *)(image + head.e_shoff);
	names = image + sections[head.e_shstrndx].sh_offset;
	for (i = 0; i < head.e_shnum; i++) {
		const char *name = names + sections[i].sh_name;

		if (!strcmp(name, ".stab"))
			stabs = (struct stab *)(image + sections[i].sh_offset), count = (int)(sections[i].sh_size / 12);
		else if (!strcmp(name, ".stabstr"))
			strings = image + sections[i].sh_offset;
		else if (!strcmp(name, ".symtab"))
			symbols = (Elf32_Sym *)(image + sections[i].sh_offset), symbol_count = (int)(sections[i].sh_size / sizeof *symbols);
		else if (!strcmp(name, ".strtab"))
			symbol_names = image + sections[i].sh_offset;
	}
	dbg_shared = symbols && dbg_symbol(symbols, symbol_count, symbol_names, "mcw_post") != 0;
	dbg_elf_entry = head.e_entry ? head.e_entry : symbols ? dbg_symbol(symbols, symbol_count, symbol_names, "_start") : 0;
	if (!stabs || !strings) {
		free(image);
		return 0;
	}
	dbg_vars = realloc(dbg_vars, (size_t)count * sizeof *dbg_vars);
	dbg_functions = realloc(dbg_functions, (size_t)count * sizeof *dbg_functions);
	dbg_lines = realloc(dbg_lines, (size_t)count * sizeof *dbg_lines);
	for (i = 0; i < count; i++) {
		const char *text = strings + stabs[i].name, *colon = strchr(text, ':'), *s;
		struct dbg_var v = { "", 0, V_FIXED, -1, 0 };

		if (stabs[i].type == 0x44 && function >= 0) {	/* a line: how far into its function */
			/* not the function's first (its frame is not made there yet: its variables
			 * would be the caller's), nor the closing brace's, which is at no instruction */
			if (stabs[i].value == 0 || stabs[i].value & 3)
				continue;
			dbg_lines[dbg_line_count].address = dbg_functions[function].start + stabs[i].value;
			dbg_lines[dbg_line_count].line = stabs[i].desc;
			dbg_lines[dbg_line_count++].function = function;
			continue;
		}
		if (!colon || !stabs[i].name)
			continue;
		snprintf(v.name, sizeof v.name, "%.*s", (int)(colon - text), text);
		s = colon + 1;
		if (colon == text) {	/* a type with no name */
			while (*s && (*s < '0' || *s > '9'))
				s++;
			dbg_type_at(&s);
			continue;
		}
		switch (stabs[i].type) {
		case 0x24:	/* a function */
			function = dbg_function_count++;
			snprintf(dbg_functions[function].name, sizeof dbg_functions[0].name, "%s", v.name);
			dbg_functions[function].start = dbg_functions[function].end = stabs[i].value;
			break;
		case 0x80:	/* a type's name, or a variable of the function */
			if (*s == 't' || *s == 'T') {
				s++;
				dbg_type_at(&s);
				break;
			}
			/* fall through */
		case 0xa0:	/* a parameter */
			if (*s == 'p')
				s++;
			v.type = dbg_type_at(&s);
			v.where = V_LOCAL, v.function = function, v.at = (int32_t)stabs[i].value;
			dbg_vars[dbg_var_count++] = v;
			break;
		case 0x20:	/* a variable of the whole program: the symbols say where */
		case 0x26:
		case 0x28:	/* one of a file or of a function: here */
			v.function = *s == 'V' ? function : -1;
			s++;
			v.type = dbg_type_at(&s);
			v.at = stabs[i].type == 0x20 && symbols ? (int32_t)dbg_symbol(symbols, symbol_count, symbol_names, v.name) : (int32_t)stabs[i].value;
			dbg_vars[dbg_var_count++] = v;
			break;
		}
	}
	/* a function reaches to where the next one begins (the last: a little past its last line) */
	for (i = 0; i < dbg_function_count; i++) {
		uint32_t end = dbg_functions[i].start + 256;
		int k;

		for (k = 0; k < dbg_line_count; k++)
			if (dbg_lines[k].function == i && dbg_lines[k].address + 256 > end)
				end = dbg_lines[k].address + 256;

		for (count = 0; count < dbg_function_count; count++)
			if (dbg_functions[count].start > dbg_functions[i].start && dbg_functions[count].start < end)
				end = dbg_functions[count].start;
		dbg_functions[i].end = end;
	}
	free(image);
	return dbg_line_count > 0;
}

/* The address a line's code begins at (the first line at or after it that has any), or 0. */
static uint32_t
dbg_address_of(int line, int *found)
{
	int i, best = -1;

	for (i = 0; i < dbg_line_count; i++)
		if (dbg_lines[i].line >= line && (best < 0 || dbg_lines[i].line < dbg_lines[best].line ||
						  (dbg_lines[i].line == dbg_lines[best].line && dbg_lines[i].address < dbg_lines[best].address)))
			best = i;
	if (found)
		*found = best < 0 ? 0 : dbg_lines[best].line;
	return best < 0 ? 0 : dbg_lines[best].address;
}

/* The line an address is in, and its function: the last line that begins at or before it. */
static int
dbg_line_of(uint32_t address, int *function)
{
	int i, best = -1;

	for (i = 0; i < dbg_line_count; i++)
		if (dbg_lines[i].address <= address && address < dbg_functions[dbg_lines[i].function].end &&
		    (best < 0 || dbg_lines[i].address > dbg_lines[best].address))
			best = i;
	if (function)
		*function = best < 0 ? -1 : dbg_lines[best].function;
	return best < 0 ? 0 : dbg_lines[best].line;
}

/* ---- the program held ---- */

static uint32_t
dbg_peek(uint32_t address)
{
	errno = 0;
	return (uint32_t)ptrace(PTRACE_PEEKTEXT, dbg_pid, (void *)(uintptr_t)(address & ~3u), NULL);
}

static void
dbg_poke(uint32_t address, uint32_t word)
{
	ptrace(PTRACE_POKETEXT, dbg_pid, (void *)(uintptr_t)address, (void *)(uintptr_t)word);
}

static void
dbg_read(uint32_t address, void *to, int bytes)
{
	uint8_t *out = to;
	int i;

	for (i = 0; i < bytes; i++) {
		uint32_t word = dbg_peek(address + (uint32_t)i);

		out[i] = (uint8_t)(word >> 8 * ((address + (uint32_t)i) & 3));
	}
}

static void
dbg_registers(int write)
{
	struct iovec io = { dbg_regs, sizeof dbg_regs };

	ptrace(write ? PTRACE_SETREGSET : PTRACE_GETREGSET, dbg_pid, (void *)1 /* NT_PRSTATUS */, &io);
}

/* At a stop: the pc where the program goes on from it. On gpu_vm the trap was at the bundle's
 * control word, and the bundle runs again whole. */
static void
dbg_rewind(void)
{
	if (dbg_regs[0] != DBG_HERE(dbg_regs[0])) {
		dbg_regs[0] = DBG_HERE(dbg_regs[0]);
		dbg_registers(1);
	}
}

/* Every address the stabs gave moved by `by`: a flat program is loaded wherever there is room. */
static void
dbg_relocate(uint32_t by)
{
	int i;

	for (i = 0; by && i < dbg_function_count; i++)
		dbg_functions[i].start += by, dbg_functions[i].end += by;
	for (i = 0; by && i < dbg_line_count; i++)
		dbg_lines[i].address += by;
	for (i = 0; by && i < dbg_var_count; i++)
		if (dbg_vars[i].where == V_FIXED && dbg_vars[i].at)
			dbg_vars[i].at += (int32_t)by;
}

/* Starts the program, held before its first instruction; its output goes to `out`. With no
 * MMU there is no fork: the kernel's vfork, whose child only makes system calls. */
static int
dbg_start(const char *path, int out)
{
	int status;

	dbg_pid = real_vfork();
	if (dbg_pid == 0) {
		ptrace(PTRACE_TRACEME, 0, NULL, NULL);
		dup2(out, 1), dup2(out, 2);
		execl(path, path, (char *)NULL);
		_exit(127);
	}
	if (dbg_pid < 0 || waitpid(dbg_pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
		dbg_state = DBG_GONE;
		return 0;
	}
	dbg_state = DBG_STOPPED;
	dbg_line_now = dbg_on_break = dbg_signal = dbg_moving = dbg_fresh = 0;
	dbg_function_now = -1;
	/* room for every stop a step can want: a function's lines, the marks, a few more */
	dbg_stops = realloc(dbg_stops, (size_t)(dbg_line_count + (int)(sizeof dbg_marks / sizeof dbg_marks[0]) + 16) * sizeof *dbg_stops);
	dbg_stop_count = 0;
	dbg_registers(0);
	/* held at its entry: where it was loaded is that against the file's entry */
	if (dbg_elf_entry)
		dbg_relocate(dbg_regs[0] - dbg_elf_entry);
	return 1;
}

static void
dbg_place(uint32_t address, int user)
{
	int i;

	for (i = 0; i < dbg_stop_count; i++)
		if (dbg_stops[i].address == address) {
			dbg_stops[i].user |= user;
			return;
		}
	dbg_stops[dbg_stop_count].address = address;
	dbg_stops[dbg_stop_count].user = user;
	dbg_stops[dbg_stop_count].word = address + DBG_WORD;
	dbg_stops[dbg_stop_count].was = dbg_peek(dbg_stops[dbg_stop_count].word);
	dbg_poke(dbg_stops[dbg_stop_count].word, DBG_STOP);
	dbg_stop_count++;
}

static void
dbg_lift(void)
{
	while (dbg_stop_count > 0) {
		dbg_stop_count--;
		dbg_poke(dbg_stops[dbg_stop_count].word, dbg_stops[dbg_stop_count].was);
	}
}

/* One instruction on, from a stop: there is no single step here, so a stop is put
 * wherever the instruction at pc can go next. False if the program ended instead. */
static int
dbg_one(void)
{
	uint32_t pc = DBG_HERE(dbg_regs[0]), next[2] = { pc + DBG_NEXT, 0 };
	int status, k;
#ifdef __gpuvm__
	uint32_t op = dbg_peek(pc + 12), sub = DBG_SUB(op);

	if (DBG_CC(op) == 4 && !(op >> 11 & 63) && !(op >> 26))	/* beq r0, r0: a near j */
		next[0] = pc + (uint32_t)(DBG_OFF9(op) * 16);
	else if (DBG_CC(op) == 5 && !(op >> 11 & 63) && !(op >> 26))	/* bne r0, r0: on to the next bundle */
		;
	else if (DBG_CC(op))	/* a branch, taken or not */
		next[1] = pc + (uint32_t)(DBG_OFF9(op) * 16);
	else if (sub == 5 || sub == 6)
		next[0] = pc + (uint32_t)(DBG_OFF18(op) * 16);
	else if (sub == 2 || sub == 3)
		next[0] = dbg_regs[DBG_RS(op)] & ~15u;
#else
	uint32_t op = dbg_peek(pc);

	if ((op & 0x7f) == 0x63)
		next[1] = pc + (uint32_t)((int32_t)(op & 0x80000000) >> 19 | (int32_t)(op >> 7 & 1) << 11 | (int32_t)(op >> 25 & 0x3f) << 5 |
					  (int32_t)(op >> 8 & 15) << 1);
	else if ((op & 0x7f) == 0x6f)
		next[0] = pc + (uint32_t)((int32_t)(op & 0x80000000) >> 11 | (int32_t)(op & 0xff000) | (int32_t)(op >> 20 & 1) << 11 |
					  (int32_t)(op >> 21 & 0x3ff) << 1);
	else if ((op & 0x7f) == 0x67)
		next[0] = (((op >> 15 & 31) ? dbg_regs[op >> 15 & 31] : 0) + (uint32_t)((int32_t)op >> 20)) & ~1u;
#endif
	for (k = 0; k < 2; k++)
		if (next[k])
			dbg_place(next[k], 0);
	DBG_SETTLE();
	ptrace(PTRACE_CONT, dbg_pid, NULL, NULL);
	if (waitpid(dbg_pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
		dbg_state = DBG_GONE;
		dbg_exit = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		dbg_stop_count = 0;
		return 0;
	}
	dbg_lift();
	dbg_registers(0);
	dbg_rewind();
	return 1;
}

/* The user's function an address is in, or -1. */
static int
dbg_function_at(uint32_t address)
{
	int i;

	for (i = 0; i < dbg_function_count; i++)
		if (address >= dbg_functions[i].start && address < dbg_functions[i].end)
			return i;
	return -1;
}

/* A function's first line with an instruction of its own (its frame is made by then), or 0. */
static uint32_t
dbg_entry(int function)
{
	uint32_t first = 0;
	int i;

	for (i = 0; i < dbg_line_count; i++)
		if (dbg_lines[i].function == function && (!first || dbg_lines[i].address < first))
			first = dbg_lines[i].address;
	return first;
}

static int
dbg_marked(int line)
{
	int i;

	for (i = 0; i < dbg_mark_count; i++)
		if (dbg_marks[i] == line)
			return 1;
	return 0;
}

/*
 * Lets it run: to the next marked line, or (a step) to the next line of the function it is in
 * or to where that function returns. A step's stops are in that one function and nowhere
 * else: the program's other cores run the same memory, and a stop one of them reaches is
 * nobody's to answer (this holds the first core's thread alone). `also` is one more place.
 */
static void
dbg_go_to(int mode, uint32_t also)
{
	uint32_t back;
	int i, found;

	if (dbg_state != DBG_STOPPED)
		return;
	dbg_mode = mode;
	/* off the instruction it stands on first, or a stop put there would be reached at once */
	if (dbg_moving && !dbg_signal && !dbg_one())
		return;
	dbg_moving = 1;
	dbg_on_break = 0;
	for (i = 0; i < dbg_mark_count; i++)
		dbg_place(dbg_address_of(dbg_marks[i], &found), 1);
	dbg_function_now = dbg_function_at(dbg_regs[0]);
	if (mode != DBG_RUN && dbg_function_now < 0) {
		/* not in the program's own code (its start, or a library): on to main */
		for (i = 0; i < dbg_function_count; i++)
			if (!strcmp(dbg_functions[i].name, "main") && dbg_entry(i))
				dbg_place(dbg_entry(i), 0);
	} else if (mode != DBG_RUN) {
		for (i = 0; i < dbg_line_count && mode != DBG_OUT; i++)
			if (dbg_lines[i].function == dbg_function_now)
				dbg_place(dbg_lines[i].address, 0);
		/* TinyCC keeps the return address just under the frame's pointer */
		back = dbg_peek(dbg_regs[DBG_FP] - 4);
		if (back && !(back & 3))
			dbg_place(back, 0);
	}
	if (also)
		dbg_place(also, 0);
	DBG_SETTLE();
	ptrace(PTRACE_CONT, dbg_pid, NULL, (void *)(uintptr_t)dbg_signal);
	dbg_signal = 0;
	dbg_state = DBG_RUNNING;
}

static void
dbg_go(int mode)
{
	dbg_go_to(mode, 0);
}

/* Asks how the program is, without waiting: true when that has just changed (it stopped at a
 * line, or with a signal in dbg_signal, or it is gone with dbg_exit). */
static int
dbg_poll(void)
{
	int status, ours, i;

	if (dbg_fresh) {
		dbg_fresh = 0;
		return 1;
	}
	if (dbg_state != DBG_RUNNING || waitpid(dbg_pid, &status, WNOHANG) <= 0)
		return 0;
	if (!WIFSTOPPED(status)) {
		dbg_state = DBG_GONE;
		dbg_exit = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
		dbg_stop_count = 0;
		return 1;
	}
	dbg_registers(0);
	/* its own stop, if it stands on one: any other SIGILL is the program's */
	for (ours = 0, i = 0; i < dbg_stop_count; i++)
		ours |= dbg_stops[i].address == DBG_HERE(dbg_regs[0]);
	dbg_lift();
	dbg_state = DBG_STOPPED;
	dbg_line_now = dbg_line_of(dbg_regs[0], &dbg_function_now);
	if (getenv("NXIDE_TRACE"))
		fprintf(stderr, "dbg: stop signal %d pc %x line %d function %d sp %x from %x mode %d\n", WSTOPSIG(status), dbg_regs[0], dbg_line_now,
			dbg_function_now, dbg_regs[2], dbg_from_sp, dbg_mode);
	if (WSTOPSIG(status) == SIGILL && ours)
		dbg_rewind();
	if (!(WSTOPSIG(status) == SIGILL && ours)) {
		dbg_signal = WSTOPSIG(status);
		return 1;
	}
	dbg_on_break = 1;
	if (dbg_marked(dbg_line_now))
		return 1;
	/* out of the program's own code (main has returned): on, to the next mark or the end */
	if (!dbg_line_now) {
		dbg_go(DBG_RUN);
		return 0;
	}
	/* a step over a line of a function that calls itself stops where the stack is no deeper */
	if (dbg_mode == DBG_OVER && dbg_regs[2] < dbg_from_sp) {
		dbg_go(DBG_OVER);
		return 0;
	}
	return 1;
}

/*
 * A step, from where it stands. Into: an instruction at a time until the line is another, and
 * at a call of one of the program's own functions on to that function's first line (a call
 * of anything else is stepped over, as a line is).
 */
static void
dbg_step(int mode)
{
	int line = dbg_line_now, function = dbg_function_now, steps, now_function;
	uint32_t op, target;

	dbg_from_sp = dbg_regs[2];
	if (mode != DBG_INTO || dbg_state != DBG_STOPPED || function < 0 || dbg_signal) {
		dbg_go(mode);
		return;
	}
	for (steps = 0; steps < 400; steps++) {
#ifdef __gpuvm__
		op = dbg_peek(DBG_HERE(dbg_regs[0]) + 12);
		if (!DBG_CC(op) && (DBG_SUB(op) == 3 || DBG_SUB(op) == 6)) {	/* callr, call */
			target = DBG_SUB(op) == 6 ? DBG_HERE(dbg_regs[0]) + (uint32_t)(DBG_OFF18(op) * 16) : dbg_regs[DBG_RS(op)] & ~15u;
#else
		op = dbg_peek(dbg_regs[0]);
		if (((op & 0x7f) == 0x6f || (op & 0x7f) == 0x67) && (op >> 7 & 31) == 1) {
			target = (op & 0x7f) == 0x6f ? dbg_regs[0] + (uint32_t)((int32_t)(op & 0x80000000) >> 11 | (int32_t)(op & 0xff000) |
										 (int32_t)(op >> 20 & 1) << 11 | (int32_t)(op >> 21 & 0x3ff) << 1)
						     : (((op >> 15 & 31) ? dbg_regs[op >> 15 & 31] : 0) + (uint32_t)((int32_t)op >> 20)) & ~1u;
#endif
			now_function = dbg_function_at(target);
			if (now_function >= 0 && dbg_entry(now_function))
				dbg_go_to(DBG_RUN, dbg_entry(now_function));
			else
				dbg_go(DBG_OVER);
			return;
		}
		if (!dbg_one()) {
			dbg_fresh = 1;
			return;
		}
		if (dbg_line_of(dbg_regs[0], &now_function) != line || now_function != function)
			break;
	}
	dbg_line_now = dbg_line_of(dbg_regs[0], &dbg_function_now);
	dbg_on_break = 0, dbg_moving = 1;
	dbg_fresh = 1;
}

static void
dbg_end(void)
{
	if (dbg_state == DBG_GONE)
		return;
	kill(dbg_pid, SIGKILL);
	waitpid(dbg_pid, NULL, 0);
	dbg_state = DBG_GONE;
	dbg_stop_count = 0;
}

/* ---- what a variable holds, as text ---- */

static int
dbg_show(int type, uint32_t address, char *to, int room, int depth)
{
	struct dbg_type *t = type < DBG_TYPES ? &dbg_types[type] : &dbg_types[0];
	uint8_t raw[8] = { 0 };
	int n = 0, i;

	if (room < 24)
		return snprintf(to, (size_t)room, "...");
	switch (t->kind) {
	case T_INT:
		dbg_read(address, raw, t->size > 8 ? 8 : t->size);
		if (t->size == 8)
			return snprintf(to, (size_t)room, t->is_unsigned ? "%llu" : "%lld", *(unsigned long long *)raw);
		i = t->size == 1 ? (t->is_unsigned ? raw[0] : (int8_t)raw[0]) : t->size == 2 ? (t->is_unsigned ? *(uint16_t *)raw : *(int16_t *)raw) : *(int32_t *)raw;
		if (t->size == 1 && i >= 32 && i < 127)
			return snprintf(to, (size_t)room, "%d '%c'", i, i);
		return t->is_unsigned && t->size == 4 ? snprintf(to, (size_t)room, "%u", (unsigned)i) : snprintf(to, (size_t)room, "%d", i);
	case T_FLOAT:
		dbg_read(address, raw, t->size > 8 ? 8 : t->size);
		return snprintf(to, (size_t)room, "%g", t->size == 4 ? (double)*(float *)raw : *(double *)raw);
	case T_POINTER:
		dbg_read(address, raw, 4);
		n = snprintf(to, (size_t)room, "0x%x", *(uint32_t *)raw);
		/* a pointer to characters: the text it points at */
		if (*(uint32_t *)raw && t->target < DBG_TYPES && dbg_types[t->target].kind == T_INT && dbg_types[t->target].size == 1) {
			char text[28] = "";

			dbg_read(*(uint32_t *)raw, text, 24);
			for (i = 0; text[i] >= 32 && text[i] < 127; i++)
				continue;
			if (i > 0 && (text[i] == 0 || i == 24))
				n += snprintf(to + n, (size_t)(room - n), " \"%.*s\"%s", i, text, i == 24 ? "..." : "");
		}
		return n;
	case T_ARRAY:
		n = snprintf(to, (size_t)room, "{");
		for (i = 0; i < t->count && i < 8 && n < room - 24; i++) {
			n += snprintf(to + n, (size_t)(room - n), i ? ", " : "");
			n += dbg_show(t->target, address + (uint32_t)(i * (t->target < DBG_TYPES ? dbg_types[t->target].size : 4)), to + n, room - n, depth + 1);
		}
		return n + snprintf(to + n, (size_t)(room - n), i < t->count ? ", ...}" : "}");
	case T_STRUCT:
		if (depth > 1)
			return snprintf(to, (size_t)room, "{...}");
		n = snprintf(to, (size_t)room, "{");
		for (i = 0; i < t->field_count && n < room - 40; i++) {
			n += snprintf(to + n, (size_t)(room - n), "%s%s = ", i ? ", " : "", dbg_fields[t->fields + i].name);
			n += dbg_show(dbg_fields[t->fields + i].type, address + (uint32_t)dbg_fields[t->fields + i].offset, to + n, room - n, depth + 1);
		}
		return n + snprintf(to + n, (size_t)(room - n), i < t->field_count ? ", ...}" : "}");
	}
	return snprintf(to, (size_t)room, "?");
}

/* The variables to show at a stop, a line each: the function's, then the program's.
 * `frame` is the frame pointer they are found from. Returns how many lines. */
static int
dbg_variables(char lines[][160], int most, int function, uint32_t frame)
{
	int pass, i, n = 0, k;

	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < dbg_var_count && n < most; i++) {
			struct dbg_var *v = &dbg_vars[i];

			if (pass == 0 ? v->function != function : v->function >= 0)
				continue;
			if ((v->where == V_FIXED && !v->at) || v->name[0] == '<')
				continue;
			k = snprintf(lines[n], 160, "%s%s = ", pass ? "(global) " : "", v->name);
			dbg_show(v->type, v->where == V_LOCAL ? frame + (uint32_t)v->at : (uint32_t)v->at, lines[n] + k, 160 - k, 0);
			n++;
		}
	return n;
}

/* The functions it is in, the innermost first, a line each ("name, line N"): TinyCC keeps the
 * return address just under the frame's pointer and the caller's frame under that. */
static int
dbg_stack(char lines[][160], int most)
{
	uint32_t pc = dbg_regs[0], frame = dbg_regs[DBG_FP];
	int n = 0, function, line;

	while (n < most) {
		line = dbg_line_of(pc, &function);
		if (function < 0)
			break;
		snprintf(lines[n++], 160, "%s, line %d", dbg_functions[function].name, line);
		if (!strcmp(dbg_functions[function].name, "main"))
			break;
		pc = dbg_peek(frame - 4) - DBG_NEXT;
		frame = dbg_peek(frame - 8);
	}
	return n;
}
#endif
