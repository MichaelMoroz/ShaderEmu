#!/usr/bin/env python3
"""What each candidate feature of an ISA is worth on real traces of core 0 (traces.ps1): python features.py [DIR] [--json FILE]
All counts are measured on the trace (rvc_cpu); nothing here is a time. Shares are of instructions unless a line says otherwise."""
import json, os, sys
import numpy as np
DIR = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\micha\agents_isa\ni\trace'
def sx(v, b): return ((v.astype(np.int64) & ((1 << b) - 1)) ^ (1 << (b - 1))) - (1 << (b - 1))
out = {}
for name in ['boot', 'tdawn', 'gzip', 'mp3', 'jpeg']:
    f = os.path.join(DIR, name + '.bin')
    if not os.path.exists(f) or os.path.getsize(f) == 0: continue
    r = np.fromfile(f, dtype=np.uint32).reshape(-1, 4)
    pc, w, a, b = r[:, 0], r[:, 1], r[:, 2], r[:, 3]
    n = len(pc) - 1
    nx = np.append(pc[1:], 0)
    op = w & 0x7f; f3 = (w >> 12) & 7; rd = (w >> 7) & 31; rs1 = (w >> 15) & 31; rs2 = (w >> 20) & 31; f7 = w >> 25
    isM = (op == 0x33) & (f7 == 1)
    alu = (op == 0x13) | ((op == 0x33) & ~isM)
    ld = op == 0x03; st = op == 0x23; flw = op == 0x07; fsw = op == 0x27
    br = op == 0x63; jal = op == 0x6f; jalr = op == 0x67; lui = op == 0x37; auipc = op == 0x17
    fp = (op == 0x53) | ((op & 0x73) == 0x43); amo = op == 0x2f; sys_ = op == 0x73
    taken = nx != pc + 4
    S = {}
    S['n'] = n
    for k, m in dict(alu=alu, load=ld | flw, store=st | fsw, branch=br, jal=jal, jalr=jalr, lui_auipc=lui | auipc, mul=isM, fp=fp, amo=amo, sys=sys_).items():
        S[k] = float(m.mean())
    S['taken_branch'] = float((br & taken).mean())
    S['all_taken'] = float(taken.mean())
    # fetch: entering a texel by a jump waits on the texture; sequential entry was read ahead
    t = (pc & 0x7fffffff) >> 4
    new_seq = (np.append(t[0], t[:-1]) + 1 == t)
    same_t = np.append(t[0], t[:-1]) == t
    jump_t = ~new_seq & ~same_t
    S['fetch_jump_new_texel'] = float(jump_t.mean())
    S['fetch_seq_new_texel'] = float(new_seq.mean())
    # a window of the last K texels fetched (an i-side buffer, what a jump back finds)
    def lru_hit(ts, k):
        cache, hits = [], 0
        for x in ts.tolist():
            if x in cache:
                hits += 1; cache.remove(x)
            elif len(cache) >= k: cache.pop(0)
            cache.append(x)
        return hits / max(1, len(ts))
    def dm_hit(ts, bits):
        m = (1 << bits) - 1; tag = np.full(1 << bits, -1, dtype=np.int64); h = 0
        for x in ts.tolist():
            i = (x ^ (x >> bits)) & m
            if tag[i] == x: h += 1
            tag[i] = x
        return h / max(1, len(ts))
    jt = t[jump_t][:400000]
    S['jump_texel_lru4'] = lru_hit(jt, 4); S['jump_texel_lru16'] = lru_hit(jt, 16); S['jump_texel_dm64'] = dm_hit(jt, 6); S['jump_texel_dm256'] = dm_hit(jt, 8)
    # data
    mem = ld | flw | st | fsw
    imm_i = sx(w >> 20, 12); imm_s = sx(((w >> 25) << 5) | ((w >> 7) & 31), 12)
    va = (a.astype(np.int64) + np.where(st | fsw, imm_s, imm_i)) & 0xffffffff
    S['mem_sp'] = float((rs1[mem] == 2).mean())
    S['mem_s0'] = float((rs1[mem] == 8).mean())
    S['load_word'] = float(((f3 == 2) & (ld | flw))[ld | flw].mean())
    S['store_word'] = float(((f3 == 2) & (st | fsw))[st | fsw].mean())
    S['mem_ram'] = float((va[mem] >= 0x82000000).mean())
    mt = va[mem] >> 4; isl = (ld | flw)[mem]
    prev = np.append(-1, mt[:-1])
    S['mem_same_texel_as_last_mem'] = float((mt == prev).mean())
    S['load_same_texel_as_last_mem'] = float((mt == prev)[isl].mean())
    # the dr_t shortcut of lean: the texel read last for data
    lt = mt[isl][:600000]
    S['load_lru4'] = lru_hit(lt, 4); S['load_lru16'] = lru_hit(lt, 16); S['load_dm32'] = dm_hit(lt, 5); S['load_dm64'] = dm_hit(lt, 6); S['load_dm128'] = dm_hit(lt, 7)
    # pass by pass: what the write cache sees (8192 instructions a pass)
    idx = np.nonzero(mem)[0]; pass_no = idx // 8192
    stm = (st | fsw)[mem]
    texels, words, hits_dirty, loads, stores = [], [], 0, 0, 0
    full_at = []
    for p in np.unique(pass_no):
        sel = pass_no == p
        tt = mt[sel]; ss = stm[sel]; vv = va[mem][sel] >> 2
        seen = set(); seenw = set()
        for x, s, y in zip(tt.tolist(), ss.tolist(), vv.tolist()):
            if s:
                stores += 1; seen.add(x); seenw.add(y)
            else:
                loads += 1
                if x in seen: hits_dirty += 1
        texels.append(len(seen)); words.append(len(seenw))
    S['pass_store_texels_mean'] = float(np.mean(texels)); S['pass_store_texels_p95'] = float(np.percentile(texels, 95)); S['pass_store_words_mean'] = float(np.mean(words)); S['pass_store_words_p95'] = float(np.percentile(words, 95))
    S['load_finds_dirty_texel'] = hits_dirty / max(1, loads)
    # pairs
    w1 = np.append(w[1:], 0); op1 = w1 & 0x7f; rs11 = (w1 >> 15) & 31; rs21 = (w1 >> 20) & 31; rd1 = (w1 >> 7) & 31
    uses_rs1 = ~np.isin(op1, [0x37, 0x17, 0x6f]); uses_rs2 = np.isin(op1, [0x33, 0x23, 0x27, 0x63])
    use_next = (rd != 0) & ((uses_rs1 & (rs11 == rd)) | (uses_rs2 & (rs21 == rd)))
    S['load_used_by_next'] = float(use_next[ld | flw].mean())
    alu1 = (op1 == 0x13) | ((op1 == 0x33) & ((w1 >> 25) != 1))
    S['p_lui_addi'] = float(((lui | auipc) & (op1 == 0x13) & (rs11 == rd) & (rd1 == rd)).mean())
    S['p_lui_mem'] = float(((lui | auipc) & np.isin(op1, [3, 0x23, 7, 0x27]) & (rs11 == rd)).mean())
    S['p_auipc_jalr'] = float((auipc & (op1 == 0x67) & (rs11 == rd)).mean())
    S['p_cmp_branch'] = float((alu & (op1 == 0x63) & use_next).mean())
    S['p_alu_branch_any'] = float((alu & (op1 == 0x63)).mean())
    S['p_shift_add'] = float((alu & ((op == 0x13) & (f3 == 1)) & (op1 == 0x33) & use_next).mean())
    S['p_alu_mem_addr'] = float((alu & np.isin(op1, [3, 0x23, 7, 0x27]) & (rs11 == rd) & (rd != 0)).mean())
    S['p_load_alu_use'] = float(((ld | flw) & alu1 & use_next).mean())
    S['p_load_branch_use'] = float(((ld | flw) & (op1 == 0x63) & use_next).mean())
    S['p_alu_alu_chain'] = float((alu & alu1 & use_next).mean())
    S['p_alu_alu_any'] = float((alu & alu1).mean())
    S['p_load_load'] = float(((ld | flw) & np.isin(op1, [3, 7])).mean())
    S['p_store_store'] = float(((st | fsw) & np.isin(op1, [0x23, 0x27])).mean())
    S['p_load_load_same_texel'] = 0.0
    both = (ld | flw)[:-1] & np.isin(op1, [3, 7])[:-1]
    vt1 = np.append(va[1:], 0) >> 4
    S['p_load_load_same_texel'] = float((both & ((va >> 4)[:-1] == vt1[:-1])).mean())
    stboth = (st | fsw)[:-1] & np.isin(op1, [0x23, 0x27])[:-1]
    S['p_store_store_same_texel'] = float((stboth & ((va >> 4)[:-1] == vt1[:-1])).mean())
    # immediates and registers
    S['imm_form_of_alu'] = float((op == 0x13)[alu].mean())
    S['mv_li'] = float(((op == 0x13) & (f3 == 0) & ((w >> 20) == 0 | (rs1 == 0))).mean())
    # block length and footprint
    S['block_len'] = float(n / max(1, taken.sum()))
    uniq, cnt = np.unique(pc, return_counts=True)
    c = np.sort(cnt)[::-1].cumsum() / cnt.sum()
    S['words_50'] = int(np.searchsorted(c, .5)); S['words_90'] = int(np.searchsorted(c, .9)); S['words_all'] = len(uniq)
    out[name] = S
keys = list(next(iter(out.values())).keys())
print(f"{'':30}" + ''.join(f'{k:>10}' for k in out))
for k in keys:
    print(f'{k:30}' + ''.join((f'{out[n][k]*100:9.2f}%' if isinstance(out[n][k], float) and out[n][k] <= 1.0001 else f'{out[n][k]:10.1f}') for n in out))
json.dump(out, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'features.json'), 'w'), indent=1)
