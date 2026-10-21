/* The translator's command:  xlat flat IN.elf OUT.bflt [STACK]  (a program),  xlat kernel vmlinux OUT.bin [MAP]  (Linux) */
#include "xlat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int xlat_flat(const uint8_t* e, size_t n, uint32_t stack, uint8_t** out, size_t* outn, XlatStats* st, char* err);
int xlat_kernel(const uint8_t* e, size_t n, const char* lds_path, uint8_t** out, size_t* outn, uint32_t* load_at, FILE* map, FILE* words, XlatStats* st, char* err);

static uint8_t* slurp(const char* p, size_t* n) {
    FILE* f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc(*n + 1);
    if (fread(b, 1, *n, f) != *n) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}

static void say(const char* name, const XlatStats* s) {
    printf("%s: %u words, %u blocks, %u ops in %u slots (%.2f an op), %u paired, %u fused, %u illegal, %u outside, "
           "%u tables (%u entries, %u unbounded, %u guessed, %u mapped), %u code addresses\n",
           name, s->words, s->blocks, s->ops, s->slots, s->slots ? (double)s->ops / s->slots : 0, s->paired, s->fused, s->illegal,
           s->outside, s->tables, s->table_entries, s->tables_unbounded, s->tables_guessed, s->tables_missed, s->code_addresses);
}

int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "flat")) {
        size_t n, on;
        uint8_t* e = slurp(argv[2], &n);
        if (!e) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
        uint8_t* o;
        XlatStats st;
        memset(&st, 0, sizeof st);
        char err[256] = "";
        if (!xlat_flat(e, n, argc > 4 ? (uint32_t)strtoul(argv[4], 0, 0) : 0x20000, &o, &on, &st, err)) { fprintf(stderr, "%s: %s\n", argv[2], err); return 1; }
        FILE* f = fopen(argv[3], "wb");
        if (!f || fwrite(o, 1, on, f) != on) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
        fclose(f);
        say(argv[3], &st);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "kernel")) {
        size_t n, on;
        uint8_t* e = slurp(argv[2], &n);
        if (!e) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
        uint8_t* o;
        uint32_t at;
        XlatStats st;
        memset(&st, 0, sizeof st);
        char err[256] = "";
        FILE* map = argc > 4 ? fopen(argv[4], "w") : NULL;
        char wn[4096];
        snprintf(wn, sizeof wn, "%s.words", argc > 4 ? argv[4] : argv[3]);
        FILE* words = argc > 4 ? fopen(wn, "wb") : NULL;
        char lds[4096];   /* the linker script beside vmlinux, as the kernel's build leaves it */
        snprintf(lds, sizeof lds, "%.*sarch/riscv/kernel/vmlinux.lds", (int)(strrchr(argv[2], '/') ? strrchr(argv[2], '/') - argv[2] + 1 : 0), argv[2]);
        if (!xlat_kernel(e, n, lds, &o, &on, &at, map, words, &st, err)) { fprintf(stderr, "%s: %s\n", argv[2], err); return 1; }
        if (map) fclose(map);
        if (words) fclose(words);
        FILE* f = fopen(argv[3], "wb");
        if (!f || fwrite(o, 1, on, f) != on) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
        fclose(f);
        say(argv[3], &st);
        printf("%s: %zu bytes from %#x\n", argv[3], on, at);
        return 0;
    }
    fprintf(stderr, "xlat flat IN.elf OUT.bflt [STACK]\nxlat kernel vmlinux OUT.bin [MAP]\n");
    return 2;
}
