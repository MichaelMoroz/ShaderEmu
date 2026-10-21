/* vmlinux (linked with --emit-relocs, kernel/patch_native.py) to the machine's kernel: the image from its first
 * address, the code's slots at __native_start, every word of data that names code made to name its slot, and a
 * slot at the image's start that jumps to the kernel's entry (ISA.md, "Linux"). */
#include "xlat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

typedef struct { uint8_t* img; uint32_t lo, size; } Img;
static int img_word(void* ctx, uint32_t a, uint32_t* w) {
    Img* m = (Img*)ctx;
    if (a - m->lo + 4 > m->size || a < m->lo) return 0;
    *w = rd32(m->img + (a - m->lo));
    return 1;
}

typedef struct { uint32_t at, value; int sign; } Fix;   /* a data word's part that names value */

static char* slurp_text(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = (char*)malloc(*n + 1);
    b[fread(b, 1, *n, f)] = 0;
    fclose(f);
    return b;
}

int xlat_kernel(const uint8_t* e, size_t n, const char* lds_path, uint8_t** out, size_t* outn, uint32_t* load_at, FILE* map, FILE* words, XlatStats* st, char* err) {
    if (n < 52 || memcmp(e, "\177ELF\1\1", 6) || rd16(e + 16) != 2) { strcpy(err, "not a little-endian ELF32 executable"); return 0; }
    uint32_t entry = rd32(e + 24), phoff = rd32(e + 28), shoff = rd32(e + 32);
    uint16_t phes = rd16(e + 42), phn = rd16(e + 44), shes = rd16(e + 46), shn = rd16(e + 48), shstr = rd16(e + 50);
    uint32_t lo = ~0u, end_file = 0;
    for (int i = 0; i < phn; i++) {
        const uint8_t* p = e + phoff + i * phes;
        if (rd32(p) != 1 || !rd32(p + 20)) continue;
        uint32_t va = rd32(p + 8), fs = rd32(p + 16);
        if (va < lo) lo = va;
        if (va + fs > end_file) end_file = va + fs;
    }
    /* the symbols: the room for the slots, and every label in code as a place code is entered */
    const uint8_t *syms = NULL, *strs = NULL;
    uint32_t nsyms = 0, nstart = 0, nend = 0;
    const uint8_t* sec_names = e + rd32(e + shoff + shstr * shes + 16);
    for (int i = 0; i < shn; i++) {
        const uint8_t* s = e + shoff + i * shes;
        if (rd32(s + 4) == 2) {
            syms = e + rd32(s + 16), nsyms = rd32(s + 20) / 16;
            strs = e + rd32(e + shoff + rd32(s + 24) * shes + 16);
        }
    }
    if (!syms) { strcpy(err, "no symbols"); return 0; }
    for (uint32_t k = 0; k < nsyms; k++) {
        const char* name = (const char*)strs + rd32(syms + 16 * k);
        if (!strcmp(name, "__native_start")) nstart = rd32(syms + 16 * k + 4);
        if (!strcmp(name, "__native_end")) nend = rd32(syms + 16 * k + 4);
    }
    if (!nstart || nend <= nstart) { strcpy(err, "no __native_start and __native_end (patch_native.py)"); return 0; }
    Img m = {(uint8_t*)calloc(nend - lo, 1), lo, nend - lo};
    for (int i = 0; i < phn; i++) {
        const uint8_t* p = e + phoff + i * phes;
        if (rd32(p) != 1) continue;
        memcpy(m.img + (rd32(p + 8) - lo), e + rd32(p + 4), rd32(p + 16));
    }
    /* the code: the executable sections, as one span with its gaps marked */
    uint32_t clo = ~0u, chi = 0;
    for (int i = 0; i < shn; i++) {
        const uint8_t* s = e + shoff + i * shes;
        if ((rd32(s + 8) & 6) == 6 && rd32(s + 4) == 1 && rd32(s + 20)) {
            uint32_t a = rd32(s + 12), z = a + rd32(s + 20);
            if (a < clo) clo = a;
            if (z > chi) chi = z;
        }
    }
    clo &= ~3u, chi = (chi + 3) & ~3u;
    uint8_t* is_code = (uint8_t*)calloc((chi - clo) / 4 + 1, 1);
    for (int i = 0; i < shn; i++) {
        const uint8_t* s = e + shoff + i * shes;
        if ((rd32(s + 8) & 6) == 6 && rd32(s + 4) == 1)
            for (uint32_t a = rd32(s + 12) & ~3u; a < rd32(s + 12) + rd32(s + 20); a += 4) is_code[(a - clo) / 4] = 1;
    }
#define CODE(v) ((v) - clo < chi - clo && !((v) & 3) && is_code[((v) - clo) / 4])
    /* the symbols the linker script defines, and _start: the bounds of memory, never a slot's address */
    uint8_t* bound = (uint8_t*)calloc(nsyms + 1, 1);
    uint8_t* keep = (uint8_t*)calloc((chi - clo) / 4 + 1, 1);
    {
        size_t ln = 0;
        char* lds = lds_path ? (char*)slurp_text(lds_path, &ln) : NULL;
        if (!lds) { sprintf(err, "no linker script %s", lds_path ? lds_path : "(none)"); free(bound); free(keep); free(m.img); free(is_code); return 0; }
        for (uint32_t k = 0; k < nsyms; k++) {
            const char* name = (const char*)strs + rd32(syms + 16 * k);
            size_t nl = strlen(name);
            if (!nl) continue;
            if (!strcmp(name, "_start")) { bound[k] = 1; continue; }
            for (const char* p = strstr(lds, name); p && !bound[k]; p = strstr(p + 1, name)) {
                const char* q = p + nl;
                int before = p == lds || !(p[-1] == '_' || p[-1] == '.' || (p[-1] >= '0' && p[-1] <= '9') || ((p[-1] | 32) >= 'a' && (p[-1] | 32) <= 'z'));
                while (*q == ' ' || *q == '\t') q++;
                if (before && q[0] == '=' && q[1] != '=') bound[k] = 1;
            }
        }
        free(lds);
    }
    uint32_t cap = 1 << 16, nent = 0, nfix = 0, fcap = 1 << 16, odd = 0, in_code_words = 0;
    uint32_t* entries = (uint32_t*)malloc(cap * 4);
    Fix* fix = (Fix*)malloc(fcap * sizeof(Fix));
#define ENTRY(v) do { if (nent == cap) entries = (uint32_t*)realloc(entries, (cap *= 2) * 4); entries[nent++] = (v); } while (0)
    ENTRY(entry);
    for (uint32_t k = 0; k < nsyms; k++) {
        uint32_t v = rd32(syms + 16 * k + 4);
        uint8_t info = syms[16 * k + 12];   /* functions, and global labels (asm's ENTRY); not .L labels */
        if (CODE(v) && ((info & 15) == 2 || ((info & 15) == 0 && (info >> 4) != 0))) ENTRY(v);
    }
    /* the relocations the link kept: in code, where it is entered; in data, what names code */
    for (int i = 0; i < shn; i++) {
        const uint8_t* s = e + shoff + i * shes;
        if (rd32(s + 4) != 4) continue;
        const uint8_t* to = e + shoff + rd32(s + 28) * shes;
        if (!(rd32(to + 8) & 2)) continue;   /* (debugging sections) */
        int code = (rd32(to + 8) & 4) != 0;
        const uint8_t* r = e + rd32(s + 16);
        for (uint32_t k = 0; k < rd32(s + 20) / 12; k++, r += 12) {
            uint32_t at = rd32(r), info = rd32(r + 4), kind = info & 0xff, sym = info >> 8;
            uint32_t v = rd32(syms + 16 * sym + 4) + rd32(r + 8);
            if (code) {
                if (kind == 23 && bound[sym] && CODE(at)) keep[(at - clo) / 4] = 1;
                if ((kind == 17 || kind == 18 || kind == 19) && CODE(v)) ENTRY(v);   /* (calls and jumps: functions) */
                if ((kind == 26 || kind == 1) && CODE(v)) odd++;   /* an absolute code address in code: not made */
                if (kind == 1 && CODE(at)) in_code_words++;
                continue;
            }
            int sign = kind == 1 || kind == 35 || kind == 56 || kind == 57 ? 1 : kind == 39 ? -1 : 0;
            if (!sign) {
                if (kind != 51 && kind != 34 && kind != 38 && kind != 33 && kind != 37 && kind != 52 && kind != 53 && kind != 54 && kind != 55) {
                    odd++;
                    if (getenv("XLAT_WHY")) fprintf(stderr, "  kind %u at %#x in %s\n", kind, at, (const char*)sec_names + rd32(to));
                }
                continue;
            }
            if (!CODE(v) || bound[sym]) continue;
            if (nfix == fcap) fix = (Fix*)realloc(fix, (fcap *= 2) * sizeof(Fix));
            fix[nfix++] = (Fix){at, v, sign};
            ENTRY(v);
        }
    }
    if (odd) fprintf(stderr, "xlat kernel: %u relocations not made (absolute code addresses in code, or of kinds not known)\n", odd);
    if (in_code_words) fprintf(stderr, "xlat kernel: %u words in code named by relocations (data in code)\n", in_code_words);
    XlatCode in = {m.img + (clo - lo), clo, chi - clo, is_code, entries, nent, img_word, &m, keep};
    XlatResult res = xlat_code(&in, nstart, st);
    if (nstart + 16ull * res.nslots > nend) {
        sprintf(err, "the slots take %u bytes, the room is %u (patch_native.py's NATIVE_ROOM)", 16 * res.nslots, nend - nstart);
        goto fail;
    }
    /* (a switch's table is named by the relocations: the translator's own patches of it are not made) */
    for (uint32_t k = 0; k < nfix; k++) {
        uint32_t w = rd32(m.img + (fix[k].at - lo)) + (uint32_t)fix[k].sign * (xlat_map(&in, &res, fix[k].value) - fix[k].value);
        memcpy(m.img + (fix[k].at - lo), &w, 4);
    }
    memcpy(m.img + (nstart - lo), res.slots, 16 * (size_t)res.nslots);
    {   /* the image's first slot: a jump to the entry's */
        uint32_t boot[4] = {K_INT | X_BR | X_ALW | 1u << X_CNT_SHIFT, ISA_SINK << 10, ISA_SINK, xlat_map(&in, &res, entry) - lo};
        memcpy(m.img, boot, 16);
    }
    if (map)
        for (uint32_t k = 0; k < nsyms; k++) {
            uint32_t v = rd32(syms + 16 * k + 4);
            if (CODE(v) && (syms[16 * k + 12] & 15) <= 2) fprintf(map, "%08x %08x %s\n", xlat_map(&in, &res, v), v, (const char*)strs + rd32(syms + 16 * k));
        }
    if (words) {   /* every word of code's slot: (first address, words, then one each; 0 for no code) */
        uint32_t head[2] = {clo, (chi - clo) / 4};
        fwrite(head, 4, 2, words);
        fwrite(res.native, 4, head[1], words);
    }
    *load_at = lo;
    *outn = nstart + 16 * (size_t)res.nslots - lo;
    *out = m.img;
    xlat_free(&res);
    free(is_code); free(entries); free(fix); free(bound); free(keep);
    return 1;
fail:
    xlat_free(&res);
    free(m.img); free(is_code); free(entries); free(fix); free(bound); free(keep);
    return 0;
}
