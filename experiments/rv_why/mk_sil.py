#!/usr/bin/env python3
"""gpu_vm's tick with a store of what is there already taking no write cache entry (lean's rule): python mk_sil.py OUT.hlsl [--wc3 NB]
Reads ../GPU-VM/tick/tick.hlsl. --wc3 NB: three tables of three (NB, NB/2, NB/4 buckets) in place of two of four."""
import argparse, os
ap = argparse.ArgumentParser(); ap.add_argument('out'); ap.add_argument('--tree', default=r'D:\Development\GPU-VM'); ap.add_argument('--wc3', type=int, default=0)
a = ap.parse_args()
t = open(os.path.join(a.tree, 'tick', 'tick.hlsl'), newline='').read().replace('\r\n', '\n')
def rep(old, new):
    global t
    assert t.count(old) == 1, old
    t = t.replace(old, new)
# paths 3 and 5: a load or a store, and a lite op
rep('''            [branch] if ((((t.x ^ op_bit(8)) & op_bit(11)) | (m0.ad & 3)) != 0) m0.q = data_texel(m0.ad);
            smax''', '''            m0.q = data_texel(m0.ad);
            smax''')
rep('''            bool nr = st0 && sl0 == NONE;
            uint at0 = st0 ? (nr ? WCS : sl0) : WCN;
            wt[at0] = stag(slot_tag(m0, sl0), m0.t, m0.i);
            wd[4 * at0 + m0.i] = merge(t.x, w0, m0.ad, r[d0]);
            bloom |= st0 ? m0.t : 0;
''', '''            uint val0 = merge(t.x, w0, m0.ad, r[d0]);
            bool put0 = st0 && val0 != w0;   // (a store of what is there takes no entry)
            bool nr = put0 && sl0 == NONE;
            uint at0 = put0 ? (nr ? WCS : sl0) : WCN;
            wt[at0] = stag(slot_tag(m0, sl0), m0.t, m0.i);
            wd[4 * at0 + m0.i] = val0;
            bloom |= put0 ? m0.t : 0;
''')
# path 6: a store, then a load or a store
rep('''            [branch] if (((t.x & op_bit(3)) | (m0.ad & 3)) != 0) m0.q = data_texel(m0.ad);
            uint w0 = seen(m0), sl0 = slot(m0), s0x = sl0 == NONE ? WCS : sl0;
            uint tg0 = stag(slot_tag(m0, sl0), m0.t, m0.i);
            wt[s0x] = tg0; wd[4 * s0x + m0.i] = merge(t.x, w0, m0.ad, r[d0]);
            bloom |= m0.t;
''', '''            m0.q = data_texel(m0.ad);
            uint w0 = seen(m0), sl0 = slot(m0), val0 = merge(t.x, w0, m0.ad, r[d0]);
            bool put0 = val0 != w0;
            uint s0x = !put0 ? WCN : sl0 == NONE ? WCS : sl0;
            uint tg0 = stag(slot_tag(m0, sl0), m0.t, m0.i);
            wt[s0x] = tg0; wd[4 * s0x + m0.i] = val0;
            bloom |= put0 ? m0.t : 0;
''')
rep('bool over = m0.t == m1.t && sl0 == NONE;', 'bool over = m0.t == m1.t && sl0 == NONE && put0;')
rep('''            [branch] if ((((t.y ^ op_bit(8)) & op_bit(11)) | (m1.ad & 3)) != 0) m1.q = data_texel(m1.ad);
''', '''            m1.q = data_texel(m1.ad);
''')
rep('''            bool nr = sl0 == NONE || (st1 && sl1 == NONE);
            uint at1 = st1 ? (sl1 == NONE ? WCS + 1 : sl1) : WCN;
            wt[at1] = stag(m1.otag, m1.t, m1.i);
            wd[4 * at1 + m1.i] = merge(t.y, w1, m1.ad, r[d1]);
            bloom |= st1 ? m1.t : 0;
''', '''            uint val1 = merge(t.y, w1, m1.ad, r[d1]);
            bool put1 = st1 && val1 != w1;
            bool nr = (put0 && sl0 == NONE) || (put1 && sl1 == NONE);
            uint at1 = put1 ? (sl1 == NONE ? WCS + 1 : sl1) : WCN;
            wt[at1] = stag(m1.otag, m1.t, m1.i);
            wd[4 * at1 + m1.i] = val1;
            bloom |= put1 ? m1.t : 0;
''')
# step12's memory path
rep('''        ok = !stv || sl != NONE;
        uint at = stv && ok ? sl : WCN;
        wt[at] = stag(slot_tag(m, sl), m.t, m.i);
        wd[4 * at + m.i] = merge(t.x, word, m.ad, r[d0]);
        bloom |= stv ? m.t : 0;
''', '''        uint val = merge(t.x, word, m.ad, r[d0]);
        bool putv = stv && val != word;
        ok = !putv || sl != NONE;
        uint at = putv && ok ? sl : WCN;
        wt[at] = stag(slot_tag(m, sl), m.t, m.i);
        wd[4 * at + m.i] = val;
        bloom |= putv ? m.t : 0;
''')
open(a.out, 'w', newline='\n').write(t)
print('written', a.out)
