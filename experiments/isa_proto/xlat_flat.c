/* A static-PIE RV32 ELF (linked at 0 or 0x40) to a bFLT v4 program whose code is the machine's slots: the image
 * as elf2bflt.py lays it out (linux/nommu), the slots after its bss (FLAT_FLAG_NATIVE, ISA.md "Programs"). */
#include "xlat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define FLAT_FLAG_NATIVE 0x100u
#define H 0x40u

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void be32(uint8_t* p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }

typedef struct { uint8_t* img; uint32_t size; } Img;
static int img_word(void* ctx, uint32_t a, uint32_t* w) {
    Img* m = (Img*)ctx;
    if (a + 4 > m->size || a + 4 < a) return 0;
    *w = rd32(m->img + a);
    return 1;
}

int xlat_flat(const uint8_t* e, size_t n, uint32_t stack, uint8_t** out, size_t* outn, XlatStats* st, char* err) {
    if (n < 52 || memcmp(e, "\177ELF\1\1", 6)) { strcpy(err, "not a little-endian ELF32"); return 0; }
    uint32_t entry = rd32(e + 24), phoff = rd32(e + 28), shoff = rd32(e + 32);
    uint16_t phes = rd16(e + 42), phn = rd16(e + 44), shes = rd16(e + 46), shn = rd16(e + 48);
    uint32_t end_file = 0, end_mem = 0, first = ~0u, first_off = 0, data_start = ~0u, dyn_off = 0, dyn_sz = 0;
    for (int i = 0; i < phn; i++) {
        const uint8_t* p = e + phoff + i * phes;
        uint32_t type = rd32(p), off = rd32(p + 4), va = rd32(p + 8), fs = rd32(p + 16), ms = rd32(p + 20), fl = rd32(p + 24);
        if (type == 1) {
            if (va + fs > end_file) end_file = va + fs;
            if (va + ms > end_mem) end_mem = va + ms;
            if (va < first) first = va, first_off = off;
            if ((fl & 2) && va < data_start) data_start = va;
        } else if (type == 2) dyn_off = off, dyn_sz = fs;
    }
    if (first != 0 && first != 0x40) { sprintf(err, "the first segment must be linked at 0 or 0x40, not %#x", first); return 0; }
    if (first == 0 && first_off != 0) { strcpy(err, "a segment at 0 must start with the ELF header"); return 0; }
    if (data_start == ~0u) data_start = end_file;
    data_start &= ~31u;
    Img m = {(uint8_t*)calloc(end_file + 4, 1), end_file};
    for (int i = 0; i < phn; i++) {
        const uint8_t* p = e + phoff + i * phes;
        if (rd32(p) != 1) continue;
        uint32_t off = rd32(p + 4), va = rd32(p + 8), fs = rd32(p + 16);
        memcpy(m.img + va, e + off, fs);
    }
    /* the code: the executable sections, as one span with its gaps marked */
    uint32_t lo = ~0u, hi = 0;
    for (int i = 0; i < shn; i++) {
        const uint8_t* s = e + shoff + i * shes;
        if ((rd32(s + 8) & 6) == 6 && rd32(s + 4) == 1 && rd32(s + 20)) {   /* SHF_ALLOC | SHF_EXECINSTR, PROGBITS */
            uint32_t a = rd32(s + 12), z = a + rd32(s + 20);
            if (a < lo) lo = a;
            if (z > hi) hi = z;
        }
    }
    if (lo == ~0u) { strcpy(err, "no code"); free(m.img); return 0; }
    lo &= ~3u, hi = (hi + 3) & ~3u;
    uint8_t* is_code = (uint8_t*)calloc((hi - lo) / 4 + 1, 1);
    for (int i = 0; i < shn; i++) {
        const uint8_t* s = e + shoff + i * shes;
        if ((rd32(s + 8) & 6) == 6 && rd32(s + 4) == 1)
            for (uint32_t a = rd32(s + 12) & ~3u; a < rd32(s + 12) + rd32(s + 20); a += 4) is_code[(a - lo) / 4] = 1;
    }
    /* the relocations: words that hold an address once loaded; those naming code are where code is entered */
    uint32_t rela = 0, relasz = 0, symtab = 0, nrel = 0;
    for (uint32_t k = 0; dyn_sz && k + 8 <= dyn_sz; k += 8) {
        int32_t tag = (int32_t)rd32(e + dyn_off + k);
        uint32_t val = rd32(e + dyn_off + k + 4);
        if (!tag) break;
        if (tag == 7) rela = val;
        if (tag == 8) relasz = val;
        if (tag == 6) symtab = val;
    }
    uint32_t* rel_at = (uint32_t*)malloc((relasz / 12 + 1) * 4);
    uint32_t* rel_val = (uint32_t*)malloc((relasz / 12 + 1) * 4);
    uint32_t* entries = (uint32_t*)malloc((relasz / 12 + 2) * 4);
    uint32_t nent = 0;
    entries[nent++] = entry;
    for (uint32_t k = 0; rela && k < relasz; k += 12) {
        uint32_t off = rd32(m.img + rela + k), info = rd32(m.img + rela + k + 4), add = rd32(m.img + rela + k + 8);
        uint32_t kind = info & 0xff, sym = info >> 8;
        if (kind != 1 && kind != 3) { sprintf(err, "relocation %u at %#x: only R_RISCV_32 and R_RISCV_RELATIVE", kind, off); goto fail; }
        if (kind == 1) {
            const uint8_t* s = m.img + symtab + 16 * sym;
            if (rd16(s + 14) == 0) {   /* a weak symbol nothing defines: 0, no relocation */
                memcpy(m.img + off, &add, 4);
                continue;
            }
            add += rd32(s + 4);
        }
        rel_at[nrel] = off, rel_val[nrel++] = add;
        if (add - lo < hi - lo) entries[nent++] = add;
    }
    {
        XlatCode in = {m.img + lo, lo, hi - lo, is_code, entries, nent, img_word, &m, NULL};
        uint32_t nb = (end_mem + 15) & ~15u;
        XlatResult r = xlat_code(&in, nb, st);
        uint32_t nlen = r.nslots * 16;
        for (uint32_t k = 0; k < r.npatches; k++) {
            uint32_t v = r.patches[k].value - r.patches[k].base;
            memcpy(m.img + r.patches[k].addr, &v, 4);
        }
        /* the file: header, image, relocations (big-endian words, values until relocated), slots */
        size_t total = H + end_file + 4 * (size_t)nrel + nlen;
        uint8_t* o = (uint8_t*)calloc(total, 1);
        for (uint32_t k = 0; k < nrel; k++) {
            uint32_t v = xlat_map(&in, &r, rel_val[k]);
            be32(m.img + rel_at[k], v);
            be32(o + H + end_file + 4 * k, rel_at[k]);
        }
        memcpy(o, "bFLT", 4);
        be32(o + 4, 4);
        be32(o + 8, H + xlat_map(&in, &r, entry));
        be32(o + 12, H + data_start);
        be32(o + 16, H + end_file);
        be32(o + 20, H + end_mem);
        be32(o + 24, stack);
        be32(o + 28, H + end_file);
        be32(o + 32, nrel);
        be32(o + 36, 1 | FLAT_FLAG_NATIVE);
        be32(o + 44, H + nb);      /* filler[0]: where the slots go, as the other offsets */
        be32(o + 48, nlen);        /* filler[1]: their length; they follow the relocations in the file */
        memcpy(o + H, m.img, end_file);
        memcpy(o + H + end_file + 4 * (size_t)nrel, r.slots, nlen);
        *out = o, *outn = total;
        xlat_free(&r);
    }
    free(m.img); free(is_code); free(rel_at); free(rel_val); free(entries);
    return 1;
fail:
    free(m.img); free(is_code); free(rel_at); free(rel_val); free(entries);
    return 0;
}
