/* elf2flt: a position-independent RV32 ELF (linked at 0, as tcc -shared or ld -pie leave it) to a
 * bFLT v4 program, loaded whole into RAM (elf2bflt.py, the same in C for the machine).
 *   elf2flt IN OUT [STACK] */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint32_t rd(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static void wr_be(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void wr_le(uint8_t *p, uint32_t v) { p[3] = v >> 24; p[2] = v >> 16; p[1] = v >> 8; p[0] = v; }

static int fail(const char *why) { fprintf(stderr, "elf2flt: %s\n", why); return 1; }

int main(int argc, char **argv)
{
	FILE *f;
	uint8_t *e, *img, hdr[64];
	long n;
	uint32_t phoff, phnum, phent, entry, end_file = 0, end_mem = 0, data_start = 0, rela = 0, relasz = 0, symtab = 0, strtab = 0, hash = 0;
	uint32_t i, k, nrel = 0, *rel, stack = argc > 3 ? strtoul(argv[3], NULL, 0) : 0x40000;
	int dyn = -1;

	if (argc < 3)
		return fail("usage: elf2flt IN OUT [STACK]");
	if (!(f = fopen(argv[1], "rb")))
		return fail("cannot read the program");
	fseek(f, 0, SEEK_END), n = ftell(f), fseek(f, 0, SEEK_SET);
	e = malloc(n);
	if (!e || fread(e, 1, n, f) != (size_t)n)
		return fail("cannot read the program");
	fclose(f);
	if (memcmp(e, "\177ELF\1\1", 6) || rd16(e + 18) != 243)
		return fail("not a little-endian RV32 ELF");
	entry = rd(e + 24), phoff = rd(e + 28), phent = rd16(e + 42), phnum = rd16(e + 44);
	for (i = 0; i < phnum; i++) {
		const uint8_t *p = e + phoff + i * phent;
		uint32_t type = rd(p), va = rd(p + 8), fs = rd(p + 16), ms = rd(p + 20), fl = rd(p + 24);

		if (type == 1) {
			if (va + fs > end_file) end_file = va + fs;
			if (va + ms > end_mem) end_mem = va + ms;
			if ((fl & 2) && (!data_start || va < data_start)) data_start = va;
		} else if (type == 2)
			dyn = (int)i;
		else if (type == 7 && ms)
			fprintf(stderr, "elf2flt: WARNING: %s has thread-local storage\n", argv[1]);
	}
	img = calloc(1, end_file + 4);
	if (!img)
		return fail("no memory");
	for (i = 0; i < phnum; i++) {
		const uint8_t *p = e + phoff + i * phent;

		if (rd(p) == 1)
			memcpy(img + rd(p + 8), e + rd(p + 4), rd(p + 16));
	}
	data_start = (data_start ? data_start : end_file) & ~31u;
	/* binfmt_flat stores the data's start into the word just below it (fs/binfmt_flat.c's shared-library table,
	 * BINFMT_FLAT_NO_DATA_START_OFFSET or not): where a section lies there (tcc's .eh_frame_hdr runs to its data), the
	 * data starts lower, at the first 32 bytes with a word of nothing below (loaded whole, the split moves no
	 * address); with none, the program is refused (elf2bflt.py) */
	{
		uint32_t shoff = rd(e + 32), shent = rd16(e + 46), shnum = rd16(e + 48), start = data_start, hit = 1, least = 4;

		for (i = 0; i < phnum; i++) {	/* (not into the text: the end of what is executed) */
			const uint8_t *p = e + phoff + i * phent;

			if (rd(p) == 1 && (rd(p + 24) & 1) && rd(p + 8) + rd(p + 20) > least)
				least = rd(p + 8) + rd(p + 20);
		}
		while (data_start >= least && hit) {
			for (i = 0, hit = 0; !hit && i < (shoff ? shnum : phnum); i++) {
				const uint8_t *s = shoff ? e + shoff + i * shent : e + phoff + i * phent;
				/* a section: ALLOC, not TLS, with a size; else a loaded segment (sh_size and p_memsz alike) */
				int is = shoff ? (rd(s + 8) & 2) && !(rd(s + 8) & 0x400) && rd(s + 20) : rd(s) == 1;
				uint32_t at = shoff ? rd(s + 12) : rd(s + 8);

				hit = is && at < data_start && at + rd(s + 20) > data_start - 4;
			}
			if (hit)
				data_start -= 32;
		}
		if (data_start < least) {
			fprintf(stderr, "elf2flt: every 32 bytes from the text's end to the data's (%#x) has a section below it, which the kernel overwrites\n", start);
			return 1;
		}
	}
	if (dyn >= 0) {
		const uint8_t *p = e + phoff + dyn * phent;

		for (k = 0; k < rd(p + 16); k += 8) {
			int32_t tag = (int32_t)rd(e + rd(p + 4) + k);
			uint32_t val = rd(e + rd(p + 4) + k + 4);

			if (tag == 0) break;
			if (tag == 7) rela = val;
			if (tag == 8) relasz = val;
			if (tag == 6) symtab = val;
			if (tag == 5) strtab = val;
			if (tag == 4) hash = val;
		}
	}
	/* a library's link (tcc -shared) names no entry: the program starts at _start */
	for (k = 0; !entry && symtab && strtab && hash && k < rd(img + hash + 4); k++)
		if (!strcmp((const char *)img + strtab + rd(img + symtab + 16 * k), "_start"))
			entry = rd(img + symtab + 16 * k + 4);
	if (!entry)
		return fail("no entry point (_start)");
	rel = malloc((relasz / 12 + 1) * 4);
	for (k = 0; k < relasz; k += 12) {
		uint32_t off = rd(img + rela + k), info = rd(img + rela + k + 4), add = rd(img + rela + k + 8);
		uint32_t kind = info & 0xff, sym = info >> 8;

		if (kind == 0)
			continue;
		if (kind == 1 || kind == 5) {	/* a word, or a PLT slot, naming a symbol */
			const uint8_t *s = img + symtab + 16 * sym;

			if (!symtab)
				return fail("a relocation with no symbols");
			if (!rd16(s + 14)) {
				if (s[12] >> 4 != 2) {	/* only a weak one may be missing: it is 0, no relocation */
					fprintf(stderr, "elf2flt: %s is not defined\n", strtab ? (const char *)img + strtab + rd(s) : "a symbol");
					return 1;
				}
				img[off] = add, img[off + 1] = add >> 8, img[off + 2] = add >> 16, img[off + 3] = add >> 24;
				continue;
			}
			add += rd(s + 4);
		} else if (kind != 3)
			return fail("a relocation other than R_RISCV_32, R_RISCV_RELATIVE and R_RISCV_JUMP_SLOT");
		wr_le(img + off, add);
		rel[nrel++] = off;
	}
	/* the loader takes addresses from the end of the 64-byte header, its entry from the file's start */
	memset(hdr, 0, sizeof hdr);
	memcpy(hdr, "bFLT", 4);
	wr_be(hdr + 4, 4), wr_be(hdr + 8, 64 + entry), wr_be(hdr + 12, 64 + data_start), wr_be(hdr + 16, 64 + end_file);
	wr_be(hdr + 20, 64 + end_mem), wr_be(hdr + 24, stack), wr_be(hdr + 28, 64 + end_file), wr_be(hdr + 32, nrel), wr_be(hdr + 36, 1 | 0x40000000);	/* the relocations in the machine's order: patch_flatreloc.py */
	if (!(f = fopen(argv[2], "wb")))
		return fail("cannot write the program");
	fwrite(hdr, 1, 64, f);
	fwrite(img, 1, end_file, f);
	for (i = 0; i < nrel; i++) {
		uint8_t b[4];

		wr_le(b, rel[i]);
		fwrite(b, 1, 4, f);
	}
	if (fclose(f))
		return fail("cannot write the program");
	chmod(argv[2], 0755);
	return 0;
}
