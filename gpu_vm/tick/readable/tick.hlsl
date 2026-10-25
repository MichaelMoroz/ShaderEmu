// gpu_vm's tick, written to be read: the machine of build/tick_gen/c4/v0001.hlsl (gpu_vm/tick/builder/gen.py), with the
// builder's switches that are off there taken out. A drop-in step: the same entry points, state layout and constants.
//
// One draw runs every core. Core c's state is a block of 8 x 6 pixels (core 0's 8 x 12) in eight textures, eight
// 16-byte texels a pixel: registers, the queued operations, the write cache's words and tags. A core runs its whole
// pass in every pixel of its block, and each pixel writes its own eight texels of the state that results. Stores go
// to the write cache only; the commit draws each entry into RAM after the pass. Then the operations the cores queued
// (copies, fills, blits) are made in rounds of three draws.
static const uint C0RT = 1;      // core 0 in the same draw as the workers, its block C0ROWS tall
static const uint C0ROWS = 12;
#if 0   // (the hosts read sizes as text, a worker's first)
static const uint WCN = 258;
static const uint NTEX = 363;
#endif
static const uint WCN0 = 386;    // core 0's write cache entries, and its state texels
static const uint NTEX0 = 523;
#include "../../isa.h"
static const uint RAM_W = 8192;
static const uint T_Q = 24;      // state texels 24-39: the queued operations, two texels each
static const uint T_WC = 40;     // then the write cache's words (a texel an entry), then its tags (four a texel)
static const uint OP_SLOTS = 32;
static const uint OP_SPAN = 272;
static const uint GEO_BLOCKS = 1;
static const uint GEO_BW = 8;
static const uint GEO_BH = 6;
static const uint GEO_BAND = 16;
static const uint GEO_PER = 128;
static const uint GEO_JROW = 0;
static const uint GEO_PERM = 0;
static const uint QUEUE_MAX = 8;
static const uint NONE = 0xffffffffu;
static const uint TEXEL_BITS = 0x0fffffffu;   // a tag: the texel, and from bit 28 which of its four words it holds
static const uint WAIT_MASK = 0x00c01100;     // the kernel's REST mask: bit k is worker k, which core 0 waits for
static const uint F_WAIT = 1, F_SOFT = 2, F_HALT = 4, F_SYNC = 8, F_AGAIN = 16;
static const uint E_BUDGET = 0, E_SYNC = 1, E_WAIT = 2, E_FULL = 3, E_HALT = 5;   // why a pass ended
static const uint UNIT_LITE = 0, UNIT_MID = 1, UNIT_FULL = 2;

Texture2D<uint4> _Ram : register(t0), _Rom : register(t1);
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
Texture2D<uint4> _OpState : register(t10), _Stage : register(t11);
cbuffer PassParams : register(b0) {
    uint _Ticks, _RamEnd, _TimeLo, _TimeHi, _Player, _Flags, _RamRows, _Ext0, _NCores; uint3 _ppad; uint4 _Ext[2];
};
cbuffer CommitParams : register(b1) { uint _Comp; uint _Cores; uint2 _cpad; };

// ---- The core's machine, as the pass goes
static uint core, bucketCount, cacheEntries, spare;   // the write cache: 2 x bucketCount buckets of four, two spare entries
static uint registers[65];                            // r0-r63; 64 takes the writes that go nowhere
static uint cacheTags[WCN0 + 1], cacheWords[4 * (WCN0 + 1)];   // entry cacheEntries is none: a miss reads it
static uint storedTexels;                             // every texel stored this pass, or'ed: a load outside it skips the lookup
static uint pc, bundleTexel, cycle, opsRun, opsTimes8, flags, fastFrom, budget, highestLoad, highestStore;
static uint4 current;                                 // the bundle at pc (bundleTexel in the fast loop)
static uint status, trapPc, cause, trapValue, trapVector, scratch, irqEnable, timerLow, timerHigh, randomState;
static uint budgetLeft, external, endReason;
static uint4 latched0, latched1, queue[16];
static uint queued, queueHead, queueGeneration, refused;
static bool irqDue, storeFull;

uint4 ram(uint texel) { return _Ram.Load(int3(texel % RAM_W, texel / RAM_W, 0)); }
uint4 data_texel(uint address) {   // RAM's or the ROM's
    uint texel = address >> 4;
    int3 at = int3(texel % RAM_W, (texel / RAM_W) & 0x1fff, 0);
    uint4 rom = _Rom.Load(at), memory = _Ram.Load(at);
    return (address & GVM_ROM) != 0 ? rom : memory;
}
uint pick(uint4 value, uint index) { return (index & 2) ? ((index & 1) ? value.w : value.z) : ((index & 1) ? value.y : value.x); }
uint4 put(uint4 value, uint index, uint word) {
    return uint4(index == 0 ? word : value.x, index == 1 ? word : value.y, index == 2 ? word : value.z, index == 3 ? word : value.w);
}
int3 state_pixel(uint pixel, uint which) {   // core 0's pixels from 48 on lie in the six rows under its block
    return int3((which % 128) * 8 + (pixel % 48) / 6, (which / 128) * 16 + (pixel / 48) * 6 + pixel % 6, 0);
}
uint4 state_in(uint texel, uint which) {
    int3 at = state_pixel(texel >> 3, which);
    switch (texel & 7) {
    case 0: return _S0.Load(at); case 1: return _S1.Load(at); case 2: return _S2.Load(at); case 3: return _S3.Load(at);
    case 4: return _S4.Load(at); case 5: return _S5.Load(at); case 6: return _S6.Load(at); default: return _S7.Load(at);
    }
}

// ---- An op word's fields (isa.h)
uint op_bit(uint bit) { return bit << 12; }
bool has_bit(uint op, uint bit) { return (op & op_bit(bit)) != 0; }
uint imm12(uint op) { return asuint((int)op >> 20); }
uint func(uint op) { return (op >> 12) & 31; }
uint operand_b(uint op, uint immediate, bool longForm, uint b) {   // a float's I form: imm12 as the top bits
    return has_bit(op, 32) ? (longForm ? immediate : (op >> 20) << 20) : b;
}

// ---- Integers: the full unit (0-23), the mid one (0-19) and lite (0-7, 16-18)
uint alu_add_logic(uint op, uint x, uint y) {   // 0-7: add, sub, and, or, xor (sub is x + ~y + 1)
    uint invert = (op >> 14) & 1, operand = y ^ (0 - invert);
    bool f0 = has_bit(op, 1), f1 = has_bit(op, 2);
    return f1 ? (f0 ? x ^ operand : x | operand) : (f0 ? x & operand : x + operand + invert);
}
uint alu_compare(uint op, uint x, uint y) {   // 8-15: slt sltu min minu sge sgeu max maxu
    uint flip = has_bit(op, 1) ? 0u : 0x80000000u;
    bool less = (x ^ flip) < (y ^ flip), chosen = has_bit(op, 4) ? !less : less;
    return has_bit(op, 2) ? (chosen ? x : y) : (chosen ? 1u : 0u);
}
uint alu_shift(uint op, uint x, uint y, uint unit) {   // 16-19: sll srl sra mul (lite has no mul)
    bool f0 = has_bit(op, 1);
    return has_bit(op, 2) ? (f0 && unit != UNIT_LITE ? x * y : asuint(asint(x) >> y)) : (f0 ? x >> y : x << y);
}
uint alu_rotate(uint op, uint x, uint y) {   // 20-23: ror rol seq sne (DXBC's shifts take 5 bits: 32 - y is -y)
    uint back = 0 - y;
    bool f0 = has_bit(op, 1);
    return has_bit(op, 2) ? ((x == y ? 1u : 0u) ^ (f0 ? 1u : 0u)) : (f0 ? (x << y) | (x >> back) : (x >> y) | (x << back));
}
uint alu(uint op, uint immediate, uint a, uint b, uint unit) {
    uint y = has_bit(op, 32) ? immediate : b;
    if (has_bit(op, 16)) return unit == UNIT_FULL && has_bit(op, 4) ? alu_rotate(op, a, y) : alu_shift(op, a, y, unit);
    return unit != UNIT_LITE && has_bit(op, 8) ? alu_compare(op, a, y) : alu_add_logic(op, a, y);
}

// ---- Floats
bool is_nan(uint bits) { return (bits & 0x7fffffffu) > 0x7f800000u; }
uint quiet(float value) { return is_nan(asuint(value)) ? 0x7fc00000u : asuint(value); }
uint float_sum(uint f, uint bitsA, uint bitsB, uint bitsC) {   // f 0-7: add mul mad msb sub nmsb min max
    precise float a = asfloat(bitsA), b = asfloat(bitsB), c = asfloat(bitsC);
    precise float product = a * b;
    precise float rounded = asfloat(quiet(product));   // the product rounded before the sum
    bool fused = f == GVM_F_MAD || f == GVM_F_MSB, negated = f == GVM_F_NMSB;
    precise float left = fused ? rounded : negated ? c : a;
    precise float right = fused ? c : negated ? rounded : b;
    precise float sum = (f == GVM_F_MSB || f == GVM_F_SUB || negated) ? left - right : left + right;
    bool nanA = is_nan(bitsA), nanB = is_nan(bitsB);
    bool less = a < b || (a == b && (bitsA >> 31) > (bitsB >> 31)), more = a > b || (a == b && (bitsB >> 31) > (bitsA >> 31));
    uint extreme = nanA ? (nanB ? 0x7fc00000u : bitsB) : nanB ? bitsA : (((f & 1) != 0 ? more : less) ? bitsA : bitsB);
    return f >= GVM_F_MIN ? extreme : f == GVM_F_MUL ? quiet(product) : quiet(sum);
}
uint int_to_float(uint a) { bool negative = (a >> 31) != 0; return asuint((float)(negative ? 0 - a : a)) | (negative ? 0x80000000u : 0); }
uint fpu(uint op, uint bitsA, uint bitsB, uint bitsC) {
    uint f = func(op);
    precise float a = asfloat(bitsA), b = asfloat(bitsB);
    [branch] if (f >= GVM_F_DIV) {   // the special functions, behind a branch
        precise float result;
        [branch] if (f == GVM_F_DIV) {   // (the card divides by 1 / b: two ordinary numbers with their exponents moved)
            uint expA = (bitsA >> 23) & 0xff, expB = (bitsB >> 23) & 0xff;
            int apart = (int)expA - (int)expB, moved = clamp(apart, -126, 127);
            precise float scaledA = asfloat((bitsA & 0x807fffffu) | ((uint)(127 + moved) << 23));
            precise float scaledB = asfloat((bitsB & 0x807fffffu) | ((uint)(127 - (apart - moved)) << 23));
            result = (expA - 1 < 254 && expB - 1 < 254) ? scaledA / scaledB : a / b;
        } else result = f == GVM_F_SQRT ? sqrt(a) : f == GVM_F_RSQRT ? rsqrt(a) : f == GVM_F_RCP ? rcp(a) : f == GVM_F_SIN ? sin(a)
                      : f == GVM_F_COS ? cos(a) : f == GVM_F_EXP2 ? exp2(a) : log2(a);
        return quiet(result);
    }
    bool nanA = is_nan(bitsA), nanB = is_nan(bitsB), f0 = (f & 1) != 0, f1 = (f & 2) != 0;
    // f 8-15: eq lt le unord sgnj sgnjn sgnjx frac
    bool compared = !nanA && !nanB && (f == GVM_F_EQ ? a == b : f == GVM_F_LT ? a < b : a <= b);
    uint signInjected = (bitsA & 0x7fffffffu) | ((f == GVM_F_SGNJ ? bitsB : f == GVM_F_SGNJN ? ~bitsB : bitsA ^ bitsB) & 0x80000000u);
    uint tests = (f & 4) ? (f == GVM_F_FRAC ? quiet(a - floor(a)) : signInjected)
               : f == GVM_F_UNORD ? ((nanA || nanB) ? 1u : 0u) : (compared ? 1u : 0u);
    // f 16-23: itof utof ftoi ftou floor ceil trunc round
    precise float whole = f1 ? (f0 ? round(a) : trunc(a)) : (f0 ? ceil(a) : floor(a));
    uint toInt = nanA ? 0 : a >= 2147483648.0 ? 0x7fffffffu : a <= -2147483648.0 ? 0x80000000u : asuint((int)a);
    uint toUint = (nanA || a <= 0.0) ? 0 : a >= 4294967296.0 ? 0xffffffffu
                : a >= 2147483648.0 ? 0x80000000u + (uint)(a - 2147483648.0) : (uint)a;
    uint converts = (f & 4) ? quiet(whole) : f1 ? (f0 ? toUint : toInt) : (f0 ? asuint((float)bitsA) : int_to_float(bitsA));
    return (f & 16) ? converts : (f & 8) ? tests : float_sum(f, bitsA, bitsB, bitsC);
}

// ---- The write cache: a texel's entry is in one of the four ways of its bucket in the first table or the second
bool holds(uint tag, uint texel, uint word) { return tag != NONE && (tag & TEXEL_BITS) == texel && ((tag >> (28 + word)) & 1) != 0; }
uint tag_with(uint tag, uint texel, uint word) { return texel | (((tag == NONE ? 0u : tag >> 28) | (1u << word)) << 28); }
uint first_bucket(uint texel) { return (((texel * 0x9e3779b1u) >> 16) * bucketCount) >> 16; }
uint second_bucket(uint texel) { return bucketCount + ((((texel * 0x85ebca6bu + 0x165667b1u) >> 16) * bucketCount) >> 16); }
uint4 bucket(uint index) { uint at = 4 * index; return uint4(cacheTags[at], cacheTags[at + 1], cacheTags[at + 2], cacheTags[at + 3]); }
uint way_bits(uint4 tags, uint texel, bool matchOnly) {   // bit k: way k holds the texel (or, unless matchOnly, is free)
    bool4 same = (tags & TEXEL_BITS) == texel, empty = tags == NONE;
    bool4 chosen = matchOnly ? same && !empty : same || empty;
    return (chosen.x ? 1u : 0u) | (chosen.y ? 2u : 0u) | (chosen.z ? 4u : 0u) | (chosen.w ? 8u : 0u);
}
struct Lookup { uint address, texel, word, bucket1, bucket2, slot, slotTag, entry, cached; uint4 memory, tags1, tags2; };
void find_slot(inout Lookup found) {   // the entry for the texel: one that holds it, else the first free one
    uint usable = way_bits(found.tags1, found.texel, false) | way_bits(found.tags2, found.texel, false) << 4;
    uint matching = way_bits(found.tags1, found.texel, true) | way_bits(found.tags2, found.texel, true) << 4;
    uint way = firstbitlow(matching != 0 ? matching : usable);   // (a match first: a dropped entry leaves holes)
    found.slotTag = way < 8 ? pick(way < 4 ? found.tags1 : found.tags2, way & 3) : NONE;
    found.slot = way < 4 ? 4 * found.bucket1 + way : way < 8 ? 4 * found.bucket2 + (way & 3) : NONE;
    found.entry = holds(found.slotTag, found.texel, found.word) ? found.slot : cacheEntries;
}
Lookup look_up(uint address, bool look) {   // without look, the cache is taken to hold nothing of the texel
    Lookup found;
    found.address = address; found.texel = address >> 4; found.word = (address >> 2) & 3;
    found.bucket1 = first_bucket(found.texel); found.bucket2 = second_bucket(found.texel);
    found.memory = 0; found.tags1 = NONE; found.tags2 = NONE;
    [branch] if (look) { found.tags1 = bucket(found.bucket1); found.tags2 = bucket(found.bucket2); }
    find_slot(found);
    found.cached = cacheWords[4 * found.entry + found.word];
    return found;
}
Lookup look_up_op(uint op, uint immediate, uint a, uint b, bool store) {
    uint address = a + (has_bit(op, 16) ? b << (op >> 20) : immediate);
    return look_up(address, store || ((address >> 4) & storedTexels) == (address >> 4));
}
uint word_seen_by(Lookup found) { return found.entry != cacheEntries ? found.cached : pick(found.memory, found.word); }
uint size_shift(uint op) { return (op >> 9) & 24; }   // 32 less the access's bits
uint extend(uint op, uint word, uint address) {
    uint shift = size_shift(op), value = (word >> (address << 3)) << shift;
    return has_bit(op, 4) ? value >> shift : asuint(asint(value) >> shift);
}
uint merge(uint op, uint word, uint address, uint value) {
    uint shift = address << 3, bytes = (0xffffffffu >> size_shift(op)) << shift;
    return word ^ ((word ^ (value << shift)) & bytes);
}
void cache_write(Lookup found, uint entry, uint op, uint word, uint value) {   // entry cacheEntries: nothing kept
    cacheTags[entry] = tag_with(found.slotTag, found.texel, found.word);
    cacheWords[4 * entry + found.word] = merge(op, word, found.address, value);
}

// ---- Branches: the control word's test (a comparison, signed by bit 2; equality; a bit; addbnz's sum), and cc 0's jumps
uint taken(uint control, uint compareA, uint compareB, uint imm6, uint sum) {
    bool immediate = (control & 0x400) != 0;
    uint other = immediate ? imm6 : compareB, flip = (control << 22) & 0x80000000u;
    bool less = (compareA ^ flip) < (other ^ flip);
    uint test = immediate ? (compareA >> other) & 1 : (sum == 0 ? 1u : 0u);
    uint result = (control & 0x100) ? (less ? 1u : 0u) : (control & 0x200) ? (compareA == other ? 1u : 0u) : test;
    return (control & 0x80) ? (result == 0 ? 1u : 0u) : result;
}
uint special_jump(uint control, uint base, uint next, uint addFrom) {   // jr, callr (rs after the ops), j, call, add (rs as found)
    uint kind = GVM_SUB(control), source = GVM_CRS(control), to = registers[source] & ~15u;
    next = kind == GVM_SUB_JR || kind == GVM_SUB_CALLR ? to : kind == GVM_SUB_J || kind == GVM_SUB_CALL ? base + asuint(GVM_OFF18(control)) * 16 : next;
    registers[kind == GVM_SUB_CALL || kind == GVM_SUB_CALLR ? 1 : 64] = base + 16;
    registers[kind == GVM_SUB_ADD ? source : 64] = addFrom + asuint(GVM_CIMM12(control));
    return next;
}

// ---- The fast loop's step (paths 3-7): two ops and a branch, from the next bundle and the target read ahead
void fast_step(inout uint left) {
    uint4 words = current;
    uint control = words.w, path = control & 7, op0 = words.x, op1 = words.y;
    uint target = bundleTexel + asuint((int)(control << 6) >> 23), next = bundleTexel + 1;
    uint4 nextBundle = ram(next), targetBundle = ram(target);
    uint imm0 = (control & 0x20) ? words.z : imm12(op0), imm1 = (control & 0x40) ? words.z : imm12(op1);
    uint dest0 = GVM_RD(op0), dest1 = GVM_RD(op1), sink0 = dest0 != 0 ? dest0 : 64, sink1 = dest1 != 0 ? dest1 : 64;
    uint regA1 = GVM_RA(op1), regB1 = GVM_RB(op1);
    uint a0 = registers[GVM_RA(op0)], b0 = registers[GVM_RB(op0)], a1 = registers[regA1], b1 = registers[regB1];
    uint compareA = registers[GVM_CRA(control)], compareB = registers[GVM_CRB(control)], value0, value1;
    [branch] if (path == GVM_PATH_INT) {   // op 0 the mid unit, op 1 lite
        value0 = alu(op0, imm0, a0, b0, UNIT_MID);
        registers[sink0] = value0;
        value1 = alu(op1, imm1, regA1 == sink0 ? value0 : a1, regB1 == sink0 ? value0 : b1, UNIT_LITE);
    } else [branch] if ((path & 1) != 0) {   // a load or a store, then lite (paths 3 and 5)
        bool store = has_bit(op0, 8);
        Lookup found = look_up_op(op0, imm0, a0, b0, store);
        [branch] if ((((op0 ^ op_bit(8)) & op_bit(11)) | (found.address & 3)) != 0) found.memory = data_texel(found.address);
        highestStore = max(highestStore, store ? found.address - GVM_DEV : 0);
        highestLoad = max(highestLoad, store ? 0 : found.address & ~GVM_ROM);
        uint word = word_seen_by(found);
        value0 = extend(op0, word, found.address);
        bool full = store && found.slot == NONE;   // no room: kept in the spare entry, and the pass ends
        cache_write(found, store ? (full ? spare : found.slot) : cacheEntries, op0, word, registers[dest0]);
        storedTexels |= store ? found.texel : 0;
        cycle -= full ? left - 1 : 0;
        left = full ? 1 : left;
        sink0 = store ? 64 : sink0;
        registers[sink0] = value0;
        value1 = alu(op1, imm1, regA1 == sink0 ? value0 : a1, regB1 == sink0 ? value0 : b1, UNIT_LITE);
    } else [branch] if (path == GVM_PATH_LOAD2) {   // two loads
        Lookup first = look_up_op(op0, imm0, a0, b0, false), second = look_up_op(op1, imm1, a1, b1, false);
        first.memory = data_texel(first.address);
        second.memory = data_texel(second.address);
        highestLoad = max(highestLoad, max(first.address & ~GVM_ROM, second.address & ~GVM_ROM));
        value0 = extend(op0, word_seen_by(first), first.address);
        registers[sink0] = value0;
        value1 = extend(op1, word_seen_by(second), second.address);
    } else {   // a store, then a load or a store that sees it (path 6)
        bool store1 = has_bit(op1, 8);
        Lookup first = look_up_op(op0, imm0, a0, b0, true), second = look_up_op(op1, imm1, a1, b1, true);
        [branch] if (((op0 & op_bit(3)) | ((op1 ^ op_bit(8)) & op_bit(11)) | ((first.address | second.address) & 3)) != 0) {
            first.memory = data_texel(first.address);
            second.memory = data_texel(second.address);
        }
        highestStore = max(highestStore, max(first.address - GVM_DEV, store1 ? second.address - GVM_DEV : 0));
        highestLoad = max(highestLoad, store1 ? 0 : second.address & ~GVM_ROM);
        uint word0 = word_seen_by(first), entry0 = first.slot == NONE ? spare : first.slot;
        cache_write(first, entry0, op0, word0, registers[dest0]);
        uint tag0 = cacheTags[entry0];
        bool over = first.texel == second.texel && first.slot == NONE;   // op 0 in the spare entry: op 1 finds it there
        second.tags1 = (entry0 >> 2) == second.bucket1 ? put(second.tags1, entry0 & 3, tag0) : second.tags1;
        second.tags2 = (entry0 >> 2) == second.bucket2 ? put(second.tags2, entry0 & 3, tag0) : second.tags2;
        find_slot(second);
        second.slot = over ? spare : second.slot;
        second.slotTag = over ? tag0 : second.slotTag;
        second.entry = over ? (holds(tag0, second.texel, second.word) ? spare : cacheEntries) : second.entry;
        second.cached = cacheWords[4 * second.entry + second.word];
        uint word1 = word_seen_by(second);
        value1 = extend(op1, word1, second.address);
        bool full = first.slot == NONE || (store1 && second.slot == NONE);
        cache_write(second, store1 ? (second.slot == NONE ? spare + 1 : second.slot) : cacheEntries, op1, word1, registers[dest1]);
        storedTexels |= first.texel | (store1 ? second.texel : 0);
        cycle -= full ? left - 1 : 0;
        left = full ? 1 : left;
        sink1 = store1 ? 64 : sink1;
    }
    registers[sink1] = value1;
    uint imm6 = asuint((int)control >> 26), sum = compareA + imm6;
    bool jump = taken(control, compareA, compareB, imm6, sum) != 0;
    registers[GVM_CC(control) != GVM_CC_ADDBNZ ? 64 : GVM_CRA(control)] = sum;
    bundleTexel = jump ? target : next;
    current = jump ? targetBundle : nextBundle;
    left--;
    opsTimes8 += control & 0x18;
}

// ---- Paths 1 and 2: a whole bundle of any control word but eret (op 0 any integer op, sel, lpc, a float op, or a load
// or store; op 1 lite or a float sum). The budget left, 0 when a store found no room (the bundle again next pass).
uint whole_step(uint left) {
    uint4 words = current;
    uint base = pc & ~15u, control = words.w, op0 = words.x, op1 = words.y, opcode0 = (op0 >> 12) & 255;
    bool long0 = (control & 0x20) != 0, long1 = (control & 0x40) != 0, fits = true;
    uint imm0 = long0 ? words.z : imm12(op0), imm1 = long1 ? words.z : imm12(op1);
    uint dest0 = GVM_RD(op0), dest1 = GVM_RD(op1), regA1 = GVM_RA(op1), regB1 = GVM_RB(op1);
    uint a0 = registers[GVM_RA(op0)], b0 = registers[GVM_RB(op0)], c0 = registers[GVM_RC(op0)];
    uint a1 = registers[regA1], b1 = registers[regB1], value0 = 0, value1;
    uint compareA = registers[GVM_CRA(control)], compareB = registers[GVM_CRB(control)], addFrom = registers[GVM_CRS(control)];
    [branch] if ((control & 7) == GVM_PATH_MEMF) {
        bool store = has_bit(op0, 8);
        Lookup found = look_up_op(op0, imm0, a0, b0, true);
        found.memory = data_texel(found.address);
        highestStore = max(highestStore, store ? found.address - GVM_DEV : 0);
        highestLoad = max(highestLoad, store ? 0 : found.address & ~GVM_ROM);
        uint word = word_seen_by(found);
        value0 = extend(op0, word, found.address);
        fits = !store || found.slot != NONE;
        cache_write(found, store && fits ? found.slot : cacheEntries, op0, word, registers[dest0]);
        storedTexels |= store ? found.texel : 0;
        dest0 = store ? 0 : dest0;
    } else [branch] if (opcode0 == GVM_OP_I(GVM_I_SEL) || opcode0 == GVM_X_LPC) value0 = opcode0 == GVM_X_LPC ? base + imm0 : c0 != 0 ? a0 : b0;
    else [branch] if ((opcode0 >> 6) == 1) value0 = alu(op0, imm0, a0, b0, UNIT_FULL);
    else [branch] if ((opcode0 & 31) <= GVM_F_MAX) value0 = float_sum(opcode0 & 31, a0, operand_b(op0, imm0, long0, b0), c0);
    else value0 = fpu(op0, a0, operand_b(op0, imm0, long0, b0), c0);
    [branch] if (!fits) { storeFull = true; return 0; }
    registers[dest0 == 0 ? 64 : dest0] = value0;
    a1 = dest0 != 0 && regA1 == dest0 ? value0 : a1;
    b1 = dest0 != 0 && regB1 == dest0 ? value0 : b1;
    [branch] if ((op1 & op_bit(0xc0)) == op_bit(0xc0)) value1 = float_sum(func(op1), a1, operand_b(op1, imm1, long1, b1), registers[GVM_RC(op1)]);
    else value1 = alu(op1, imm1, a1, b1, UNIT_LITE);
    registers[dest1 == 0 ? 64 : dest1] = value1;
    uint imm6 = asuint(GVM_CIMM6(control)), sum = compareA + imm6;
    registers[GVM_CC(control) != GVM_CC_ADDBNZ ? 64 : GVM_CRA(control)] = sum;   // after the ops' writes: it wins
    pc = taken(control, compareA, compareB, imm6, sum) != 0 ? base + asuint(GVM_OFF9(control)) * 16 : base + 16;
    [branch] if (GVM_CC(control) == GVM_CC_SPECIAL) pc = special_jump(control, base, base + 16, addFrom);
    current = ram(pc >> 4);
    cycle++;
    opsRun += GVM_N(control);
    return left - 1;
}

// ---- Traps, special registers, interrupts
uint pending() {
    bool due = (timerLow | timerHigh) != 0 && (_TimeHi > timerHigh || (_TimeHi == timerHigh && _TimeLo >= timerLow));
    return ((flags & F_SOFT) ? 1u : 0u) | (due ? 2u : 0u) | (external << 2);
}
void trap(uint why, uint at, uint value) {
    trapPc = at; cause = why; trapValue = value;
    status = (status & GVM_ST_S ? GVM_ST_PS : 0) | (status & GVM_ST_IE ? GVM_ST_PIE : 0) | GVM_ST_S;
    pc = trapVector & ~15u;
    flags &= ~F_WAIT;
}
uint special_read(uint number) {
    [branch] if (number == GVM_SR_RANDOM) { randomState ^= randomState << 13; randomState ^= randomState >> 17; randomState ^= randomState << 5; }
    uint4 group0 = uint4(status, trapPc, cause, trapValue), group1 = uint4(trapVector, scratch, irqEnable, pending());
    uint4 group2 = uint4(timerLow, timerHigh, _TimeLo, _TimeHi), group3 = uint4(cycle, core, _Player, randomState);
    return number < 4 ? pick(group0, number) : number < 8 ? pick(group1, number) : number < 12 ? pick(group2, number)
         : number < 16 ? pick(group3, number) : number == GVM_SR_LEFT ? budgetLeft
         : number == GVM_SR_XR || number == GVM_SR_NPAIR ? 0 : opsRun + 1;   // (OPS counts the op reading it)
}
void special_write(uint number, uint value) {
    status = number == GVM_SR_STATUS ? (value & 7) | (status & GVM_ST_S) : status;
    trapPc = number == GVM_SR_EPC ? value : trapPc;
    cause = number == GVM_SR_CAUSE ? value : cause;
    trapValue = number == GVM_SR_TVAL ? value : trapValue;
    trapVector = number == GVM_SR_TVEC ? value : trapVector;
    scratch = number == GVM_SR_SCRATCH ? value : scratch;
    irqEnable = number == GVM_SR_IEN ? value : irqEnable;
    flags = number == GVM_SR_IPEND ? (flags & ~F_SOFT) | ((value & 1) ? F_SOFT : 0) : flags;
    timerLow = number == GVM_SR_TCMP_LO ? value : timerLow;
    timerHigh = number == GVM_SR_TCMP_HI ? value : timerHigh;
}

// ---- The operations: a descriptor's two texels, what it writes (writeBytes at its destination), what it reads
struct Operation { uint kind, destination, source, count, destinationStride, sourceStride, rowBytes, rows, writeBytes, readBytes; };
Operation operation_of(uint4 first, uint4 second) {
    Operation op;
    op.kind = first.x; op.destination = first.y; op.source = first.z; op.count = first.w;
    op.destinationStride = second.x; op.sourceStride = second.y; op.rowBytes = second.z; op.rows = second.w;
    bool blit = op.kind == GVM_OP_BLIT && op.rows != 0 && op.rowBytes != 0;
    op.writeBytes = op.kind == GVM_OP_COPY || op.kind == GVM_OP_FILL ? op.count : blit ? op.destinationStride * (op.rows - 1) + op.rowBytes : 0;
    op.readBytes = op.kind == GVM_OP_COPY ? op.count : blit ? op.sourceStride * (op.rows - 1) + op.rowBytes : 0;
    return op;
}
bool in_range(uint address, uint bytes, uint start, uint length) { return length != 0 && (address - start < length || start - address < bytes); }
bool operation_holds(uint address, uint bytes, bool store) {   // an operation in flight holds it: the pass ends before the op
    [branch] if (address >= GVM_OPS) return store && address == GVM_OPS && bytes == 16 && queued == QUEUE_MAX;
    bool held = false;
    [loop] for (uint index = 0; index < min(queued, QUEUE_MAX); index++) {
        Operation op = operation_of(queue[2 * index], queue[2 * index + 1]);
        held = held || in_range(address, bytes, op.destination, op.writeBytes) || (store && in_range(address, bytes, op.source, op.readBytes));
    }
    return held;
}

// ---- The general step's memory: any address, any alignment
uint word_seen(uint address) {   // a readable word as this core sees it
    Lookup found = look_up(address, true);
    found.memory = data_texel(address);
    return address >= GVM_ROM ? pick(found.memory, found.word) : word_seen_by(found);
}
bool readable(uint address) { return address < _RamEnd || (address >= GVM_ROM && address < GVM_ROM_END); }
bool load_any(uint address, uint bytes, out uint value) {   // 1, 2 or 4 bytes; false: a fault
    value = 0;
    [branch] if (address >= GVM_OPS) {   // in flight, refused, the latched words
        uint offset = address - GVM_OPS;
        value = offset == 0 ? queued : offset == 4 ? refused : offset < 16 ? pick(latched0, offset >> 2) : pick(latched1, (offset >> 2) & 3);
        return offset <= 32 - bytes && ((address | bytes) & 3) == 0;
    }
    bool fine = readable(address) && readable(address + bytes - 1);
    [branch] if (fine) {
        uint shift = (address & 3) * 8, low = word_seen(address & ~3u), high = 0;
        [branch] if ((address & 3) + bytes > 4) high = word_seen((address & ~3u) + 4);
        uint word = shift != 0 ? (low >> shift) | (high << (32 - shift)) : low;
        value = bytes == 4 ? word : word & ((1u << (bytes * 8)) - 1);
    }
    return fine;
}
bool store_allowed(uint address, uint bytes) {
    return address >= GVM_OPS ? address - GVM_OPS <= 32 - bytes && ((address | bytes) & 3) == 0
                              : address >= GVM_DEV && address < _RamEnd && address + bytes - 1 < _RamEnd;
}
uint word_of(uint4 data, uint index) { return index < 4 ? pick(data, index) : 0; }
bool store_any(uint address, uint bytes, uint4 data) {   // into the write cache; false: no room, nothing made
    uint offset = address & 3, wordCount = (offset + bytes + 3) >> 2, index, slots[5], oldTags[5], oldWords[5], wordAt[5];
    bool fine = true;
    [loop] for (index = 0; index < min(wordCount, 5u) && fine; index++) {
        uint wordIndex = (address >> 2) + index, texel = wordIndex >> 2, word = wordIndex & 3;
        Lookup found = look_up(wordIndex << 2, true);
        uint slot = found.slot;
        fine = slot != NONE;
        [branch] if (fine) {
            uint old = found.entry != cacheEntries ? found.cached : pick(ram(texel), word);
            uint low = index == 0 ? offset : 0, high = min(4u, offset + bytes - 4 * index);
            uint mask = (high >= 4 ? 0xffffffffu : (1u << (8 * high)) - 1) & ~((1u << (8 * low)) - 1);
            uint value = offset != 0 ? (word_of(data, index) << (8 * offset)) | (index != 0 ? word_of(data, index - 1) >> (32 - 8 * offset) : 0)
                                     : word_of(data, index);
            slots[index] = slot; oldTags[index] = found.slotTag; oldWords[index] = cacheWords[4 * slot + word]; wordAt[index] = 4 * slot + word;
            cacheTags[slot] = tag_with(found.slotTag, texel, word);
            cacheWords[4 * slot + word] = old ^ ((old ^ value) & mask);
            storedTexels |= texel;
        }
    }
    [branch] if (!fine) [loop] for (index = index - 1; index > 0; index--) {   // undone, last first
        cacheTags[slots[index - 1]] = oldTags[index - 1];
        cacheWords[wordAt[index - 1]] = oldWords[index - 1];
    }
    return fine;
}

// ---- The general step: one op of any class at slot s. 0: done; 1: trapped; 2: the pass ends after it (counted);
// 3: the pass ends before it (not counted, pc at it)
uint extended_op(uint opcode, uint a, uint b) {
    uint lowA = a & 0xffff, highA = a >> 16, lowB = b & 0xffff, highB = b >> 16;   // the high word of a * b, by halves
    uint lowLow = lowA * lowB, lowHigh = lowA * highB, highLow = highA * lowB;
    uint middle = (lowLow >> 16) + (lowHigh & 0xffff) + (highLow & 0xffff);
    uint highUnsigned = highA * highB + (lowHigh >> 16) + (highLow >> 16) + (middle >> 16);
    bool signs = opcode == GVM_X_DIV || opcode == GVM_X_REM, negativeA = signs && (a >> 31) != 0, negativeB = signs && (b >> 31) != 0;
    uint positiveA = negativeA ? 0 - a : a, positiveB = negativeB ? 0 - b : b;
    uint quotient = positiveB == 0 ? 0xffffffffu : positiveA / positiveB, modulo = positiveB == 0 ? positiveA : positiveA - quotient * positiveB;
    bool remainder = opcode == GVM_X_REM || opcode == GVM_X_REMU;
    uint division = b == 0 ? (remainder ? a : 0xffffffffu) : remainder ? (negativeA ? 0 - modulo : modulo) : (negativeA != negativeB ? 0 - quotient : quotient);
    uint highProduct = opcode == GVM_X_MULHU ? highUnsigned : highUnsigned - ((a >> 31) ? b : 0) - (opcode == GVM_X_MULH && (b >> 31) ? a : 0);
    uint bits = opcode == GVM_X_CLZ ? (a == 0 ? 32u : 31u - firstbithigh(a)) : opcode == GVM_X_CTZ ? (a == 0 ? 32u : firstbitlow(a))
              : opcode == GVM_X_POPC ? countbits(a) : opcode == GVM_X_BSWAP ? (a >> 24 | ((a >> 8) & 0xff00u) | ((a << 8) & 0xff0000u) | a << 24)
              : reversebits(a);
    return opcode <= GVM_X_MULHSU ? highProduct : opcode <= GVM_X_REMU ? division : bits;
}
uint field_mask(uint immediate) { uint length = ((immediate >> 5) & 31) + 1; return length >= 32 ? 0xffffffffu : (1u << length) - 1; }
uint memory_op(uint op, uint immediate, uint a, uint b, uint opPc, inout uint ending, inout bool illegal) {
    uint opcode = (op >> 12) & 255, mode = (opcode >> 3) & 7, width = opcode & 7, dest = GVM_RD(op);
    [branch] if (mode <= 3) {   // loads and stores
        uint bytes = width == GVM_W_W ? 4u : width == GVM_W_D ? 8u : width == GVM_W_Q ? 16u : width == GVM_W_NC ? 0u : (width & 1) ? 1u : 2u;
        illegal = bytes == 0;
        uint address = a + (mode >= 2 ? b << ((op >> 20) & 31) : immediate);
        bool store = (mode & 1) != 0;
        bytes = bytes <= 4 ? min(bytes, 4 - (address & 3)) : bytes;   // (a byte, a half or a word stays in its word)
        [branch] if (illegal) return 0;
        [branch] if (operation_holds(address, bytes, store)) { pc = opPc; ending = E_SYNC; return 3; }
        uint count = clamp(bytes >> 2, 1u, 4u), index;
        [branch] if (!store) {
            uint4 value = 0;
            bool fine = true;
            [loop] for (index = 0; index < count && fine; index++) { uint word; fine = load_any(address + 4 * index, min(bytes, 4u), word); value = put(value, index, word); }
            [branch] if (!fine) { trap(GVM_CAUSE_LOAD, opPc, address); return 1; }
            value.x = width == GVM_W_B ? asuint(asint(value.x << 24) >> 24) : width == GVM_W_H ? asuint(asint(value.x << 16) >> 16) : value.x;
            [loop] for (index = 0; index < count; index++) { uint target = (dest + index) & 63; registers[target == 0 ? 64 : target] = pick(value, index); }
            return 0;
        }
        [branch] if (!store_allowed(address, bytes)) { trap(GVM_CAUSE_STORE, opPc, address); return 1; }
        uint4 data = uint4(registers[dest], registers[(dest + 1) & 63], registers[(dest + 2) & 63], registers[(dest + 3) & 63]);
        [branch] if (address >= GVM_OPS) {   // the latched words; a 16-byte store at GVM_OPS queues the operation
            [loop] for (index = 0; index < min(bytes >> 2, 4u); index++) {
                uint latch = ((address - GVM_OPS) >> 2) + index;
                latched0 = latch < 4 ? put(latched0, latch, pick(data, index)) : latched0;
                latched1 = latch >= 4 ? put(latched1, latch & 3, pick(data, index)) : latched1;
            }
            [branch] if (address == GVM_OPS && bytes == 16) { queue[2 * queued] = latched0; queue[2 * queued + 1] = latched1; queued++; fastFrom = GVM_PATH_INT; }
            return 0;
        }
        [branch] if (!store_any(address, bytes, data)) { pc = opPc; ending = E_FULL; return 3; }
        return 0;
    }
    illegal = !(mode == GVM_M_AMO || (mode == GVM_M_AMO2 && width <= GVM_AMO2_MAXU));   // atomics
    [branch] if (illegal) return 0;
    [branch] if ((a & 3) != 0 || !store_allowed(a, 4) || a >= GVM_OPS) { trap(GVM_CAUSE_STORE, opPc, a); return 1; }
    [branch] if (operation_holds(a, 4, true)) { pc = opPc; ending = E_SYNC; return 3; }
    uint old = word_seen(a), expected = registers[dest];
    bool less = asint(old) < asint(b), lessUnsigned = old < b;
    uint result = mode == GVM_M_AMO2 ? (width == GVM_AMO2_MINU ? (lessUnsigned ? old : b) : (lessUnsigned ? b : old))
                : width == GVM_AMO_SWAP ? b : width == GVM_AMO_ADD ? old + b : width == GVM_AMO_AND ? (old & b) : width == GVM_AMO_OR ? (old | b)
                : width == GVM_AMO_XOR ? (old ^ b) : width == GVM_AMO_MIN ? (less ? old : b) : width == GVM_AMO_MAX ? (less ? b : old)
                : (old == expected ? b : old);
    [branch] if (!store_any(a, 4, uint4(result, 0, 0, 0))) { pc = opPc; ending = E_FULL; return 3; }
    registers[dest == 0 ? 64 : dest] = old;
    return 0;
}
uint any_op(uint op, uint immediate, bool longForm, uint base, uint slot, inout uint ending) {
    uint opcode = (op >> 12) & 255, dest = GVM_RD(op), regC = GVM_RC(op), group = opcode >> 6, f = opcode & 31, opPc = base | (slot * 4);
    uint a = registers[GVM_RA(op)], b = registers[GVM_RB(op)], c = registers[regC], value = 0, result = 0;
    bool immediateForm = (opcode & 32) != 0, illegal = false, writes = true, supervisor = (status & GVM_ST_S) != 0;
    [branch] if (group == 1) {   // integer: the unit, then the R form's carries, shadd, sel, and the I form's bfe
        uint carryIn = c & 1, sum = a + b, sumCarry = sum + carryIn, difference = a - b;
        uint other = f == GVM_I_SHADD ? a + (b << regC) : f == GVM_I_SEL ? (c != 0 ? a : b) : f == GVM_I_ADDCY ? sumCarry
                   : f == GVM_I_CARRY ? ((sum < a || sumCarry < sum) ? 1u : 0u) : f == GVM_I_SUBBY ? difference - carryIn
                   : ((a < b || difference < carryIn) ? 1u : 0u);
        uint mask = field_mask(immediate), field = (a >> (immediate & 31)) & mask;
        uint signFill = ~mask & (0 - ((field & ~(mask >> 1)) != 0 ? 1u : 0u));
        illegal = immediateForm ? f > GVM_I_COMMON_LAST && f != GVM_I_BFE : f > GVM_I_BORROW;
        value = f <= GVM_I_COMMON_LAST ? alu(op, immediate, a, b, UNIT_FULL) : immediateForm ? field | ((immediate & 0x400) ? signFill : 0) : other;
    } else [branch] if (group == 3) {   // float
        illegal = immediateForm && (f == GVM_F_MAD || f == GVM_F_MSB || f == GVM_F_NMSB);
        value = fpu(op, a, immediateForm ? (longForm ? immediate : (op >> 20) << 20) : b, c);
    } else [branch] if (group == 2) {
        result = memory_op(op, immediate, a, b, opPc, ending, illegal);
        writes = false;
    } else {   // extended
        writes = opcode <= GVM_X_LPC || (opcode >= GVM_X_MFSR && opcode != GVM_X_MTSR);
        uint mask = field_mask(immediate) << (immediate & 31), number = immediate & 0xfff;
        value = opcode <= GVM_X_BREV ? extended_op(opcode, a, b) : opcode == GVM_X_BFI ? (registers[dest] & ~mask) | ((a << (immediate & 31)) & mask) : base + immediate;
        illegal = opcode == GVM_X_ILL || opcode == 0x0f || (opcode > GVM_X_FENCE && opcode < GVM_X_MFSR) || opcode > GVM_X_SRCLR
               || (opcode >= GVM_X_MFSR && number >= GVM_SR_COUNT);
        [branch] if (illegal) {
        } else if (opcode == GVM_X_SYSCALL) { trap(GVM_CAUSE_SYSCALL, opPc, 0); result = 1; }
        else if (opcode == GVM_X_BREAK) {
            [branch] if (_Flags & 1) { pc = opPc; flags |= F_HALT; ending = E_HALT; result = 3; }
            else { trap(GVM_CAUSE_BREAK, opPc, 0); result = 1; }
        } else if (opcode == GVM_X_WAIT) {   // a user's: the pass's end; a supervisor's: until an interrupt is pending
            pc = opPc + 4;
            bool goes = supervisor && (pending() & irqEnable) != 0;
            flags |= supervisor && !goes ? F_WAIT : 0;
            ending = E_WAIT; result = goes ? 0 : 2;
        } else if (opcode == GVM_X_SYNC) { pc = opPc + 4; flags |= F_SYNC; ending = E_SYNC; result = 2; }
        else if (opcode >= GVM_X_MFSR) {
            [branch] if (!supervisor && (opcode != GVM_X_MFSR || number < GVM_SR_USER_FIRST)) { trap(GVM_CAUSE_PRIV, opPc, op); result = 1; }
            else {
                value = special_read(number);
                [branch] if (opcode != GVM_X_MFSR) special_write(number, opcode == GVM_X_SRSET ? value | a : opcode == GVM_X_SRCLR ? value & ~a : a);
            }
        }
    }
    [branch] if (illegal) { trap(GVM_CAUSE_ILLEGAL, opPc, op); result = 1; }
    opsRun += result == 0 || result == 2 ? 1 : 0;
    registers[result != 0 || !writes || dest == 0 ? 64 : dest] = value;
    return result;
}
uint bundle() {   // the bundle at pc from its word on; NONE when the core goes on, else why the pass ends
    uint base = pc & ~15u, slot = (pc >> 2) & 3, ending = NONE, result = 0;
    [branch] if (base >= _RamEnd) { trap(GVM_CAUSE_FETCH, pc, pc); return NONE; }
    uint4 words = ram(base >> 4);
    uint control = words.w, count = GVM_N(control);
    cycle++;
    [branch] if (count == 3) { trap(GVM_CAUSE_ILLEGAL, base | 12, control); return NONE; }
    uint compareA = registers[GVM_CRA(control)], compareB = registers[GVM_CRB(control)], addFrom = registers[GVM_CRS(control)];
    [loop] for (; slot < count && result == 0; slot++) {
        uint op = slot != 0 ? words.y : words.x;
        bool longForm = ((control >> (5 + slot)) & 1) != 0;
        result = any_op(op, longForm ? words.z : imm12(op), longForm, base, slot, ending);
    }
    [branch] if (result == 1) return NONE;
    [branch] if (result != 0) { pc = ((pc >> 2) & 3) >= count ? base | 12 : pc; return ending; }
    uint kind = GVM_SUB(control), next = base + 16, imm6 = asuint(GVM_CIMM6(control)), sum = compareA + imm6;
    bool special = GVM_CC(control) == GVM_CC_SPECIAL;
    bool bad = special ? (kind == GVM_SUB_ILLEGAL || (kind == GVM_SUB_ADD && GVM_CRS(control) == 0)) : GVM_CC(control) == GVM_CC_ADDBNZ && GVM_CRA(control) == 0;
    [branch] if (bad) { trap(GVM_CAUSE_ILLEGAL, base | 12, control); return NONE; }
    [branch] if (special) {
        [branch] if (kind == GVM_SUB_ERET) {
            [branch] if ((status & GVM_ST_S) == 0) { trap(GVM_CAUSE_PRIV, base | 12, control); return NONE; }
            next = trapPc;
            status = (status & GVM_ST_PIE ? GVM_ST_IE : 0) | (status & GVM_ST_PS ? GVM_ST_S : 0) | GVM_ST_PIE;
        }
        next = special_jump(control, base, next, addFrom);
    } else {
        registers[GVM_CC(control) == GVM_CC_ADDBNZ ? GVM_CRA(control) : 64] = sum;
        next = taken(control, compareA, compareB, imm6, sum) != 0 ? base + asuint(GVM_OFF9(control)) * 16 : next;
    }
    pc = next;
    return NONE;
}
void fetch_current() {   // the fast loop makes whole bundles and takes no interrupt: a bundle entered midway goes the general way
    irqDue = (status & GVM_ST_IE) != 0 && (pending() & irqEnable) != 0;
    current = ram(pc >> 4);
    current.w &= (pc & 15) != 0 || irqDue ? ~7u : 0xffffffffu;
}
uint general_step(uint left) {   // an interrupt if one is due, then a bundle; the budget left, 0 when the pass ends
    [branch] if (irqDue && (pc & 15) == 0) {
        uint which = pending() & irqEnable;
        trap(GVM_CAUSE_IRQ | ((which & 4) ? 2u : (which & 2) ? 1u : 0u), pc, 0);
    }
    budgetLeft = left - 1;
    uint ending = bundle();
    fetch_current();
    endReason = ending != NONE ? ending : endReason;
    return ending != NONE ? 0 : left - 1;
}

// ---- A pass
bool same_as_memory(uint entry, uint4 memory) {   // every word the entry holds is RAM's already
    uint tag = cacheTags[entry];
    bool same = tag != NONE;
    [unroll] for (uint word = 0; word < 4; word++) same = same && (((tag >> (28 + word)) & 1) == 0 || cacheWords[4 * entry + word] == pick(memory, word));
    return same;
}
uint free_way(uint index) { uint4 tags = bucket(index); return tags.x == NONE ? 4 * index : tags.y == NONE ? 4 * index + 1 : tags.z == NONE ? 4 * index + 2 : tags.w == NONE ? 4 * index + 3 : NONE; }
bool make_room() {   // a full write cache: drop the entries that hold RAM's words, then place the stores in the spare entries
    uint group, way, dropped = 0;
    [loop] for (group = 0; group < 8 * bucketCount; group += 8) {
        uint4 memory[8];
        [unroll] for (way = 0; way < 8; way++) memory[way] = ram(cacheTags[group + way] & TEXEL_BITS);
        [unroll] for (way = 0; way < 8; way++) {
            bool same = same_as_memory(group + way, memory[way]);
            [flatten] if (same) cacheTags[group + way] = NONE;
            dropped += same ? 1u : 0u;
        }
    }
    [branch] if (dropped == 0) return false;
    bool placed = true;
    [loop] for (uint entry = spare; entry < spare + 2; entry++) {   // the texel's first bucket, else its second
        uint tag = cacheTags[entry];
        [branch] if (tag != NONE) {
            uint texel = tag & TEXEL_BITS, free = free_way(first_bucket(texel));
            free = free != NONE ? free : free_way(second_bucket(texel));
            [branch] if (free != NONE) {
                cacheTags[free] = tag; cacheTags[entry] = NONE;
                [unroll] for (way = 0; way < 4; way++) cacheWords[4 * free + way] = cacheWords[4 * entry + way];
            } else placed = false;
        }
    }
    return placed;
}
void run(uint left) {
    [branch] if (flags & F_HALT) { endReason = E_HALT; return; }
    [branch] if ((flags & F_SYNC) != 0 && queued != 0) { endReason = E_SYNC; return; }
    flags &= ~F_SYNC;
    [branch] if ((flags & F_WAIT) != 0 && (pending() & irqEnable) == 0) { endReason = E_WAIT; return; }
    flags &= ~F_WAIT;
    fetch_current();
    uint guard = left + 8;   // a hard cap on both loops' trips, whatever the budget does
    [loop] while (left > 0 && guard > 0) {
        uint before = left;
        guard--;
        bundleTexel = pc >> 4;
        opsTimes8 = 0;
        [loop] while (left > 0 && guard > 0 && (current.w & 7) >= fastFrom) { fast_step(left); guard--; }
        cycle += before - left;
        opsRun += opsTimes8 >> 3;
        pc = before != left ? bundleTexel << 4 : pc;
        uint path = current.w & 7;
        [branch] if (left > 0 && (path == GVM_PATH_ALU || path == GVM_PATH_MEMF) && fastFrom == GVM_PATH_FAST) left = whole_step(left);
        else [branch] if (left > 0) left = general_step(left);
    }
}
void load_state(uint which) {
    uint index;
    [unroll] for (index = 0; index < 16; index++) {
        uint4 texel = state_in(index, which);
        registers[4 * index] = texel.x; registers[4 * index + 1] = texel.y; registers[4 * index + 2] = texel.z; registers[4 * index + 3] = texel.w;
    }
    registers[0] = 0;
    [loop] for (index = 0; index <= cacheEntries; index++) cacheTags[index] = NONE;
    storedTexels = 0;
    uint4 machine = state_in(16, which), traps = state_in(17, which), timer = state_in(18, which), counts = state_in(19, which);
    uint4 queueState = state_in(22, which), progress = _OpState.Load(int3(which, 0, 0));
    pc = machine.x; status = machine.y; trapPc = machine.z; cause = machine.w;
    trapValue = traps.x; trapVector = traps.y; scratch = traps.z; irqEnable = traps.w;
    timerLow = timer.x; timerHigh = timer.y; flags = timer.z; randomState = timer.w;
    cycle = counts.x; opsRun = counts.y; endReason = E_BUDGET;
    latched0 = state_in(20, which); latched1 = state_in(21, which);
    queued = min(queueState.x, QUEUE_MAX); queueHead = queueState.y; queueGeneration = queueState.z; refused = queueState.w;
    [unroll] for (index = 0; index < 16; index++) queue[index] = state_in(T_Q + index, which);
    [branch] if (progress.x == queueGeneration) {   // the operations made since: off the queue's front
        uint done = min(progress.y, queued);
        [loop] for (index = 0; index + done < queued; index++) { queue[2 * index] = queue[2 * (index + done)]; queue[2 * index + 1] = queue[2 * (index + done) + 1]; }
        queued -= done; queueHead = progress.z; refused += progress.w;
    }
    queueGeneration++;
}
uint4 state_out(uint texel, uint4 before) {   // before: texel 19 as the pass found it
    uint tagsAt = T_WC + cacheEntries;
    [branch] if (texel < 16) return uint4(registers[4 * texel], registers[4 * texel + 1], registers[4 * texel + 2], registers[4 * texel + 3]);
    [branch] if (texel >= T_WC && texel < tagsAt) { uint at = 4 * (texel - T_WC); return uint4(cacheWords[at], cacheWords[at + 1], cacheWords[at + 2], cacheWords[at + 3]); }
    [branch] if (texel >= tagsAt && texel < tagsAt + (cacheEntries + 3) / 4) {
        uint at = 4 * (texel - tagsAt);
        return uint4(cacheTags[at], cacheTags[at + 1], at + 2 < cacheEntries ? cacheTags[at + 2] : NONE, at + 3 < cacheEntries ? cacheTags[at + 3] : NONE);
    }
    [branch] if (texel >= T_Q && texel < T_WC) return queue[texel - T_Q];
    uint ran = cycle - before.x;
    uint4 counts = uint4(cycle, opsRun, ran, endReason == E_BUDGET && ran < budget && (flags & F_HALT) == 0 ? E_FULL : endReason);
    return texel == 16 ? uint4(pc, status, trapPc, cause) : texel == 17 ? uint4(trapValue, trapVector, scratch, irqEnable)
         : texel == 18 ? uint4(timerLow, timerHigh, flags, randomState) : texel == 19 ? counts : texel == 20 ? latched0
         : texel == 21 ? latched1 : texel == 22 ? uint4(queued, queueHead, queueGeneration, refused) : 0;
}
uint4 again_out(uint texel, uint which) {   // the pass undone (it touched memory past RAM's end): the state as it was, its cache clean
    uint4 value = state_in(texel, which);
    return texel >= T_WC + cacheEntries ? uint4(NONE, NONE, NONE, NONE) : texel >= T_WC ? uint4(0, 0, 0, 0)
         : texel == 18 ? uint4(value.xy, value.z | F_AGAIN, value.w) : texel == 19 ? uint4(value.xyz, E_BUDGET) : value;
}
uint worker_budget() {   // a worker's pass ends about when core 0's does, while a program waits on workers (wad 7)
    uint4 core0 = state_in(19, 0);
    bool capped = core0.w == E_FULL && ram((WAIT_MASK >> 4) + 2).x != 0;
    [branch] if (core0.w == E_SYNC) {   // core 0 rested: capped unless it waits for this worker
        uint4 mask0 = ram(WAIT_MASK >> 4), mask1 = ram((WAIT_MASK >> 4) + 1), mine = (core >> 7) != 0 ? mask1 : mask0;
        capped = any((mask0 | mask1) != 0) && ((pick(mine, (core >> 5) & 3) >> (core & 31)) & 1) == 0;
    }
    return capped ? clamp(core0.z + (core0.z >> 2), 512u, _Ticks) : _Ticks;
}

struct v2f { float4 vertex : SV_POSITION; };
v2f vert(uint id : SV_VertexID) {   // one triangle over the state texture
    v2f result;
    result.vertex = float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1);
    return result;
}
struct tick_out { uint4 t0 : SV_Target0; uint4 t1 : SV_Target1; uint4 t2 : SV_Target2; uint4 t3 : SV_Target3;
                  uint4 t4 : SV_Target4; uint4 t5 : SV_Target5; uint4 t6 : SV_Target6; uint4 t7 : SV_Target7; };
tick_out frag(v2f input) {
    uint2 position = uint2(input.vertex.xy);
    uint row = position.y % 16;
    core = (position.y / 16) * 128 + position.x / 8;
    bucketCount = core == 0 ? 48 : 32;
    cacheEntries = 8 * bucketCount + 2;
    spare = cacheEntries - 2;
    uint pixel = (row / 6) * 48 + (position.x % 8) * 6 + row % 6, statePixels = (T_WC + cacheEntries + (cacheEntries + 3) / 4 + 7) / 8;
    tick_out output;
    [branch] if (row >= (core == 0 ? 12u : 6u) || pixel >= statePixels || core >= _NCores) {
        output.t0 = output.t1 = output.t2 = output.t3 = output.t4 = output.t5 = output.t6 = output.t7 = 0;
        return output;
    }
    external = (pick(_Ext[core >> 7], (core >> 5) & 3) >> (core & 31)) & 1;
    budget = core == 0 ? _Ticks : worker_budget();
    load_state(core);
    fastFrom = (flags & F_AGAIN) != 0 || queued != 0 ? GVM_PATH_INT : GVM_PATH_FAST;   // queued operations: only integer bundles fast
    flags &= ~F_AGAIN;
    highestLoad = 0; highestStore = 0;
    uint start = cycle;
    [loop] for (uint tries = 0; tries <= 8u; tries++) {   // a full cache that make_room() frees runs on with the budget left
        run(budget - (cycle - start));
        bool full = endReason == E_FULL || (endReason == E_BUDGET && (storeFull || cacheTags[spare] != NONE || cacheTags[spare + 1] != NONE));
        [branch] if (tries == 8u || !full) break;
        [branch] if (!make_room()) break;
        storeFull = false;
        endReason = E_BUDGET;
    }
    bool again = highestLoad >= _RamEnd || highestStore >= _RamEnd - GVM_DEV;
    uint4 before = state_in(19, core);
    uint first = pixel * 8;
    output.t0 = again ? again_out(first, core) : state_out(first, before);
    output.t1 = again ? again_out(first + 1, core) : state_out(first + 1, before);
    output.t2 = again ? again_out(first + 2, core) : state_out(first + 2, before);
    output.t3 = again ? again_out(first + 3, core) : state_out(first + 3, before);
    output.t4 = again ? again_out(first + 4, core) : state_out(first + 4, before);
    output.t5 = again ? again_out(first + 5, core) : state_out(first + 5, before);
    output.t6 = again ? again_out(first + 6, core) : state_out(first + 6, before);
    output.t7 = again ? again_out(first + 7, core) : state_out(first + 7, before);
    return output;
}

// ---- The commit: a point a write cache entry, drawn into its texel of RAM in the draw of its word's component
struct c2f { float4 vertex : SV_POSITION; nointerpolation uint value : V; };
c2f commit_vert(uint id : SV_VertexID) {
    bool first = id < WCN0;   // core 0's entries, then 258 a worker
    uint which = first ? 0 : 1 + (id - WCN0) / 258u, entry = first ? id : (id - WCN0) % 258u;
    uint tag = pick(state_in(T_WC + (first ? WCN0 : 258u) + (entry >> 2), which), entry & 3), texel = tag & TEXEL_BITS;
    float2 at = (float2(texel % RAM_W, texel / RAM_W) + 0.5) / float2(RAM_W, _RamRows);
    c2f result;
    result.vertex = tag != NONE && ((tag >> (28 + _Comp)) & 1) != 0 ? float4(at.x * 2 - 1, 1 - at.y * 2, 0, 1) : float4(-2, -2, 0, 1);
    result.value = pick(state_in(T_WC + entry, which), _Comp);
    return result;
}
uint4 commit_frag(c2f input) : SV_Target { return input.value; }

// ---- The operations, in rounds of three draws: stage (each piece's texels as they will be), write (into RAM), progress
// (_OpState: the queue's generation, operations through, pieces made of the next, refused). A piece is up to 4 KB within
// one 4 KB block of its destination; a round makes up to OP_SLOTS of them a core, in the queue's order, each operation
// whole before the next starts, and the next only if it is independent of those before it in the round.
bool operation_valid(Operation op) {
    uint lastWritten = op.destination + op.writeBytes - 1, lastRead = op.source + op.readBytes - 1;
    bool writes = op.writeBytes == 0 || (lastWritten >= op.destination && op.destination >= GVM_DEV && lastWritten < _RamEnd);
    bool reads = op.readBytes == 0 || (lastRead >= op.source && (lastRead < _RamEnd || (op.source >= GVM_ROM && lastRead < GVM_ROM_END)));
    return (op.kind == GVM_OP_COPY || op.kind == GVM_OP_FILL || op.kind == GVM_OP_BLIT) && writes && reads;
}
uint pieces_per_row(Operation op) { return op.kind == GVM_OP_BLIT ? ((op.rowBytes + 4095) >> 12) + 1 : 0; }
uint piece_count(Operation op) {
    return op.kind == GVM_OP_BLIT ? (op.writeBytes != 0 ? op.rows * pieces_per_row(op) : 0)
         : op.count == 0 ? 0 : ((op.destination + op.count - 1) >> 12) - (op.destination >> 12) + 1;
}
bool overlap(uint first, uint firstLength, uint second, uint secondLength) {
    return firstLength != 0 && secondLength != 0 && first < second + secondLength && second < first + firstLength;
}
uint texel_span(Operation op) { return op.writeBytes != 0 ? ((op.destination + op.writeBytes - 1) >> 4) - (op.destination >> 4) + 1 : 0; }
bool in_order(Operation op) {   // a blit whose rows share texels or read what it writes: a piece a round, in order
    return op.kind == GVM_OP_BLIT && op.writeBytes != 0
        && ((op.rows > 1 && op.destinationStride < op.rowBytes + 15) || overlap(op.destination, op.writeBytes, op.source, op.readBytes));
}
bool conflict(Operation op, Operation earlier) {   // op reads what earlier writes, or both write one texel
    return overlap(op.source, op.readBytes, earlier.destination, earlier.writeBytes)
        || overlap(op.destination >> 4, texel_span(op), earlier.destination >> 4, texel_span(earlier));
}
void piece_of(Operation op, uint piece, out uint start, out uint end, out uint from) {   // bytes [start, end) from from
    bool blit = op.kind == GVM_OP_BLIT, backward = op.kind == GVM_OP_COPY && op.destination > op.source && op.destination - op.source < op.count;
    uint perRow = blit ? pieces_per_row(op) : piece_count(op), row = blit ? piece / perRow : 0;
    uint block = blit ? piece % perRow : backward ? perRow - 1 - piece : piece;
    uint rowStart = op.destination + row * op.destinationStride, rowSource = op.source + row * op.sourceStride;
    uint length = blit ? op.rowBytes : op.count, aligned = rowStart & ~4095u;
    start = max(rowStart, aligned + block * 4096);
    end = max(start, min(rowStart + length, aligned + (block + 1) * 4096));
    from = rowSource + (start - rowStart);
}
Operation operation_at(uint which, uint index) { return operation_of(state_in(T_Q + 2 * index, which), state_in(T_Q + 2 * index + 1, which)); }
struct Plan { uint done, pieces, refusedCount, index, piece; bool hit; };
Plan plan(uint which, uint wanted) {   // core which's round: slot wanted's operation and piece, and the progress after the round
    uint4 queueState = state_in(22, which), progress = _OpState.Load(int3(which, 0, 0));
    bool fresh = progress.x != queueState.z;
    Plan result;
    result.done = fresh ? 0 : progress.y; result.pieces = fresh ? queueState.y : progress.z; result.refusedCount = fresh ? 0 : progress.w;
    result.index = 0; result.piece = 0; result.hit = false;
    uint used = 0, first = result.done;
    bool stop = which >= _NCores;
    [loop] for (uint index = result.done; index < min(queueState.x, QUEUE_MAX) && !stop; index++) {
        Operation op = operation_at(which, index);
        bool valid = operation_valid(op), independent = true;
        [loop] for (uint earlier = first; earlier < min(index, QUEUE_MAX) && valid; earlier++) {
            Operation before = operation_at(which, earlier);
            independent = independent && !(operation_valid(before) && conflict(op, before));
        }
        [branch] if (!valid) { result.refusedCount++; result.done = index + 1; result.pieces = 0; }
        else [branch] if (!independent) stop = true;
        else {
            uint total = piece_count(op), remaining = total - result.pieces;
            uint taking = min(in_order(op) ? min(remaining, 1u) : remaining, OP_SLOTS - used);
            [flatten] if (wanted - used < taking) { result.hit = true; result.index = index; result.piece = result.pieces + wanted - used; }
            used += taking;
            result.pieces += taking;
            stop = result.pieces < total || used == OP_SLOTS;
            [flatten] if (result.pieces >= total) { result.done = index + 1; result.pieces = 0; }
        }
    }
    return result;
}
struct o2f { float4 vertex : SV_POSITION; nointerpolation uint4 j0 : J0; nointerpolation uint4 j1 : J1; };
float4 corner(uint vertexId, float left, float right, float top, float bottom, float width, float height, bool shown) {   // a quad, six vertices
    uint index = vertexId % 6;
    float x = (index == 1 || index == 3 || index == 4) ? right : left, y = (index == 2 || index == 4 || index == 5) ? bottom : top;
    return shown ? float4(x / width * 2 - 1, 1 - y / height * 2, 0, 1) : float4(-2, -2, 0, 1);
}
void slot_piece(uint instance, out Operation op, out uint start, out uint end, out uint from, out bool shown) {
    Plan found = plan(instance / OP_SLOTS, instance % OP_SLOTS);
    op = operation_at(instance / OP_SLOTS, found.index);
    piece_of(op, found.piece, start, end, from);
    shown = found.hit && end > start;
}
o2f op_stage_vert(uint vertexId : SV_VertexID, uint instance : SV_InstanceID) {
    Operation op; uint start, end, from; bool shown;
    slot_piece(instance, op, start, end, from, shown);
    uint texels = ((end - 1) >> 4) - (start >> 4) + 1, shift = (op.destination & 3) * 8, period = start - from;
    o2f result;
    result.vertex = corner(vertexId, 0, (float)texels, (float)instance, (float)(instance + 1), (float)OP_SPAN, (float)(_NCores * OP_SLOTS), shown);
    result.j0 = uint4(start, end, from, op.kind);
    result.j1 = uint4(shift != 0 ? (op.source << shift) | (op.source >> (32 - shift)) : op.source,   // a fill's word, turned to the destination's bytes
                      op.kind == GVM_OP_BLIT && period != 0 && period < end - start ? period : 0, 0, 0);
    return result;
}
uint source_byte(uint address) { return (pick(data_texel(address), (address >> 2) & 3) >> ((address & 3) * 8)) & 0xffu; }
uint4 op_stage_frag(o2f input) : SV_Target {
    uint start = input.j0.x, end = input.j0.y, from = input.j0.z, texel = (start >> 4) + (uint)input.vertex.x, index;
    uint low = max(start, texel * 16) - texel * 16, high = min(end, texel * 16 + 16) - texel * 16, sourceAt = from + (texel * 16 - start);
    uint4 old = ram(texel), made = input.j1.x;
    [branch] if (input.j0.w != GVM_OP_FILL) {
        [branch] if (input.j1.y != 0) {   // a blit's row that reads what it writes: a byte at a time, as made in order
            [loop] for (index = 0; index < 16; index++) {
                uint value = source_byte(from + (texel * 16 + index - start) % input.j1.y);
                made = put(made, index >> 2, (pick(made, index >> 2) & ~(0xffu << (8 * (index & 3)))) | (value << (8 * (index & 3))));
            }
        } else {   // two source texels, their bytes moved into place
            uint4 lower = data_texel(sourceAt), upper = data_texel(sourceAt + 16);
            uint words[8] = { lower.x, lower.y, lower.z, lower.w, upper.x, upper.y, upper.z, upper.w };
            uint offset = sourceAt & 15, shift = (offset & 3) * 8;
            [unroll] for (index = 0; index < 4; index++) {
                uint here = words[(offset >> 2) + index], after = words[(offset >> 2) + index + 1];
                made = put(made, index, shift != 0 ? (here >> shift) | (after << (32 - shift)) : here);
            }
        }
    }
    [unroll] for (index = 0; index < 4; index++) {   // the bytes [low, high) of the texel are the piece's
        uint first = clamp((int)low - 4 * (int)index, 0, 4), last = clamp((int)high - 4 * (int)index, 0, 4);
        uint mask = (last >= 4 ? 0xffffffffu : (1u << (8 * last)) - 1) & ~(first >= 4 ? 0xffffffffu : (1u << (8 * first)) - 1);
        old = put(old, index, pick(old, index) ^ ((pick(old, index) ^ pick(made, index)) & mask));
    }
    return old;
}
o2f op_write_vert(uint vertexId : SV_VertexID, uint instance : SV_InstanceID) {   // two quads: the piece's texels on two rows
    Operation op; uint start, end, from; bool shown;
    slot_piece(instance, op, start, end, from, shown);
    uint firstTexel = start >> 4, lastTexel = (end - 1) >> 4, x = firstTexel % RAM_W, y = firstTexel / RAM_W, xEnd = x + lastTexel - firstTexel + 1;
    bool second = vertexId >= 6;
    o2f result;
    result.vertex = corner(vertexId, second ? 0 : (float)x, (float)(second ? xEnd - RAM_W : min(xEnd, RAM_W)), (float)(y + (second ? 1 : 0)),
                           (float)(y + (second ? 2 : 1)), (float)RAM_W, (float)_RamRows, shown && (!second || xEnd > RAM_W));
    result.j0 = uint4(firstTexel, lastTexel, instance, 0);
    result.j1 = 0;
    return result;
}
uint4 op_write_frag(o2f input) : SV_Target {
    uint2 position = uint2(input.vertex.xy);
    uint texel = position.y * RAM_W + position.x;
    if (texel < input.j0.x || texel > input.j0.y) discard;
    return _Stage.Load(int3(texel - input.j0.x, input.j0.z, 0));
}
struct p2f { float4 vertex : SV_POSITION; nointerpolation uint4 value : V; };
p2f op_prog_vert(uint which : SV_VertexID) {
    Plan found = plan(which, NONE);
    p2f result;
    result.vertex = float4((float)(2 * which + 1) / (float)_NCores - 1, 0, 0, 1);   // (fxc2 took (float)c's bits for c in the loads)
    result.value = uint4(state_in(22, which).z, found.done, found.pieces, found.refusedCount);
    return result;
}
uint4 op_prog_frag(p2f input) : SV_Target { return input.value; }
