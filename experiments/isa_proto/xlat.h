/* The toolchain's back end (xlat.c): RISC-V code in a linked program made the machine's slots. */
#ifndef XLAT_H
#define XLAT_H
#include <stdint.h>
#include "isa.h"

typedef struct {
    const uint8_t* bytes;     /* the program's bytes from addr on */
    uint32_t addr, size;      /* the span that holds code (a multiple of 4) */
    const uint8_t* is_code;   /* a byte a word: 1 where the span's word is code (NULL: all of it) */
    const uint32_t* entries;  /* addresses code is entered at that no branch names (entry, pointers in data) */
    uint32_t nentries;
    int (*read_data)(void* ctx, uint32_t addr, uint32_t* word);   /* the program's words, for switch tables */
    void* ctx;
    const uint8_t* keep;      /* a byte a word: 1 where an auipc names memory, not code (a range's bound): no slot's */
} XlatCode;

enum { XLAT_REL = 1 };   /* a word that becomes (the target's slot - base): a switch table's entry */
typedef struct { uint32_t addr, value, base, kind; } XlatPatch;

typedef struct {
    uint32_t words, blocks, ops, slots, paired, fused, illegal, outside, tables, table_entries, tables_unbounded, tables_guessed, tables_missed, maps, code_addresses;
} XlatStats;

typedef struct {
    uint8_t* slots;           /* 16 bytes each, to be placed at the base given */
    uint32_t nslots;
    uint32_t* native;         /* each code word's slot address (0 for a word that is not code) */
    XlatPatch* patches;       /* switch tables' entries to write */
    uint32_t npatches;
} XlatResult;

XlatResult xlat_code(const XlatCode* in, uint32_t base, XlatStats* st);
void xlat_free(XlatResult* r);
/* An address in the code span as its slot's address; any other address as it is. */
uint32_t xlat_map(const XlatCode* in, const XlatResult* r, uint32_t addr);

#endif
