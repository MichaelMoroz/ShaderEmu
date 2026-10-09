#ifndef MMU_H
#define MMU_H

#include "types.h"

// M_MODE_ONLY: machine mode is the only privilege level. No supervisor or user mode, no trap
// delegation, no sret; MPP is hardwired to machine. It includes NO_PAGING. For firmware-less
// programs; OpenSBI and anything it starts need the supervisor.
#if defined(M_MODE_ONLY) && !defined(NO_PAGING)
#define NO_PAGING 1
#endif

// Pass-local shadows of state that is read on every instruction. They are never stored in
// the state texture: each pass starts with them empty.
static bool hot_mstatus_ok = false, hot_mip_ok = false, hot_mie_ok = false;
static uint hot_mstatus, hot_mip, hot_mie;
// NO_PAGING: a machine whose satp is hardwired to 0 (Bare), as the privileged spec allows.
// Every address is physical, so there are no TLBs and no translation state at all. Linux
// needs paging; bare-metal programs and OpenSBI payloads that stay in Bare mode do not.
#ifndef NO_PAGING
#define TLB_EMPTY uint4(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff)
static uint4 tlb_f_vpn = TLB_EMPTY, tlb_r_vpn = TLB_EMPTY, tlb_w_vpn = TLB_EMPTY;  // slot = vpn & 3
static uint4 tlb_f_page, tlb_r_page, tlb_w_page;
#endif
// True while no interrupt can be pending-and-enabled and the UART has nothing to flush, so the
// per-instruction interrupt/UART step would do nothing. Cleared by anything that feeds it:
// CSR writes and MMIO writes. Starts false, so each pass re-establishes it.
static bool irq_quiet = false;
static bool irq_last_handled = false;  // did the last interrupt/trap step take a trap
static uint fw_addr0 = 0xffffffff, fw_addr1 = 0xffffffff;  // instruction window: current and next texel
static uint4 fw_tex0, fw_tex1;
#ifndef NO_PAGING
static uint fetch_vpn = 0xffffffff, fetch_page;  // last translated fetch page (OPT_FETCH_FAST)
// Second-level TLB: TLB2_N direct-mapped entries per access mode, behind the small first level.
// A tag is the page number, the privilege context (effective privilege, SUM, MXR) and a
// generation: entries outlive traps and are dropped together when mappings change (satp,
// sfence.vma). A zero tag never matches. 64 entries were too few for a program like Doom.
#ifndef TLB2_N
#define TLB2_N 256
#endif
// With L1_LOCAL the two arrays are locals of the tick's main, handed down like the write cache
// (l1_local.h), and start undefined. Either way an entry counts only when its bit in tlb2_occ
// says it was written in this pass, and a bit in tlb2_dead says that what the last pass left
// in the texture for that entry has been flushed since.
#if !(defined(L1_LOCAL) && defined(PASS_TICK))
static uint tlb2_tag[3 * TLB2_N];
static uint tlb2_pg[3 * TLB2_N];
#endif
#define TLB2_WORDS ((3 * TLB2_N + 31) / 32)
static uint tlb2_occ[TLB2_WORDS];
static uint tlb2_dead[TLB2_WORDS];
#define TLB2_OCC(k) (((tlb2_occ[(k) >> 5] >> ((k) & 31)) & 1) != 0)
#define TLB2_OCC_SET(k) tlb2_occ[(k) >> 5] |= 1u << ((k) & 31);
#define TLB2_DEAD(k) (((tlb2_dead[(k) >> 5] >> ((k) & 31)) & 1) != 0)
#define TLB2_HIT(k, tag) (TLB2_OCC(k) && tlb2_tag[k] == (tag))
static uint tlb2_gen = 1;
static bool tlb_wiped = false;   // the generations wrapped in this pass: what the last pass left is void
// Megapage TLB, behind the second level: one entry maps 4 MiB, which is how a kernel maps its
// own memory. Without it every 4 KiB page of such a mapping costs a page walk of its own, and
// the TLBs start empty on every pass. Same tags and generation as the second level.
#define TLBM_N 16
static uint tlbm_tag[3 * TLBM_N];
static uint tlbm_pg[3 * TLBM_N];
#define TLBM_IDX(mode, va) ((mode) * TLBM_N + (((va) >> 22) & (TLBM_N - 1)))
#define TLBM_TAG(va, ctx) (((va) >> 22) | ((ctx) << 20) | (tlb2_gen << 24))
static uint xl_ctx = 0;  // current privilege context, refreshed by the general path
#define TLB2_IDX(mode, va) ((mode) * TLB2_N + (((va) >> 12) & (TLB2_N - 1)))
#define TLB2_TAG(va, ctx) (((va) >> 12) | ((ctx) << 20) | (tlb2_gen << 24))
// sfence.vma for one page: only what could translate that address goes, in all three access
// modes and whatever the privilege context. An entry for another page in the same slot goes
// with it, which is allowed. Linux flushes single pages far more often than everything, and
// after a full flush every page touched next costs a page walk on the general path.
void tlb2_flush_page(uint va) {
    for (uint fm = 0; fm < 3; fm++) {
        uint fk = TLB2_IDX(fm, va);
        tlb2_occ[fk >> 5] &= ~(1u << (fk & 31));
        tlb2_dead[fk >> 5] |= 1u << (fk & 31);
        tlbm_tag[TLBM_IDX(fm, va)] = 0;
    }
}
void tlb2_flush() {
    tlb2_gen++;
    if (tlb2_gen == 256) {
        for (uint k2 = 0; k2 < TLB2_WORDS; k2++) tlb2_occ[k2] = 0;
        for (uint km = 0; km < 3 * TLBM_N; km++) tlbm_tag[km] = 0;
        tlb2_gen = 1;
        tlb_wiped = true;
    }
}


#if 3 * TLB2_N / 2 + 3 * TLBM_N / 2 + 1 != TLB_STATE_TEXELS
#error TLB_STATE_TEXELS (types.h) does not match the TLB sizes
#endif
#ifdef PASS_TICK
// A real TLB keeps its entries until they are flushed; so do these, across passes. The
// megapage entries are read back here. The second level's arrays start each pass empty, and
// an entry they lack is looked up where the last pass left it (tlb2_saved).
void tlb_state_load() {
    uint k;
#if CORES > 1
    if (hart != 0) {
        // a worker core keeps none: it starts a pass with every TLB empty (docs/multicore.md)
        for (k = 0; k < 3 * TLBM_N; k++) { tlbm_tag[k] = 0; tlbm_pg[k] = 0; }
        tlb_wiped = true;
        tlb2_gen = 1;
        return;
    }
#endif
    for (k = 0; k < 3 * TLBM_N / 2; k++) {
        uint at = TLB_STATE_AT + 3 * TLB2_N / 2 + k;
        uint4 t = STATE_TEX(uint2(at & 63, at >> 6));
        tlbm_tag[2 * k] = t.r; tlbm_pg[2 * k] = t.g; tlbm_tag[2 * k + 1] = t.b; tlbm_pg[2 * k + 1] = t.a;
    }
    uint last = TLB_STATE_AT + TLB_STATE_TEXELS - 1;
    uint gen = STATE_TEX(uint2(last & 63, last >> 6)).r;
    tlb2_gen = gen == 0 ? 1 : gen;   // a state from before these texels existed: all zero, nothing matches
}
// Second-level entry k as the last pass left it, if it is the one for `tag`.
bool tlb2_saved(uint k, uint tag, inout uint page) {
    uint at = TLB_STATE_AT + (k >> 1);
    uint4 t = STATE_TEX(uint2(at & 63, at >> 6));
    bool odd = (k & 1) != 0;
    if (tlb_wiped || TLB2_DEAD(k) || (odd ? t.b : t.r) != tag) return false;
    page = odd ? t.a : t.g;
    return true;
}
// What a pixel of the TLB's texels holds after this pass; false for any other pixel.
bool tlb_state_texel_l1(L1P uint2 pos, out uint4 t) {
    uint k = pos.x + 64 * pos.y - TLB_STATE_AT;
    t = 0;
    if (pos.x + 64 * pos.y < TLB_STATE_AT || k >= TLB_STATE_TEXELS) return false;
    if (k < 3 * TLB2_N / 2) {
        // an entry made in this pass (its tag has the generation), or the one already there
        uint4 was = tlb_wiped ? (uint4)0 : STATE_TEX(pos);
        bool a = TLB2_OCC(2 * k) && (tlb2_tag[2 * k] >> 24) == tlb2_gen;
        bool b = TLB2_OCC(2 * k + 1) && (tlb2_tag[2 * k + 1] >> 24) == tlb2_gen;
        if (TLB2_DEAD(2 * k)) was.rg = 0;
        if (TLB2_DEAD(2 * k + 1)) was.ba = 0;
        t = uint4(a ? tlb2_tag[2 * k] : was.r, a ? tlb2_pg[2 * k] : was.g, b ? tlb2_tag[2 * k + 1] : was.b, b ? tlb2_pg[2 * k + 1] : was.a);
    } else if (k < TLB_STATE_TEXELS - 1) {
        uint m = k - 3 * TLB2_N / 2;
        t = uint4(tlbm_tag[2 * m], tlbm_pg[2 * m], tlbm_tag[2 * m + 1], tlbm_pg[2 * m + 1]);
    } else {
        t.r = tlb2_gen;
    }
    return true;
}
#endif

#else
void tlb2_flush() {}
void tlb2_flush_page(uint va) {}
#endif

void hot_flush() {
    PROF(PROF_hot_flush)
    hot_mstatus_ok = false; hot_mip_ok = false; hot_mie_ok = false;
#ifndef NO_PAGING
    tlb_f_vpn = TLB_EMPTY; tlb_r_vpn = TLB_EMPTY; tlb_w_vpn = TLB_EMPTY;
    fetch_vpn = 0xffffffff;
#endif
}

void mmu_update(uint satp) {
#ifdef NO_PAGING
    return;  // satp stays 0: writes are ignored and it reads back as Bare
#endif
    hot_flush();
    tlb2_flush();
    irq_quiet = false;  // paging mode feeds the fast step's translation flags; take the general path once
    cpu.mmu.mode = satp >> 31;
    cpu.mmu.ppn = satp & 0x7fffffff;
}

#include "mem.h"
#include "trap.h"

#define MMU_MODE_OFF 0
#define MMU_MODE_SV32 1

#define MMU_ACCESS_FETCH 0
#define MMU_ACCESS_READ 1
#define MMU_ACCESS_WRITE 2

#define PAGESIZE 4096
#define PTESIZE 4

#define ADDR_PART_OFFSET(x) ((x >> 0) & 0xfff)
#define ADDR_PART_PN0(x) ((x >> 12) & 0x3ff)
#define ADDR_PART_PN1(x) ((x >> 22) & 0xfff)

typedef struct {
    bool v, r, w, x, u, g, a, d;
    uint rsw;
    uint ppn0;
    uint ppn1;
} mmu_page;

uint get_trap_type(uint mode) {
    return mode == MMU_ACCESS_FETCH ? trap_InstructionPageFault :
        (mode == MMU_ACCESS_READ ? trap_LoadPageFault : trap_StorePageFault);
}

uint get_effective_privilege(out uint sum, out uint mxr) {
    uint mstatus = read_mstatus();
    sum = (mstatus >> 18) & 0x1;
    mxr = (mstatus >> 19) & 0x1;
    if ((mstatus >> 17) & 0x1) {
        // TODO: Check if this shouldn't be 9
        return (mstatus >> 11) & 0x3;
    }
    return cpu.csr.privilege;
}

mmu_page load_page_l1(L1P uint addr) {
    PROF(PROF_pte_load)
    uint data = mem_get_cached_or_tex(addr & 0x7ffffffc);
    mmu_page ret;
    #define BOOL(name, bit) ret.name = (data >> bit) & 0x1;
    BOOL(v, 0)
    BOOL(r, 1)
    BOOL(w, 2)
    BOOL(x, 3)
    BOOL(u, 4)
    BOOL(g, 5)
    BOOL(a, 6)
    BOOL(d, 7)
    #undef BOOL
    ret.rsw = (data >> 8) & 0x3;
    ret.ppn0 = (data >> 10) & 0x3ff;
    ret.ppn1 = (data >> 20) & 0xfff;
    return ret;
}

uint mmu_translate_l1(L1P inout ins_ret ins, uint addr, uint mode) {
#ifdef NO_PAGING
    return addr;
#else
    if (cpu.mmu.mode == MMU_MODE_OFF) {
        PROF(PROF_mmu_off)
        return addr;
    }

    #define FAULT \
        ins.trap.en = true; \
        ins.trap.type = get_trap_type(mode); \
        ins.trap.value = addr; \
        return 0;

    uint sum, mxr;
    uint priv = get_effective_privilege(sum, mxr);

    // machine mode fetch will always use physical addresses, otherwise 'mxr'
    // defines if paging will be used
    if (priv == PRIV_MACHINE || (cpu.csr.privilege == PRIV_MACHINE && mode == MMU_ACCESS_FETCH)) {
        PROF(PROF_mmu_machine)
        return addr;
    }

    if (mode == MMU_ACCESS_FETCH) { PROF(PROF_mmu_fetch) } else if (mode == MMU_ACCESS_READ) { PROF(PROF_mmu_read) } else { PROF(PROF_mmu_write) }
    // Successful translations cached per access mode; flushed by hot_flush().
    uint tlb_slot = (addr >> 12) & 3;
    uint4 hit_vpns = mode == MMU_ACCESS_FETCH ? tlb_f_vpn : (mode == MMU_ACCESS_READ ? tlb_r_vpn : tlb_w_vpn);
    if (idx_uint4(hit_vpns, tlb_slot) == (addr >> 12)) {
        if (mode == MMU_ACCESS_FETCH) { PROF(PROF_tlb_hit_fetch) } else if (mode == MMU_ACCESS_READ) { PROF(PROF_tlb_hit_read) } else { PROF(PROF_tlb_hit_write) }
        uint4 hit_pages = mode == MMU_ACCESS_FETCH ? tlb_f_page : (mode == MMU_ACCESS_READ ? tlb_r_page : tlb_w_page);
        uint hit_page = idx_uint4(hit_pages, tlb_slot);
        if (mode == MMU_ACCESS_FETCH) { fetch_vpn = addr >> 12; fetch_page = hit_page; }
        return hit_page | ADDR_PART_OFFSET(addr);
    }

    uint tlb2_k = TLB2_IDX(mode, addr);
    uint tlb2_ctx = priv | (sum << 2) | (mxr << 3);
    uint l2_page = tlb2_pg[tlb2_k];
    bool l2_hit = TLB2_HIT(tlb2_k, TLB2_TAG(addr, tlb2_ctx));
    if (!l2_hit && tlb2_saved(tlb2_k, TLB2_TAG(addr, tlb2_ctx), l2_page)) {
        tlb2_tag[tlb2_k] = TLB2_TAG(addr, tlb2_ctx);
        tlb2_pg[tlb2_k] = l2_page;
        TLB2_OCC_SET(tlb2_k)
        l2_hit = true;
    }
    if (l2_hit) {
        if (mode == MMU_ACCESS_FETCH) { set_idx_uint4(tlb_f_vpn, addr >> 12, tlb_slot); set_idx_uint4(tlb_f_page, l2_page, tlb_slot); fetch_vpn = addr >> 12; fetch_page = l2_page; }
        else if (mode == MMU_ACCESS_READ) { set_idx_uint4(tlb_r_vpn, addr >> 12, tlb_slot); set_idx_uint4(tlb_r_page, l2_page, tlb_slot); }
        else { set_idx_uint4(tlb_w_vpn, addr >> 12, tlb_slot); set_idx_uint4(tlb_w_page, l2_page, tlb_slot); }
        return l2_page | ADDR_PART_OFFSET(addr);
    }

    uint tlbm_k = TLBM_IDX(mode, addr);
    if (tlbm_tag[tlbm_k] == TLBM_TAG(addr, tlb2_ctx)) {
        uint m_page = tlbm_pg[tlbm_k] | (addr & 0x3ff000);
        if (mode == MMU_ACCESS_FETCH) { set_idx_uint4(tlb_f_vpn, addr >> 12, tlb_slot); set_idx_uint4(tlb_f_page, m_page, tlb_slot); fetch_vpn = addr >> 12; fetch_page = m_page; }
        else if (mode == MMU_ACCESS_READ) { set_idx_uint4(tlb_r_vpn, addr >> 12, tlb_slot); set_idx_uint4(tlb_r_page, m_page, tlb_slot); }
        else { set_idx_uint4(tlb_w_vpn, addr >> 12, tlb_slot); set_idx_uint4(tlb_w_page, m_page, tlb_slot); }
        return m_page | ADDR_PART_OFFSET(addr);
    }

    bool super;
    mmu_page page;

    // perform two-layer page walk, exit early in case of super page
    [loop]
    for (uint im = 0; im < 2; im++) {
        uint page_addr = im == 0 ?
            (cpu.mmu.ppn * PAGESIZE + ADDR_PART_PN1(addr) * PTESIZE) :
            ((page.ppn0 | (page.ppn1 << 10)) * PAGESIZE + ADDR_PART_PN0(addr) * PTESIZE);
        page = load_page(page_addr);
        super = im == 0;

        if (!page.v || (!page.r && page.w)) {
            FAULT
        }

        if (page.r || page.x) {
            break;
        } else if (im == 1) {
            // non-leaf page at bottom level
            FAULT
        }
    }

    // PTE has been found, permission check
    bool perm =
        priv == PRIV_MACHINE || // machine can read everything
        (priv == PRIV_USER && page.u) || // this is a user page
        (priv == PRIV_SUPERVISOR && (!page.u || sum)); // supervisor page or SUM
    bool access =
        (mode == MMU_ACCESS_FETCH && page.x) ||
        (mode == MMU_ACCESS_READ && (page.r || (page.x && mxr))) ||
        (mode == MMU_ACCESS_WRITE && page.w);
    bool allowed = perm && access;

    if (!allowed) {
        /* access permission fault */
        FAULT
    }

    if (super && page.ppn0 != 0) {
        /* misaligned super page fault */
        FAULT
    }

    if (!page.a || (mode == MMU_ACCESS_WRITE && !page.d)) {
        /* access/dirty bit fault */
        FAULT
    }

    // translation success
    uint pa = ADDR_PART_OFFSET(addr);
    pa |= super ? ADDR_PART_PN0(addr) << 12 : page.ppn0 << 12;
    pa |= page.ppn1 << 22;

    if (mode == MMU_ACCESS_FETCH) { set_idx_uint4(tlb_f_vpn, addr >> 12, tlb_slot); set_idx_uint4(tlb_f_page, pa & ~0xfff, tlb_slot); fetch_vpn = addr >> 12; fetch_page = pa & ~0xfff; }
    else if (mode == MMU_ACCESS_READ) { set_idx_uint4(tlb_r_vpn, addr >> 12, tlb_slot); set_idx_uint4(tlb_r_page, pa & ~0xfff, tlb_slot); }
    else { set_idx_uint4(tlb_w_vpn, addr >> 12, tlb_slot); set_idx_uint4(tlb_w_page, pa & ~0xfff, tlb_slot); }
    tlb2_tag[tlb2_k] = TLB2_TAG(addr, tlb2_ctx);
    tlb2_pg[tlb2_k] = pa & ~0xfff;
    TLB2_OCC_SET(tlb2_k)
    if (super) {
        tlbm_tag[tlbm_k] = TLBM_TAG(addr, tlb2_ctx);
        tlbm_pg[tlbm_k] = pa & ~0x3fffff;
    }

    /* if (!(pa & 0x80000000) || (pa & 0x7fffffff) >= RAM_MAX) { */
    /*     FAULT */
    /* } */

    return pa;
#endif
}

#endif
