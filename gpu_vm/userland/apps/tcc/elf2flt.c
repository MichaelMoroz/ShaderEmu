/* elf2flt: a gpu_vm program TinyCC linked as a library at 0 (tcc -shared) to a bFLT v4 program, loaded whole into RAM
 * (gpu_vm/userland/elf2flt, the same for ld.lld's links, in C for the machine; linux/nommu/elf2flt.c is RISC-V's).
 * Its absolute words are the library's R_GVM_RELATIVE relocations.
 *   elf2flt IN OUT [STACK] */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define EM_GVM 0x4756
#define R_GVM_32 1
#define R_GVM_RELATIVE 9

static uint32_t rd(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static void wr_be(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static int fail(const char *why) { fprintf(stderr, "elf2flt: %s\n", why); return 1; }

int main(int argc, char **argv)
{
	FILE *f;
	uint8_t *e, *img, hdr[64];
	long n;
	uint32_t phoff, phnum, phent, entry, end_file = 0, end_mem = 0, data_start = 0, ro_end = 0, rela = 0, relasz = 0;
	uint32_t symtab = 0, strtab = 0, hash = 0, i, k, nrel = 0, *rel, stack = argc > 3 ? strtoul(argv[3], NULL, 0) : 0x40000;
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
	if (memcmp(e, "\177ELF\1\1", 6) || rd16(e + 18) != EM_GVM)
		return fail("not a gpu_vm ELF");
	entry = rd(e + 24), phoff = rd(e + 28), phent = rd16(e + 42), phnum = rd16(e + 44);
	for (i = 0; i < phnum; i++) {
		const uint8_t *p = e + phoff + i * phent;
		uint32_t type = rd(p), va = rd(p + 8), fs = rd(p + 16), ms = rd(p + 20), fl = rd(p + 24);

		if (type == 1) {
			if (va + fs > end_file) end_file = va + fs;
			if (va + ms > end_mem) end_mem = va + ms;
			if ((fl & 2) && (!data_start || va < data_start)) data_start = va;
			if (!(fl & 2) && va + ms > ro_end) ro_end = va + ms;
		} else if (type == 2)
			dyn = (int)i;
		else if (type == 7 && ms)
			return fail("thread-local storage (TinyCC makes none; a library's would need __gvm_phdr)");
	}
	img = calloc(1, end_file + 4);
	if (!img)
		return fail("no memory");
	for (i = 0; i < phnum; i++) {
		const uint8_t *p = e + phoff + i * phent;

		if (rd(p) == 1)
			memcpy(img + rd(p + 8), e + rd(p + 4), rd(p + 16));
	}
	/* the file's data starts on 32 bytes; binfmt_flat writes the data's start into the word below it, which must be
	   nothing of the program's (tcc puts the writable segment on a page of its own) */
	data_start = (data_start ? data_start : end_file) & ~31u;
	if (data_start && data_start - 4 < ro_end)
		return fail("the word below the data is the program's (binfmt_flat writes it)");
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
	/* a library's link names no entry: the program starts at _start */
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
		if (kind == R_GVM_32) {	/* a word naming a symbol */
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
		} else if (kind != R_GVM_RELATIVE)
			return fail("a relocation other than R_GVM_32 and R_GVM_RELATIVE");
		if (!add) {	/* binfmt_flat leaves a word of 0 alone */
			img[off] = img[off + 1] = img[off + 2] = img[off + 3] = 0;
			continue;
		}
		wr_be(img + off, add);	/* big-endian until relocated */
		rel[nrel++] = off;
	}
	/* the loader takes addresses from the end of the 64-byte header, its entry from the file's start */
	memset(hdr, 0, sizeof hdr);
	memcpy(hdr, "bFLT", 4);
	wr_be(hdr + 4, 4), wr_be(hdr + 8, 64 + entry), wr_be(hdr + 12, 64 + data_start), wr_be(hdr + 16, 64 + end_file);
	wr_be(hdr + 20, 64 + end_mem), wr_be(hdr + 24, stack), wr_be(hdr + 28, 64 + end_file), wr_be(hdr + 32, nrel), wr_be(hdr + 36, 1);
	if (!(f = fopen(argv[2], "wb")))
		return fail("cannot write the program");
	fwrite(hdr, 1, 64, f);
	fwrite(img, 1, end_file, f);
	for (i = 0; i < nrel; i++) {
		uint8_t b[4];

		wr_be(b, rel[i]);
		fwrite(b, 1, 4, f);
	}
	if (fclose(f))
		return fail("cannot write the program");
	chmod(argv[2], 0755);
	return 0;
}
