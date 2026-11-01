"""The no-MMU Linux kernel translated to Udon bytecode at build time, with an interpreter in Udon
for code that is not the kernel's text (its programs) and for the system instructions (README.md).

python linux2udon.py linux_payload.bin OUT_DIR [rootfs.bin PROGRAM...]
writes OUT_DIR/prog.bc (big-endian Udon bytecode), prog.heap, prog.externs, prog.syms and the
tables (NAME.u32 / NAME.i32) the runner (editor/UdonLinux.cs) puts in the heap. The PROGRAMs
(paths in the ROM, FLAT binaries) are translated too, to run wherever the kernel loads them.
"""
import os, struct, sys, time

RAM, MIN, MASK = 0x80000000, 0x80000000, 0xFFFFFFFF
RAMSIZE = 0x12000000
ROM = 0x40000000
RTC0, RTC1 = 0x12000020, 0x26101901
TEXT, TEND = 0x82000000, 0x822d63ec
COUNT = os.environ.get("L2U_COUNT", "1") == "1"    # count translated instructions (gcount), for rdcycle

I32 = "SystemInt32.__op_%s__SystemInt32_SystemInt32__SystemInt32"
CMP = "SystemInt32.__op_%s__SystemInt32_SystemInt32__SystemBoolean"
I64 = "SystemInt64.__op_%s__SystemInt64_SystemInt64__SystemInt64"
I64SH = "SystemInt64.__op_%s__SystemInt64_SystemInt32__SystemInt64"
I64EQ = "SystemInt64.__op_Equality__SystemInt64_SystemInt64__SystemBoolean"
AGET = "SystemInt32Array.__Get__SystemInt32__SystemInt32"
ASET = "SystemInt32Array.__Set__SystemInt32_SystemInt32__SystemVoid"
UGET = "SystemUInt32Array.__Get__SystemInt32__SystemUInt32"
B2I = "SystemConvert.__ToInt32__SystemBoolean__SystemInt32"
L2I = "SystemConvert.__ToInt32__SystemInt64__SystemInt32"
I2L = "SystemConvert.__ToInt64__SystemInt32__SystemInt64"
BIGMUL = "SystemMath.__BigMul__SystemInt32_SystemInt32__SystemInt64"
CLAMP = "UnityEngineMathf.__Clamp__SystemInt32_SystemInt32_SystemInt32__SystemInt32"
ACOPY = "SystemArray.__Copy__SystemArray_SystemInt32_SystemArray_SystemInt32_SystemInt32__SystemVoid"
ACLEAR = "SystemArray.__Clear__SystemArray_SystemInt32_SystemInt32__SystemVoid"
ADD, SUB, AND, OR, XOR = (I32 % n for n in ("Addition", "Subtraction", "LogicalAnd", "LogicalOr", "LogicalXor"))
SHL, SHR, MUL = I32 % "LeftShift", I32 % "RightShift", I32 % "Multiplication"
EQ, NE, LT, GT, LE, GE = (CMP % n for n in ("Equality", "Inequality", "LessThan", "GreaterThan",
                                            "LessThanOrEqual", "GreaterThanOrEqual"))

T_I, T_U, T_B, T_L, T_S, T_IA, T_UA, T_BY, T_F = range(9)   # heap slot types, as the runner reads them (a float as its bits)

FOP = "SystemSingle.__op_%s__SystemSingle_SystemSingle__SystemSingle"
FCMP = "SystemSingle.__op_%s__SystemSingle_SystemSingle__SystemBoolean"
FNEG = "SystemSingle.__op_UnaryMinus__SystemSingle__SystemSingle"
BITS2F = "SystemBitConverter.__Int32BitsToSingle__SystemInt32__SystemSingle"
F2BITS = "SystemBitConverter.__SingleToInt32Bits__SystemSingle__SystemInt32"
FMATH = "UnityEngineMathf.__%s__SystemSingle__SystemSingle"
I2F = "SystemConvert.__ToSingle__SystemInt32__SystemSingle"
L2F = "SystemConvert.__ToSingle__SystemInt64__SystemSingle"
F2I = "SystemConvert.__ToInt32__SystemSingle__SystemInt32"


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


class Asm:
    """Udon bytecode with labels; heap slots by name ('#i..' Int32 and '#u..' label constants)."""

    def __init__(self):
        self.code = bytearray()
        self.labels, self.fix, self.hfix = {}, [], []
        self.slots, self.heap, self.externs = {}, [], []
        self.n = 0
        self.later = []           # out-of-line stubs, emitted after the text

    def var(self, name, typ, val=0):
        assert name not in self.slots, name
        self.slots[name] = len(self.heap)
        self.heap.append([typ, val])
        return name

    def sl(self, name):
        r = self.slots.get(name)
        if r is not None: return r
        r = self.slots[name] = len(self.heap)
        if name[1] == "i": self.heap.append([T_I, sx(int(name[2:], 16), 32)])
        elif name[1] == "u": self.heap.append([T_U, 0]); self.hfix.append((r, name[2:]))
        elif name[1] == "e": self.heap.append([T_S, len(self.externs)]); self.externs.append(name[2:])
        else: raise KeyError(name)
        return r

    def c(self, v): return "#i%08x" % (v & MASK)
    def u(self, label): return "#u" + label
    def new(self): self.n += 1; return "_%d" % self.n

    def L(self, name):
        assert name not in self.labels, name
        self.labels[name] = len(self.code)

    def push(self, a): self.code += struct.pack(">II", 1, self.sl(a))

    def ex(self, sig, *args):
        for a in args: self.code += struct.pack(">II", 1, self.sl(a))
        self.code += struct.pack(">II", 6, self.sl("#e" + sig))

    def copy(self, a, b):
        self.code += struct.pack(">IIIII", 1, self.sl(a), 1, self.sl(b), 9)

    def jump(self, label):
        self.fix.append((len(self.code) + 4, label))
        self.code += struct.pack(">II", 5, 0)

    def jif(self, cnd, label):        # jumps when cnd is false
        self.code += struct.pack(">II", 1, self.sl(cnd))
        self.fix.append((len(self.code) + 4, label))
        self.code += struct.pack(">II", 4, 0)

    def jind(self, a): self.code += struct.pack(">II", 8, self.sl(a))
    def exit(self): self.code += struct.pack(">II", 5, 0xFFFFFFFC)

    def finish(self):
        for at, label in self.fix: struct.pack_into(">I", self.code, at, self.labels[label])
        for slot, label in self.hfix: self.heap[slot][1] = self.labels[label]


def x(r): return "x%d" % r


# ---------------------------------------------------------------- shared pieces

def emit_mul(o, f3, a, b, rd):
    if f3 == 0: o.ex(MUL, a, b, rd); return
    if f3 <= 3:
        o.ex(BIGMUL, a, b, "l"); o.ex(I64SH % "RightShift", "l", o.c(32), "l"); o.ex(L2I, "l", "t3")
        # unsigned high halves from the signed one: + b where a < 0 (mulhu), + a where b < 0
        if f3 == 3:
            o.ex(SHR, a, o.c(31), "t"); o.ex(AND, "t", b, "t"); o.ex(ADD, "t3", "t", "t3")
        if f3 in (2, 3):
            o.ex(SHR, b, o.c(31), "t"); o.ex(AND, "t", a, "t"); o.ex(ADD, "t3", "t", "t3")
        o.copy("t3", rd); return
    # division in 64 bits, where MIN / -1 cannot throw; x / 0 apart; no Int64 remainder extern
    o.ex(I2L, a, "l"); o.ex(I2L, b, "l2")
    if f3 in (5, 7):
        o.ex(I64 % "LogicalAnd", "l", "k_u32", "l"); o.ex(I64 % "LogicalAnd", "l2", "k_u32", "l2")
    nz, done = o.new(), o.new()
    o.ex(I64EQ, "l2", "k_l0", "cnd"); o.jif("cnd", nz)
    o.copy(o.c(MASK) if f3 < 6 else a, "t3"); o.jump(done)
    o.L(nz)
    if f3 < 6: o.ex(I64 % "Division", "l", "l2", "l")
    else:
        o.ex(I64 % "Division", "l", "l2", "l3"); o.ex(I64 % "Multiplication", "l3", "l2", "l3")
        o.ex(I64 % "Subtraction", "l", "l3", "l")
    o.ex(I64SH % "LeftShift", "l", o.c(32), "l"); o.ex(I64SH % "RightShift", "l", o.c(32), "l")
    o.ex(L2I, "l", "t3")
    o.L(done)
    o.copy("t3", rd)


def emit_srl(o, a, b, rd):
    """Logical shift by a register: arithmetic shift, then clear the top bits ~((MIN >> s) << 1)."""
    o.ex(SHR, o.c(MIN), b, "t"); o.ex(SHL, "t", o.c(1), "t"); o.ex(XOR, "t", o.c(MASK), "t")
    o.ex(SHR, a, b, "t2"); o.ex(AND, "t2", "t", rd)


def emit_insert(o, width, v):
    """Writes v's low width bits into word w at the byte t's low bits say; stores w at idx."""
    o.ex(AND, "t", o.c(3 if width == 8 else 2), "sh"); o.ex(SHL, "sh", o.c(3), "sh")
    o.ex(SHL, o.c((1 << width) - 1), "sh", "t2"); o.ex(XOR, "t2", o.c(MASK), "t2")
    o.ex(AND, "w", "t2", "w")
    o.ex(AND, v, o.c((1 << width) - 1), "t2"); o.ex(SHL, "t2", "sh", "t2"); o.ex(OR, "w", "t2", "w")
    o.ex(ASET, "mem", "idx", "w")


def emit_extract(o, f3, rd):
    """A byte or half of word w at t's low bits, sign- or zero-extended, into rd."""
    width = 8 if f3 in (0, 4) else 16
    o.ex(AND, "t", o.c(3 if width == 8 else 2), "sh"); o.ex(SHL, "sh", o.c(3), "sh")
    if f3 in (4, 5):
        o.ex(SHR, "w", "sh", "w"); o.ex(AND, "w", o.c((1 << width) - 1), rd)
    else:
        o.ex(SUB, o.c(32 - width), "sh", "sh"); o.ex(SHL, "w", "sh", "w"); o.ex(SHR, "w", o.c(32 - width), rd)


def sync_to_xr(o):
    for r in range(1, 32): o.ex(ASET, "xr", o.c(r), x(r))


def sync_from_xr(o):
    for r in range(1, 32): o.ex(AGET, "xr", o.c(r), x(r))


# ---------------------------------------------------------------- the translated kernel text

def decode(w):
    op = w & 0x7F
    d = dict(w=w, op=op, rd=(w >> 7) & 31, f3=(w >> 12) & 7, rs1=(w >> 15) & 31, rs2=(w >> 20) & 31, f7=w >> 25)
    i_imm = sx(w >> 20, 12)
    if op == 0x37: d.update(k="lui", imm=w & 0xFFFFF000)
    elif op == 0x17: d.update(k="auipc", imm=w & 0xFFFFF000)
    elif op == 0x6F:
        d.update(k="jal", imm=sx(((w >> 31) << 20) | (((w >> 12) & 255) << 12) | (((w >> 20) & 1) << 11) | (((w >> 21) & 1023) << 1), 21))
    elif op == 0x67 and d["f3"] == 0: d.update(k="jalr", imm=i_imm)
    elif op == 0x63 and d["f3"] not in (2, 3):
        d.update(k="branch", imm=sx(((w >> 31) << 12) | (((w >> 7) & 1) << 11) | (((w >> 25) & 63) << 5) | (((w >> 8) & 15) << 1), 13))
    elif op == 0x03 and d["f3"] in (0, 1, 2, 4, 5): d.update(k="load", imm=i_imm)
    elif op == 0x23 and d["f3"] in (0, 1, 2): d.update(k="store", imm=sx(((w >> 25) << 5) | ((w >> 7) & 31), 12))
    elif op == 0x13: d.update(k="opimm", imm=i_imm)
    elif op == 0x33 and d["f7"] in (0, 1, 0x20) and (d["f7"] != 0x20 or d["f3"] in (0, 5)): d.update(k="op")
    elif op == 0x0F: d.update(k="fence")
    elif op == 0x2F and d["f3"] == 2 and (w >> 27) in (0, 1, 2, 3, 4, 8, 12, 16, 20, 24, 28): d.update(k="amo", f5=w >> 27)
    else: d.update(k="interp")       # system, floating point, anything else: the interpreter's
    return d


class Seg:
    """Text translated as one piece: the kernel's at its address, or a program's at its file offsets,
    which run at ud + offset wherever the kernel loaded it (user=True)."""
    def __init__(self, lo, hi, user=False, sid=0):
        self.lo, self.hi, self.user, self.sid = lo, hi, user, sid
        self.pre = "s%d_" % sid if user else ""
        self.jt, self.tk = ("jt_s%d" % sid, "tk_s%d" % sid) if user else ("jt", "tk")

    def lab(self, kind, v): return "%s%s%x" % (self.pre, kind, v)


S = None          # the segment being translated


def in_text(pc): return S.lo <= pc < S.hi


def pcv(o, seg, v, dest):
    """dest = guest pc v of seg."""
    if seg.user: o.ex(ADD, "ud", o.c(v), dest)
    else: o.copy(o.c(v), dest)


def target(o, t):
    """The label for guest pc t: its translation, or a stub that hands it to the interpreter."""
    if in_text(t) and t % 4 == 0: return S.lab("p", t)
    lab = S.lab("ext", t)
    if lab not in o.labels and lab not in o.ext_wanted:
        o.ext_wanted.add(lab)
        def stub(lab=lab, t=t, seg=S): o.L(lab); pcv(o, seg, t, "ipc"); o.jump("to_interp")
        o.later.append(stub)
    return lab


def t_halt(o, pc, seg=None):
    """End the slice; the next one starts at guest pc's translation."""
    seg = seg or S
    o.copy(o.u(seg.lab("p", pc)), "resume"); pcv(o, seg, pc, "rpc"); o.jump("exit")


def to_interp_at(o, pc):
    """A stub that hands guest pc to the interpreter, with the registers as they are."""
    lab = S.lab("i", pc)
    if lab not in o.istubs:
        o.istubs.add(lab)
        seg = S
        o.later.append(lambda: (o.L(lab), pcv(o, seg, pc, "ipc"), o.jump("to_interp")))
    return lab


PAUSE = 0x0100000F
PURE = ("lui", "auipc", "opimm", "op")


def leaders_of(ds, data=()):
    """Where a block starts: only these are entered from outside it (others through the interpreter)."""
    TEXT, TEND = S.lo, S.hi
    lead = {TEXT}
    for i, d in enumerate(ds):
        pc, k = TEXT + 4 * i, d["k"]
        if k in ("branch", "jal"):
            t = (pc + d["imm"]) & MASK
            if in_text(t) and t % 4 == 0: lead.add(t)
        if k in ("branch", "jal", "jalr", "interp") or (k == "fence" and d["w"] == PAUSE): lead.add(pc + 4)
        if k == "interp": lead.add(pc)
        if k == "jalr" and d["rd"] == 0 and d["rs1"] != 1:      # a computed jump: into a run (Duff's device)
            for j in range(i + 1, min(i + 257, len(ds))):
                lead.add(TEXT + 4 * j)
                if ds[j]["k"] in ("branch", "jal", "jalr"): break
    lead |= {a for a in data if TEXT <= a < TEND and a % 4 == 0}   # jump tables, function pointers
    entries = os.environ.get("L2U_ENTRIES")                    # entry points a reference run saw
    if entries and not S.user:
        seen = struct.unpack("<%di" % (os.path.getsize(entries) // 4), open(entries, "rb").read())
        lead |= {TEXT + 4 * i for i, c in enumerate(seen) if c and i < len(ds)}
    return lead


def fold(d, pc, get):
    """The value a pure instruction writes when its inputs are known, else None."""
    k, f3 = d["k"], d["f3"]
    if k == "lui": return d["imm"] & MASK
    if k == "auipc": return None if S.user else (pc + d["imm"]) & MASK
    a = get(d["rs1"])
    if a is None: return None
    if k == "opimm": b, alt = d["imm"] & MASK, f3 == 5 and (d["w"] >> 30) & 1
    else:
        if d["f7"] == 1: return None
        b = get(d["rs2"])
        if b is None: return None
        alt = d["f7"] == 0x20
    sa, sb, s = sx(a, 32), sx(b, 32), b & 31
    r = {0: a - b if k == "op" and alt else a + b, 1: a << s, 2: int(sa < sb), 3: int(a < b), 4: a ^ b,
         5: sa >> s if alt else a >> s, 6: a | b, 7: a & b}[f3]
    return r & MASK


def reads_of(d):
    k = d["k"]
    if k in ("opimm", "load", "jalr"): return (d["rs1"],)
    if k == "op": return (d["rs1"], d["rs2"])
    if k in ("branch", "store", "amo"): return (d["rs1"], d["rs2"])
    return ()


def writes_of(d):
    return d["rd"] if d["k"] in ("lui", "auipc", "jal", "jalr", "opimm", "op", "load", "amo") else 0


def emit_text(o, ds, checked, stats):
    lead = o.leaders
    starts = sorted({i for i in ((pc - S.lo) // 4 for pc in lead) if 0 <= i < len(ds)})
    tk = [0] * len(ds)
    for bi, i0 in enumerate(starts):
        i1 = starts[bi + 1] if bi + 1 < len(starts) else len(ds)
        stats["blocks"] += 1
        emit_block(o, ds, i0, i1, checked, stats, tk)
    o.L(S.pre + "text_end")
    pcv(o, S, S.hi, "ipc"); o.jump("to_interp")
    return tk, lead


def emit_block(o, ds, i0, i1, checked, stats, tk):
    pc0 = S.lo + 4 * i0
    o.L(S.lab("p", pc0))
    o.pcpos[i0] = len(o.code)
    if ds[i0]["k"] == "interp":
        stats["interp"] += i1 - i0
        pcv(o, S, pc0, "ipc"); o.jump("to_interp")
        return
    tk[i0] = 1
    # forward: known constants and registers known to point into RAM
    known, ram, neg, items, have_o = {}, set(), set(), [], set()
    for i in range(i0, i1):
        d, pc = dict(ds[i]), S.lo + 4 * i
        get = lambda r: 0 if r == 0 else known.get(r)
        k, rd = d["k"], writes_of(ds[i])
        it = dict(d=d, pc=pc, form=k, reads=set(r for r in reads_of(d) if r and get(r) is None))
        d["A"] = x(d["rs1"]) if get(d["rs1"]) is None else o.c(get(d["rs1"]))
        d["B"] = x(d["rs2"]) if get(d["rs2"]) is None else o.c(get(d["rs2"]))
        d["KA"], d["KB"] = get(d["rs1"]), get(d["rs2"])
        if k == "amo": it["reads"] = set(r for r in reads_of(d) if r)
        v = fold(d, pc, get) if k in PURE else None
        if v is not None:
            it["form"], it["value"], it["reads"] = "const", v, set()
            stats["folded"] += 1
        if k in ("load", "store"):
            base = get(d["rs1"])
            a = None if base is None else (base + d["imm"]) & MASK
            if k == "load" and not rd: it["mode"] = "none"
            elif a is not None and RAM <= a < RAM + RAMSIZE - 4:
                it["mode"], it["addr"], it["reads"] = "const", a, it["reads"] - {d["rs1"]}
                stats["const_mem"] += 1
            else:
                r = d["rs1"]
                it["new_o"] = r not in have_o; have_o.add(r)
                it["mode"] = "check" if r not in ram and r not in (2, 3, 4) else "free"
                stats["checked_mem" if it["mode"] == "check" else "unchecked"] += 1
                if it["mode"] == "check" and d["imm"] <= 0: neg.add(r)    # r itself is at least RAM's start
                ram.add(r)
        if k == "branch": it["neg"] = all(r in neg or r in (2, 3, 4) for r in (d["rs1"], d["rs2"]))
        if rd:
            if it["form"] == "const": known[rd] = it["value"]
            else: known.pop(rd, None)
            if k == "opimm" and d["f3"] == 0 and d["imm"] == 0 and (d["rs1"] in ram or d["rs1"] in (2, 3, 4)): ram.add(rd)
            else: ram.discard(rd)
            if k == "opimm" and d["f3"] == 0 and d["imm"] == 0 and (d["rs1"] in neg or d["rs1"] in (2, 3, 4)): neg.add(rd)
            else: neg.discard(rd)
            have_o.discard(rd)
        items.append(it)
    # backward: a pure write nothing in the block reads before it is written again is left out
    live = set(range(1, 32))
    for it in reversed(items):
        rd = writes_of(it["d"])
        if it["form"] in PURE + ("const",) and rd and rd not in live:
            it["dead"] = True; stats["dead"] += 1
            continue
        if rd: live.discard(rd)
        live |= it["reads"]
        if it.get("mode") == "check": live = set(range(1, 32))   # a failed check hands over every register
    if COUNT: o.ex(ADD, "gcount", o.c(i1 - i0), "gcount")
    if pc0 in checked:
        o.ex(SUB, "budget", o.c(1), "budget"); o.ex(GT, "budget", "x0", "cnd"); o.jif("cnd", halt_label(o, pc0))
    o.boff, o.bhave = {}, set()
    for it in items:
        o.pcpos[(it["pc"] - S.lo) // 4] = max(o.pcpos[(it["pc"] - S.lo) // 4], len(o.code)) if it["pc"] != pc0 else o.pcpos[i0]
        stats["translated"] += 1
        w = writes_of(it["d"])
        if w: o.bhave.discard(w)        # o_r is invalidated by new_o in the forward pass's order
        if it.get("dead"): continue
        if it["form"] == "const":
            if it["d"]["rd"]: o.copy(o.c(it["value"]), x(it["d"]["rd"]))
            continue
        t_insn(o, it)


def t_insn(o, it):
    d, pc = it["d"], it["pc"]
    k, rd, rs1, rs2, f3, f7 = d["k"], d["rd"], d["rs1"], d["rs2"], d["f3"], d["f7"]
    imm = d.get("imm", 0)
    if k == "jal":
        if rd: pcv(o, S, pc + 4, x(rd))
        o.jump(target(o, (pc + imm) & MASK))
    elif k == "jalr":
        if d["KA"] is not None and not S.user:      # a known target: a direct jump
            t = (d["KA"] + imm) & MASK & ~1
            if rd: o.copy(o.c(pc + 4), x(rd))
            if t <= pc and t in o.leaders:
                o.ex(SUB, "budget", o.c(1), "budget"); o.ex(GT, "budget", "x0", "cnd"); o.jif("cnd", halt_label(o, t))
            o.jump(target(o, t) if t in o.leaders else to_interp_at(o, t))
            return
        jt_index(o, x(rs1), imm)
        if rd: pcv(o, S, pc + 4, x(rd))
        o.ex(UGET, S.jt, "idx", "tgt")
        if not (rd == 0 and rs1 == 1 and imm == 0):    # a return cannot make a loop by itself
            o.ex(SUB, "budget", o.c(1), "budget"); o.ex(GT, "budget", "x0", "cnd2")
            o.jif("cnd2", S.pre + "jalr_halt")
        o.jind("tgt")
    elif k == "branch": t_branch(o, d, target(o, (pc + imm) & MASK), it.get("neg", False))
    elif k == "opimm":
        if rd: t_opimm(o, d)
    elif k == "op":
        if not rd: return
        if f7 == 1: emit_mul(o, f3, d["A"], d["B"], x(rd))
        else: t_op(o, d)
    elif k == "auipc": o.ex(ADD, "ud", o.c(pc + imm), x(rd))    # a program's: never folded
    elif k == "lui": pass                           # always folded
    elif k in ("load", "store"): t_mem(o, it)
    elif k == "fence":
        if d["w"] == PAUSE:                         # the pause hint ends the slice
            o.copy(o.c(3), "status"); t_halt(o, pc + 4)
    elif k == "amo": t_amo(o, d)


def jt_index(o, base, imm, seg=None):
    """idx = 1 + (base + imm - lo) / 4 for a target in the text, else 0 or n + 1 (u kept for those)."""
    seg = seg or S
    if seg.user: o.ex(SUB, base, "ud", "u"); o.ex(ADD, "u", o.c(imm - seg.lo + 4), "u")
    else: o.ex(ADD, base, o.c(imm - seg.lo + 4), "u")
    o.ex(SHR, "u", o.c(2), "idx")
    o.ex(CLAMP, "idx", "x0", o.c((seg.hi - seg.lo) // 4 + 1), "idx")


def t_branch(o, d, tgt, neg=False):
    f3, ka, kb = d["f3"], d["KA"], d["KB"]
    if ka is not None and kb is not None:
        a, b = (ka, kb) if f3 >= 6 else (sx(ka, 32), sx(kb, 32))
        if {0: a == b, 1: a != b, 4: a < b, 5: a >= b, 6: a < b, 7: a >= b}[f3]: o.jump(tgt)
        return
    a, b = d["A"], d["B"]
    if f3 in (6, 7) and neg: f3 -= 2                 # both in RAM: the same order signed
    if f3 in (6, 7):
        if kb == 0:
            if f3 == 7: o.jump(tgt)
            return
        if ka == 0:
            o.ex(EQ if f3 == 6 else NE, b, "x0", "cnd"); o.jif("cnd", tgt); return
        if ka is not None: a = o.c(ka ^ MIN)
        else: o.ex(XOR, a, o.c(MIN), "t"); a = "t"
        if kb is not None: b = o.c(kb ^ MIN)
        else: o.ex(XOR, b, o.c(MIN), "t2"); b = "t2"
    inv = {0: NE, 1: EQ, 4: GE, 5: LT, 6: GE, 7: LT}[f3]      # the opposite: jif jumps on false
    o.ex(inv, a, b, "cnd"); o.jif("cnd", tgt)


def halt_label(o, t):
    lab = S.lab("h", t)
    if lab not in o.hstubs:
        o.hstubs.add(lab)
        seg = S
        o.later.append(lambda: (o.L(lab), t_halt(o, t, seg)))
    return lab


GETB = "SystemBuffer.__GetByte__SystemArray_SystemInt32__SystemByte"
SETB = "SystemBuffer.__SetByte__SystemArray_SystemInt32_SystemByte__SystemVoid"
TOB = "SystemConvert.__ToByte__SystemInt32__SystemByte"
BTOI = "SystemConvert.__ToInt32__SystemByte__SystemInt32"


def t_mem(o, it):
    """A load or store: at a known address, or at a register's, checked to be RAM once a block."""
    d, mode = it["d"], it["mode"]
    k, f3, rd = d["k"], d["f3"], d["rd"]
    if mode == "none": return
    if mode == "const":
        off = it["addr"] - RAM
        t, idx = o.c(off), o.c(off >> 2)
        if f3 in (1, 5) or (k == "store" and f3 == 1): o.copy(t, "t")
    else:
        r, imm = d["rs1"], d["imm"]
        orr, brr = "o%d" % r, "b%d" % r
        if it["new_o"]:                         # o_r: the first access's byte offset in RAM, b_r its word
            o.boff[r] = imm; o.bhave.discard(r)
            o.ex(ADD, d["A"], o.c(imm + MIN), orr)
            if mode == "check":
                o.ex(GE, orr, "x0", "cnd"); o.jif("cnd", to_interp_at(o, it["pc"]))
                if S.user: o.ex(LT, orr, o.c(RAMSIZE - 4096), "cnd"); o.jif("cnd", to_interp_at(o, it["pc"]))
        delta = imm - o.boff[r]
        if f3 == 2 and delta % 4 == 0:
            if r not in o.bhave: o.ex(SHR, orr, o.c(2), brr); o.bhave.add(r)
            idx, t = brr, None
            if delta: o.ex(ADD, brr, o.c(delta >> 2), "idx"); idx = "idx"
        else:
            t, idx = orr, "idx"
            if delta: o.ex(ADD, orr, o.c(delta), "t"); t = "t"
            if f3 in (1, 2, 5):
                if t != "t": o.copy(t, "t"); t = "t"
                o.ex(SHR, "t", o.c(2), "idx")
    if k == "load":
        r = x(rd)
        if f3 == 2: o.ex(AGET, "mem", idx, r)
        elif f3 in (0, 4):
            o.ex(GETB, "mem", t, "bt"); o.ex(BTOI, "bt", r)
            if f3 == 0: o.ex(SHL, r, o.c(24), r); o.ex(SHR, r, o.c(24), r)
        else:
            o.ex(AGET, "mem", idx, "w"); emit_extract(o, f3, r)
        return
    v = d["B"]
    if f3 == 2: o.ex(ASET, "mem", idx, v)
    elif f3 == 0:
        o.ex(AND, v, o.c(255), "t2"); o.ex(TOB, "t2", "bt"); o.ex(SETB, "mem", t, "bt")
    else:
        if idx != "idx": o.copy(idx, "idx")
        o.ex(AGET, "mem", "idx", "w"); emit_insert(o, 16, v)


def t_opimm(o, d):
    rd, rs1, imm, f3 = x(d["rd"]), d["A"], d["imm"], d["f3"]
    sh = imm & 31
    if f3 == 0:
        if imm == 0: o.copy(rs1, rd)
        elif d["rs1"] == 0: o.copy(o.c(imm), rd)
        else: o.ex(ADD, rs1, o.c(imm), rd)
    elif f3 == 1: o.ex(SHL, rs1, o.c(sh), rd)
    elif f3 == 5:
        if d["w"] >> 30 & 1: o.ex(SHR, rs1, o.c(sh), rd)
        elif sh == 0: o.copy(rs1, rd)
        else: o.ex(SHR, rs1, o.c(sh), "t"); o.ex(AND, "t", o.c(MASK >> sh), rd)
    elif f3 == 2: o.ex(LT, rs1, o.c(imm), "cnd"); o.ex(B2I, "cnd", rd)
    elif f3 == 3:
        if imm == 1: o.ex(EQ, rs1, "x0", "cnd")
        else: o.ex(XOR, rs1, o.c(MIN), "t"); o.ex(LT, "t", o.c((imm & MASK) ^ MIN), "cnd")
        o.ex(B2I, "cnd", rd)
    else: o.ex({4: XOR, 6: OR, 7: AND}[f3], rs1, o.c(imm), rd)


def t_op(o, d):
    rd, a, b, f3, f7 = x(d["rd"]), d["A"], d["B"], d["f3"], d["f7"]
    if f3 == 0: o.ex(SUB if f7 == 0x20 else ADD, a, b, rd)
    elif f3 == 1: o.ex(SHL, a, b, rd)
    elif f3 == 2: o.ex(LT, a, b, "cnd"); o.ex(B2I, "cnd", rd)
    elif f3 == 3:
        if d["rs1"] == 0: o.ex(NE, b, "x0", "cnd")
        else: o.ex(XOR, a, o.c(MIN), "t"); o.ex(XOR, b, o.c(MIN), "t2"); o.ex(LT, "t", "t2", "cnd")
        o.ex(B2I, "cnd", rd)
    elif f3 == 4: o.ex(XOR, a, b, rd)
    elif f3 == 5:
        if f7 == 0x20: o.ex(SHR, a, b, rd)
        else: emit_srl(o, a, b, rd)
    elif f3 == 6: o.ex(OR, a, b, rd)
    else: o.ex(AND, a, b, rd)


def t_amo(o, d):
    f5, rd, rs1, v = d["f5"], d["rd"], d["rs1"], d["B"]
    o.ex(ADD, x(rs1), o.c(MIN), "t"); o.ex(SHR, "t", o.c(2), "idx")
    if f5 == 2:                                     # lr.w
        o.copy(x(rs1), "res")
        if rd: o.ex(AGET, "mem", "idx", x(rd))
        return
    if f5 == 3:                                     # sc.w
        fail, done = o.new(), o.new()
        o.ex(EQ, "res", x(rs1), "cnd"); o.copy(o.c(MASK), "res"); o.jif("cnd", fail)
        o.ex(ASET, "mem", "idx", v)
        if rd: o.copy("x0", x(rd))
        o.jump(done); o.L(fail)
        if rd: o.copy(o.c(1), x(rd))
        o.L(done); return
    o.ex(AGET, "mem", "idx", "w")
    amo_value(o, f5, v)
    o.ex(ASET, "mem", "idx", "t3")
    if rd: o.copy("w", x(rd))


def amo_value(o, f5, v):
    """t3 = what an AMO stores, from the old word w and the operand v."""
    ops = {0: ADD, 4: XOR, 8: OR, 12: AND}
    if f5 == 1: o.copy(v, "t3")
    elif f5 in ops: o.ex(ops[f5], "w", v, "t3")
    else:
        keep = o.new()
        a, b = "w", v
        if f5 >= 24: o.ex(XOR, "w", o.c(MIN), "t"); o.ex(XOR, v, o.c(MIN), "t2"); a, b = "t", "t2"
        o.copy("w", "t3")
        o.ex(LT if f5 in (16, 24) else GT, a, b, "cnd"); o.jif("cnd", keep)
        o.jump(keep + "k")
        o.L(keep); o.copy(v, "t3")
        o.L(keep + "k")


# ---------------------------------------------------------------- the machine's routines and the interpreter

def routines(o):
    c = o.c
    o.L("exit"); o.exit()

    o.L("jalr_halt")                                # a jalr ran out of budget: resume at its target
    o.copy(o.u("resume_rpc"), "resume"); o.ex(ADD, "u", c(TEXT - 4), "t"); o.ex(AND, "t", c(~1), "rpc"); o.jump("exit")
    o.L("resume_rpc")
    jt_index(o, "rpc", 0); o.ex(UGET, "jt", "idx", "tgt"); o.jind("tgt")
    o.L("nonleader")                    # inside a block or outside the text: the interpreter takes it
    o.ex(ADD, "u", c(TEXT - 4), "t"); o.ex(AND, "t", c(~1), "ipc"); o.jump("to_interp")

    o.L("to_interp")
    sync_to_xr(o); o.copy(c(0), "mode"); o.ex(ADD, "nint", c(1), "nint"); o.jump("iloop")
    o.L("to_trans")
    sync_from_xr(o); o.copy(c(1), "mode"); o.jind("tgt")

    # mload: the word at t + 2^31 (t < 0: below RAM) into w; the ROM, the clock, else 0
    o.L("mload")
    o.ex(ADD, "t", c(ROM), "t2"); o.ex(GE, "t2", "x0", "cnd"); o.jif("cnd", "ml_low")
    o.ex(SHR, "t2", c(2), "t2"); o.ex(LT, "t2", "romlen", "cnd"); o.jif("cnd", "ml_zero")
    o.ex(AGET, "rom", "t2", "w"); o.jind("mret")
    o.L("ml_low")
    o.ex(AND, "t", c(~3), "t2")
    o.ex(EQ, "t2", c(0x030007F8 + MIN), "cnd"); o.jif("cnd", "ml_r1"); o.copy("rtc0", "w"); o.jind("mret")
    o.L("ml_r1")
    o.ex(EQ, "t2", c(0x030007FC + MIN), "cnd"); o.jif("cnd", "ml_zero"); o.copy("rtc1", "w"); o.jind("mret")
    o.L("ml_zero"); o.copy("x0", "w"); o.jind("mret")

    # gread: the word at ga into w, t = ga + 2^31 for its low bits
    o.L("gread")
    o.ex(ADD, "ga", c(MIN), "t"); o.ex(GE, "t", "x0", "cnd"); o.jif("cnd", "mload")
    o.ex(LT, "t", c(RAMSIZE), "cnd"); o.jif("cnd", "ml_zero")
    o.ex(SHR, "t", c(2), "idx"); o.ex(AGET, "mem", "idx", "w"); o.jind("mret")

    # slice entry: the timer, then an interrupt if one is pending and enabled, else resume
    o.L("entry")
    o.ex(EQ, "th", "x0", "cnd"); o.jif("cnd", "e_irq")
    o.ex(GT, "tl", "x0", "cnd"); o.jif("cnd", "e_irq")
    o.ex(LE, "tl", "now", "cnd"); o.jif("cnd", "e_irq")
    o.ex(AGET, "csr", c(12), "t"); o.ex(OR, "t", c(0x20), "t"); o.ex(ASET, "csr", c(12), "t")
    o.L("e_irq")
    irq_pending(o, "e_none")
    o.ex(EQ, "mode", c(1), "cnd"); o.jif("cnd", "irq_take")
    sync_to_xr(o); o.copy("rpc", "ipc"); o.copy(c(0), "mode"); o.jump("irq_take")
    o.L("e_none"); o.jind("resume")

    # after a system instruction: an interrupt now enabled is taken at ipc
    o.L("irq_look")
    irq_pending(o, "ichk")
    o.L("irq_take")
    o.ex(AND, "ip", c(0x200), "t"); o.ex(EQ, "t", "x0", "cnd"); o.jif("cnd", "irq_9")
    o.ex(AND, "ip", c(2), "t"); o.ex(EQ, "t", "x0", "cnd"); o.jif("cnd", "irq_1")
    o.copy(c(0x80000005), "tcause"); o.jump("irq_go")
    for bit in (9, 1):
        o.L("irq_%d" % bit); o.copy(c(0x80000000 | bit), "tcause")
        o.ex(AGET, "csr", c(12), "t"); o.ex(AND, "t", c(~(1 << bit)), "t"); o.ex(ASET, "csr", c(12), "t")
        o.jump("irq_go")
    o.L("irq_go"); o.copy("x0", "tval"); o.ex(ADD, "nirq", c(1), "nirq")

    # take_trap: tcause, tval, epc = ipc; to stvec in supervisor mode
    o.L("take_trap")
    o.ex(ASET, "csr", c(9), "ipc"); o.ex(ASET, "csr", c(10), "tcause"); o.ex(ASET, "csr", c(11), "tval")
    o.ex(AGET, "csr", c(5), "t"); o.ex(AND, "t", c(~3), "t2"); o.ex(AND, "t", c(3), "t3")
    o.ex(EQ, "t3", "x0", "cnd"); o.jif("cnd", "tt_v"); o.copy("t2", "ipc"); o.jump("tt_st")
    o.L("tt_v")
    o.ex(LT, "tcause", "x0", "cnd"); o.jif("cnd", "tt_d")
    o.ex(AND, "tcause", c(31), "t3"); o.ex(SHL, "t3", c(2), "t3"); o.ex(ADD, "t2", "t3", "ipc"); o.jump("tt_st")
    o.L("tt_d"); o.copy("t2", "ipc")
    o.L("tt_st")
    o.ex(AGET, "csr", c(0), "t"); o.ex(AND, "t", c(~0x122), "t2")
    o.ex(AND, "t", c(2), "t3"); o.ex(SHL, "t3", c(4), "t3"); o.ex(OR, "t2", "t3", "t2")
    o.ex(AND, "priv", c(1), "t3"); o.ex(SHL, "t3", c(8), "t3"); o.ex(OR, "t2", "t3", "t2")
    o.ex(ASET, "csr", c(0), "t2"); o.copy(c(1), "priv"); o.ex(ADD, "ntrap", c(1), "ntrap")
    o.jump("ichk")


def seg_routines(o, seg):
    """A program's jalr out of budget, its resume and its pads, as the kernel's in routines()."""
    c, pre = o.c, seg.pre
    o.L(pre + "jalr_halt")
    o.copy(o.u(pre + "resume_rpc"), "resume")
    o.ex(ADD, "u", c(seg.lo - 4), "t"); o.ex(ADD, "t", "ud", "t"); o.ex(AND, "t", c(~1), "rpc"); o.jump("exit")
    o.L(pre + "resume_rpc")
    jt_index(o, "rpc", 0, seg); o.ex(UGET, seg.jt, "idx", "tgt"); o.jind("tgt")
    o.L(pre + "nonleader")
    o.ex(ADD, "u", c(seg.lo - 4), "t"); o.ex(ADD, "t", "ud", "t"); o.ex(AND, "t", c(~1), "ipc"); o.jump("to_interp")
    # the interpreter at a pc of this program: back to its translation where a block starts
    o.L(pre + "ichk")
    jt_index(o, "ipc", 0, seg)
    o.ex(AGET, seg.tk, "idx", "t"); o.ex(GT, "t", "x0", "cnd"); o.jif("cnd", "iloop")
    o.ex(UGET, seg.jt, "idx", "tgt"); o.jump("to_trans")


def ubind(o, segs):
    """At sret to user mode, pc inext: which translated program runs there and at what base (useg,
    ud). A binding seen before is looked up and checked against four words of its text; a program
    at its entry point is recognised by the same four words and remembered (bb, bs)."""
    c = o.c
    o.L("ubind")
    o.copy(c(0), "bi")
    o.L("ub_loop")
    o.ex(LT, "bi", c(16), "cnd"); o.jif("cnd", "ub_disc")
    o.ex(AGET, "bs", "bi", "bseg"); o.ex(ADD, "bi", c(1), "bi")
    o.ex(LT, "bseg", "x0", "cnd"); o.jif("cnd", "ub_has"); o.jump("ub_loop")
    o.L("ub_has")
    o.ex(SUB, "bi", c(1), "t"); o.ex(AGET, "bb", "t", "bbase")
    o.ex(SUB, "inext", "bbase", "brel")
    o.ex(AGET, "sglo", "bseg", "t"); o.ex(LT, "brel", "t", "cnd"); o.jif("cnd", "ub_lo"); o.jump("ub_loop")
    o.L("ub_lo")
    o.ex(AGET, "sghi", "bseg", "t"); o.ex(LT, "brel", "t", "cnd"); o.jif("cnd", "ub_loop")
    o.ex(SHL, "bseg", c(2), "bk")
    for k in range(4):
        o.ex(AGET, "sgoff", "bk", "t"); o.ex(ADD, "bbase", "t", "t"); ub_word(o, "ub_loop")
        o.ex(AGET, "sgw", "bk", "t"); o.ex(EQ, "w", "t", "cnd"); o.jif("cnd", "ub_loop")
        if k < 3: o.ex(ADD, "bk", c(1), "bk")
    o.copy("bseg", "useg"); o.copy("bbase", "ud"); o.jump("ilook")
    o.L("ub_disc")
    for sg in segs:
        nxt = o.new()
        o.ex(SUB, "inext", c(sg.entry), "bbase")
        for off, word in sg.sig:
            o.ex(ADD, "bbase", c(off), "t"); ub_word(o, nxt)
            o.ex(EQ, "w", c(word), "cnd"); o.jif("cnd", nxt)
        o.ex(ASET, "bb", "rr", "bbase"); o.ex(ASET, "bs", "rr", c(sg.sid))
        o.ex(ADD, "rr", c(1), "rr"); o.ex(AND, "rr", c(15), "rr")
        o.copy(c(sg.sid), "useg"); o.copy("bbase", "ud"); o.jump("ilook")
        o.L(nxt)
    o.copy(c(MASK), "useg"); o.jump("ilook")


def ub_word(o, fail):
    """w = the RAM word at address t, else to fail."""
    o.ex(ADD, "t", o.c(MIN), "t"); o.ex(GE, "t", "x0", "cnd"); o.jif("cnd", fail)
    o.ex(LT, "t", o.c(RAMSIZE - 4), "cnd"); o.jif("cnd", fail)
    o.ex(SHR, "t", o.c(2), "t"); o.ex(AGET, "mem", "t", "w")


def irq_pending(o, none):
    """ip = the pending enabled interrupts; to `none` when there is none to take."""
    c = o.c
    o.ex(AGET, "csr", c(12), "t"); o.ex(AGET, "csr", c(4), "t2"); o.ex(AND, "t", "t2", "t")
    o.ex(AND, "t", c(0x222), "ip"); o.ex(EQ, "ip", "x0", "cnd")
    some = o.new(); o.jif("cnd", some); o.jump(none)
    o.L(some)
    o.ex(EQ, "priv", c(1), "cnd"); take = o.new(); o.jif("cnd", take)
    o.ex(AGET, "csr", c(0), "t"); o.ex(AND, "t", c(2), "t"); o.ex(EQ, "t", "x0", "cnd"); o.jif("cnd", take)
    o.jump(none)
    o.L(take)


def interpreter(o, tables):
    c = o.c

    def table(name, n, default):
        tables[name] = [default] * n
        return tables[name]

    # one instruction: budget, fetch, decode, dispatch on the opcode
    o.L("iloop")
    o.ex(SUB, "budget", c(1), "budget"); o.ex(GT, "budget", "x0", "cnd"); o.jif("cnd", "ihalt")
    o.ex(ADD, "icount", c(1), "icount")
    o.copy("ipc", "ga"); o.copy(o.u("ifetched"), "mret"); o.jump("gread")
    o.L("ifetched")
    o.copy("w", "iw")
    o.ex(AND, "iw", c(127), "iop")
    o.ex(SHR, "iw", c(7), "t"); o.ex(AND, "t", c(31), "ird")
    o.ex(SHR, "iw", c(12), "t"); o.ex(AND, "t", c(7), "if3")
    o.ex(SHR, "iw", c(15), "t"); o.ex(AND, "t", c(31), "irs1")
    o.ex(SHR, "iw", c(20), "t"); o.ex(AND, "t", c(31), "irs2")
    o.ex(AGET, "xr", "irs1", "ia"); o.ex(AGET, "xr", "irs2", "ib")
    o.ex(ADD, "ipc", c(4), "inext")
    o.ex(UGET, "ijt", "iop", "tgt"); o.jind("tgt")
    ijt = table("ijt", 128, "illegal")

    o.L("ihalt"); o.copy(o.u("iloop"), "resume"); o.copy("ipc", "rpc"); o.jump("exit")

    # write-back, then back to translated code when the pc is supervisor text that has one
    o.L("iwb"); o.ex(ASET, "xr", "ird", "iv"); o.ex(ASET, "xr", "x0", "x0")
    o.L("inoreg"); o.copy("inext", "ipc")
    o.L("ichk")
    o.ex(EQ, "priv", c(1), "cnd"); o.jif("cnd", "ichk_u")
    jt_index(o, "ipc", 0)
    o.ex(AGET, "tk", "idx", "t"); o.ex(GT, "t", "x0", "cnd"); o.jif("cnd", "iloop")
    o.ex(UGET, "jt", "idx", "tgt"); o.jump("to_trans")

    o.L("ichk_u")
    o.ex(LT, "useg", "x0", "cnd"); o.jif("cnd", "ichk_u2"); o.jump("iloop")
    o.L("ichk_u2"); o.ex(UGET, "usjt", "useg", "tgt"); o.jind("tgt")

    o.L("ilook"); o.copy("inext", "ipc"); o.jump("irq_look")

    o.L("illegal"); o.copy(c(2), "tcause"); o.copy("iw", "tval"); o.jump("take_trap")

    o.L("lui"); o.ex(AND, "iw", c(0xFFFFF000), "iv"); o.jump("iwb")
    o.L("auipc"); o.ex(AND, "iw", c(0xFFFFF000), "t"); o.ex(ADD, "t", "ipc", "iv"); o.jump("iwb")
    ijt[0x37], ijt[0x17] = "lui", "auipc"

    o.L("jal")
    o.copy("inext", "iv")
    o.ex(SHR, "iw", c(11), "t"); o.ex(AND, "t", c(0xFFF00000), "t")
    o.ex(AND, "iw", c(0xFF000), "t2"); o.ex(OR, "t", "t2", "t")
    o.ex(SHR, "iw", c(9), "t2"); o.ex(AND, "t2", c(0x800), "t2"); o.ex(OR, "t", "t2", "t")
    o.ex(SHR, "iw", c(20), "t2"); o.ex(AND, "t2", c(0x7FE), "t2"); o.ex(OR, "t", "t2", "t")
    o.ex(ADD, "ipc", "t", "inext"); o.jump("iwb")
    o.L("jalr")
    o.copy("inext", "iv"); o.ex(SHR, "iw", c(20), "t"); o.ex(ADD, "ia", "t", "t"); o.ex(AND, "t", c(~1), "inext")
    o.jump("iwb")
    ijt[0x6F], ijt[0x67] = "jal", "jalr"

    # branches: taken computes the offset
    o.L("branch"); o.ex(UGET, "bjt", "if3", "tgt"); o.jind("tgt")
    ijt[0x63] = "branch"
    bjt = table("bjt", 8, "illegal")
    for f3, (cmp, uns) in {0: (EQ, 0), 1: (NE, 0), 4: (LT, 0), 5: (GE, 0), 6: (LT, 1), 7: (GE, 1)}.items():
        o.L("b%d" % f3); bjt[f3] = "b%d" % f3
        a, b = "ia", "ib"
        if uns: o.ex(XOR, "ia", c(MIN), "t"); o.ex(XOR, "ib", c(MIN), "t2"); a, b = "t", "t2"
        o.ex(cmp, a, b, "cnd"); o.jif("cnd", "inoreg"); o.jump("btaken")
    o.L("btaken")
    o.ex(SHR, "iw", c(19), "t"); o.ex(AND, "t", c(0xFFFFF000), "t")
    o.ex(SHL, "iw", c(4), "t2"); o.ex(AND, "t2", c(0x800), "t2"); o.ex(OR, "t", "t2", "t")
    o.ex(SHR, "iw", c(20), "t2"); o.ex(AND, "t2", c(0x7E0), "t2"); o.ex(OR, "t", "t2", "t")
    o.ex(SHR, "iw", c(7), "t2"); o.ex(AND, "t2", c(0x1E), "t2"); o.ex(OR, "t", "t2", "t")
    o.ex(ADD, "ipc", "t", "inext"); o.jump("inoreg")

    # loads through gread (aligned: a word load at a word, a half at a half)
    o.L("load"); o.ex(SHR, "iw", c(20), "t3"); o.ex(ADD, "ia", "t3", "ga")
    o.copy(o.u("load2"), "mret"); o.jump("gread")
    o.L("load2"); o.ex(UGET, "ljt", "if3", "tgt"); o.jind("tgt")
    ijt[0x03] = "load"
    ljt = table("ljt", 8, "illegal")
    for f3 in (0, 1, 4, 5):
        o.L("l%d" % f3); ljt[f3] = "l%d" % f3; emit_extract(o, f3, "iv"); o.jump("iwb")
    o.L("l2"); ljt[2] = "l2"; o.copy("w", "iv"); o.jump("iwb")
    o.L("flw"); o.ex(SHR, "iw", c(20), "t3"); o.ex(ADD, "ia", "t3", "ga")
    o.copy(o.u("flw2"), "mret"); o.jump("gread")
    o.L("flw2"); o.ex(ASET, "fr", "ird", "w"); o.jump("inoreg")
    ijt[0x07] = "flw"

    # stores: outside RAM they are dropped
    def store_addr():
        o.ex(SHR, "iw", c(20), "t3"); o.ex(AND, "t3", c(~31), "t3"); o.ex(OR, "t3", "ird", "t3")
        o.ex(ADD, "ia", "t3", "ga"); o.ex(ADD, "ga", c(MIN), "t")
        o.ex(GE, "t", "x0", "cnd"); o.jif("cnd", "inoreg")
        o.ex(LT, "t", c(RAMSIZE), "cnd"); o.jif("cnd", "inoreg")
        o.ex(SHR, "t", c(2), "idx")
    o.L("store"); store_addr(); o.ex(UGET, "sjt", "if3", "tgt"); o.jind("tgt")
    ijt[0x23] = "store"
    sjt = table("sjt", 8, "illegal")
    o.L("s2"); sjt[2] = "s2"; o.ex(ASET, "mem", "idx", "ib"); o.jump("inoreg")
    for f3 in (0, 1):
        o.L("s%d" % f3); sjt[f3] = "s%d" % f3
        o.ex(AGET, "mem", "idx", "w"); emit_insert(o, 8 if f3 == 0 else 16, "ib"); o.jump("inoreg")
    o.L("fsw"); store_addr(); o.ex(AGET, "fr", "irs2", "iv"); o.ex(ASET, "mem", "idx", "iv"); o.jump("inoreg")
    ijt[0x27] = "fsw"
    fpu(o, ijt, table)

    # register-immediate
    o.L("opimm"); o.ex(SHR, "iw", c(20), "iimm"); o.ex(UGET, "oijt", "if3", "tgt"); o.jind("tgt")
    ijt[0x13] = "opimm"
    oijt = table("oijt", 8, "illegal")
    for f3, op in {0: ADD, 1: SHL, 4: XOR, 6: OR, 7: AND}.items():
        o.L("oi%d" % f3); oijt[f3] = "oi%d" % f3; o.ex(op, "ia", "iimm", "iv"); o.jump("iwb")
    o.L("oi2"); oijt[2] = "oi2"; o.ex(LT, "ia", "iimm", "cnd"); o.ex(B2I, "cnd", "iv"); o.jump("iwb")
    o.L("oi3"); oijt[3] = "oi3"
    o.ex(XOR, "ia", c(MIN), "t"); o.ex(XOR, "iimm", c(MIN), "t2"); o.ex(LT, "t", "t2", "cnd"); o.ex(B2I, "cnd", "iv")
    o.jump("iwb")
    o.L("oi5"); oijt[5] = "oi5"
    o.ex(AND, "iw", c(0x40000000), "t"); o.ex(EQ, "t", "x0", "cnd"); o.jif("cnd", "oi5a")
    emit_srl(o, "ia", "iimm", "iv"); o.jump("iwb")
    o.L("oi5a"); o.ex(SHR, "ia", "iimm", "iv"); o.jump("iwb")

    # register-register: key = f7 bit 0 (mul) at 8, f7 bit 5 (sub, sra) at 16, and f3
    o.L("op")
    o.ex(SHR, "iw", c(22), "t"); o.ex(AND, "t", c(8), "t")
    o.ex(SHR, "iw", c(26), "t2"); o.ex(AND, "t2", c(16), "t2"); o.ex(OR, "t", "t2", "t"); o.ex(OR, "t", "if3", "t")
    o.ex(UGET, "rjt", "t", "tgt"); o.jind("tgt")
    ijt[0x33] = "op"
    rjt = table("rjt", 32, "illegal")
    for f3, op in {0: ADD, 1: SHL, 4: XOR, 6: OR, 7: AND}.items():
        o.L("r%d" % f3); rjt[f3] = "r%d" % f3; o.ex(op, "ia", "ib", "iv"); o.jump("iwb")
    o.L("r2"); rjt[2] = "r2"; o.ex(LT, "ia", "ib", "cnd"); o.ex(B2I, "cnd", "iv"); o.jump("iwb")
    o.L("r3"); rjt[3] = "r3"
    o.ex(XOR, "ia", c(MIN), "t"); o.ex(XOR, "ib", c(MIN), "t2"); o.ex(LT, "t", "t2", "cnd"); o.ex(B2I, "cnd", "iv")
    o.jump("iwb")
    o.L("r5"); rjt[5] = "r5"; emit_srl(o, "ia", "ib", "iv"); o.jump("iwb")
    o.L("r16"); rjt[16] = "r16"; o.ex(SUB, "ia", "ib", "iv"); o.jump("iwb")
    o.L("r21"); rjt[21] = "r21"; o.ex(SHR, "ia", "ib", "iv"); o.jump("iwb")
    for f3 in range(8):
        o.L("m%d" % f3); rjt[8 + f3] = "m%d" % f3; emit_mul(o, f3, "ia", "ib", "iv"); o.jump("iwb")

    # atomics (RAM only)
    o.L("amo")
    o.ex(ADD, "ia", c(MIN), "t"); o.ex(SHR, "t", c(2), "idx"); o.ex(AGET, "mem", "idx", "w")
    o.ex(SHR, "iw", c(27), "t2"); o.ex(AND, "t2", c(31), "t2"); o.ex(UGET, "ajt", "t2", "tgt"); o.jind("tgt")
    ijt[0x2F] = "amo"
    ajt = table("ajt", 32, "illegal")
    o.L("a2"); ajt[2] = "a2"; o.copy("w", "iv"); o.copy("ia", "res"); o.jump("iwb")
    o.L("a3"); ajt[3] = "a3"
    o.ex(EQ, "res", "ia", "cnd"); o.copy(c(MASK), "res"); o.jif("cnd", "a3f")
    o.ex(ASET, "mem", "idx", "ib"); o.copy("x0", "iv"); o.jump("iwb")
    o.L("a3f"); o.copy(c(1), "iv"); o.jump("iwb")
    for f5 in (0, 1, 4, 8, 12, 16, 20, 24, 28):
        o.L("a%d" % f5); ajt[f5] = "a%d" % f5
        amo_value(o, f5, "ib"); o.ex(ASET, "mem", "idx", "t3"); o.copy("w", "iv"); o.jump("iwb")

    # fence; the pause hint ends the slice
    o.L("fence"); o.ex(EQ, "iw", c(0x0100000F), "cnd"); o.jif("cnd", "inoreg")
    o.copy("inext", "ipc"); o.copy(c(3), "status"); o.copy(o.u("iloop"), "resume"); o.copy("ipc", "rpc"); o.jump("exit")
    ijt[0x0F] = "fence"

    system(o, tables)
    ijt[0x73] = "system"


def fpu(o, ijt, table):
    """The F extension as the machine has it (docs/machine/fpu.md): registers as bits in fr, an
    operation on floats from BitConverter; multiply-add rounds its product; no flags."""
    c = o.c
    o.L("fp")
    o.ex(AGET, "fr", "irs1", "fia"); o.ex(AGET, "fr", "irs2", "fib")
    o.ex(BITS2F, "fia", "fa"); o.ex(BITS2F, "fib", "fb")
    o.ex(SHR, "iw", c(27), "t"); o.ex(AND, "t", c(31), "t"); o.ex(UGET, "fjt", "t", "tgt"); o.jind("tgt")
    ijt[0x53] = "fp"
    fjt = table("fjt", 32, "illegal")
    for f5, op in {0: "Addition", 1: "Subtraction", 2: "Multiplication", 3: "Division"}.items():
        o.L("f%d" % f5); fjt[f5] = "f%d" % f5; o.ex(FOP % op, "fa", "fb", "fv"); o.jump("fwbv")
    o.L("f11"); fjt[11] = "f11"; o.ex(FMATH % "Sqrt", "fa", "fv")
    o.L("fwbv"); o.ex(F2BITS, "fv", "iv")
    o.L("fwb"); o.ex(ASET, "fr", "ird", "iv"); o.jump("inoreg")

    # sign injection: f3 0 b's sign, 1 its opposite, 2 a's sign xor b's
    o.L("f4"); fjt[4] = "f4"
    o.ex(AND, "fib", c(MIN), "t2")
    l1, lj = o.new(), o.new()
    o.ex(EQ, "if3", c(1), "cnd"); o.jif("cnd", l1); o.ex(XOR, "t2", c(MIN), "t2"); o.L(l1)
    o.ex(EQ, "if3", c(2), "cnd"); o.jif("cnd", lj); o.ex(XOR, "fia", "t2", "iv"); o.jump("fwb")
    o.L(lj); o.ex(AND, "fia", c(0x7FFFFFFF), "t"); o.ex(OR, "t", "t2", "iv"); o.jump("fwb")

    # min (f3 0), max (1): a NaN loses to a number, -0 is less than +0
    o.L("f5"); fjt[5] = "f5"
    an, bn, ne, pb = o.new(), o.new(), o.new(), o.new()
    o.ex(AND, "fia", c(0x7FFFFFFF), "t"); o.ex(GT, "t", c(0x7F800000), "cnd"); o.jif("cnd", an)
    o.copy("fib", "iv"); o.jump("fwb")
    o.L(an); o.ex(AND, "fib", c(0x7FFFFFFF), "t"); o.ex(GT, "t", c(0x7F800000), "cnd"); o.jif("cnd", bn)
    o.copy("fia", "iv"); o.jump("fwb")
    o.L(bn); o.ex(FCMP % "Equality", "fa", "fb", "cnd"); o.jif("cnd", ne)
    mx = o.new()
    o.ex(EQ, "if3", "x0", "cnd"); o.jif("cnd", mx); o.ex(OR, "fia", "fib", "iv"); o.jump("fwb")
    o.L(mx); o.ex(AND, "fia", "fib", "iv"); o.jump("fwb")
    o.L(ne)
    o.ex(FCMP % "LessThan", "fa", "fb", "cnd"); o.ex(B2I, "cnd", "t")
    o.ex(EQ, "if3", "x0", "cnd"); o.ex(B2I, "cnd", "t2")
    o.ex(EQ, "t", "t2", "cnd"); o.jif("cnd", pb); o.copy("fia", "iv"); o.jump("fwb")
    o.L(pb); o.copy("fib", "iv"); o.jump("fwb")

    # feq (f3 2), flt (1), fle (0): false with a NaN
    o.L("f20"); fjt[20] = "f20"
    l1, l2 = o.new(), o.new()
    o.ex(EQ, "if3", c(2), "cnd"); o.jif("cnd", l1); o.ex(FCMP % "Equality", "fa", "fb", "cnd"); o.ex(B2I, "cnd", "iv"); o.jump("iwb")
    o.L(l1); o.ex(EQ, "if3", c(1), "cnd"); o.jif("cnd", l2); o.ex(FCMP % "LessThan", "fa", "fb", "cnd"); o.ex(B2I, "cnd", "iv"); o.jump("iwb")
    o.L(l2); o.ex(FCMP % "LessThanOrEqual", "fa", "fb", "cnd"); o.ex(B2I, "cnd", "iv"); o.jump("iwb")

    # fcvt.w.s (rs2 0), fcvt.wu.s (1): rounded as rm (7: frm) says, then clamped to the range
    o.L("f24"); fjt[24] = "f24"
    o.copy("if3", "t")
    l1 = o.new()
    o.ex(EQ, "t", c(7), "cnd"); o.jif("cnd", l1); o.ex(SHR, "fcsr", c(5), "t"); o.ex(AND, "t", c(7), "t"); o.L(l1)
    nan, done = o.new(), o.new()
    o.ex(AND, "fia", c(0x7FFFFFFF), "t2"); o.ex(GT, "t2", c(0x7F800000), "cnd"); o.jif("cnd", nan)
    o.copy(c(MASK), "iv"); o.ex(EQ, "irs2", "x0", "cnd"); o.jif("cnd", "iwb"); o.copy(c(0x7FFFFFFF), "iv"); o.jump("iwb")
    o.L(nan)
    rz, rd, ru, rn = o.new(), o.new(), o.new(), o.new()
    o.ex(EQ, "t", c(1), "cnd"); o.jif("cnd", rz)
    neg = o.new()
    o.ex(FCMP % "LessThan", "fa", "k_f0", "cnd"); o.jif("cnd", neg); o.ex(FMATH % "Ceil", "fa", "fv"); o.jump(done)
    o.L(neg); o.ex(FMATH % "Floor", "fa", "fv"); o.jump(done)
    o.L(rz); o.ex(EQ, "t", c(2), "cnd"); o.jif("cnd", rd); o.ex(FMATH % "Floor", "fa", "fv"); o.jump(done)
    o.L(rd); o.ex(EQ, "t", c(3), "cnd"); o.jif("cnd", ru); o.ex(FMATH % "Ceil", "fa", "fv"); o.jump(done)
    o.L(ru); o.ex(FMATH % "Round", "fa", "fv")
    o.L(done)
    uns, big = o.new(), o.new()
    o.ex(EQ, "irs2", "x0", "cnd"); o.jif("cnd", uns)
    o.copy(c(0x7FFFFFFF), "iv"); o.ex(FCMP % "GreaterThanOrEqual", "fv", "k_f2p31", "cnd"); o.jif("cnd", rn); o.jump("iwb")
    o.L(rn); o.copy(c(MIN), "iv"); o.ex(FCMP % "LessThan", "fv", "k_fm2p31", "cnd"); o.jif("cnd", big); o.jump("iwb")
    o.L(big); o.ex(F2I, "fv", "iv"); o.jump("iwb")
    o.L(uns)
    u1, u2, u3 = o.new(), o.new(), o.new()
    o.copy(c(MASK), "iv"); o.ex(FCMP % "GreaterThanOrEqual", "fv", "k_f2p32", "cnd"); o.jif("cnd", u1); o.jump("iwb")
    o.L(u1); o.copy("x0", "iv"); o.ex(FCMP % "LessThan", "fv", "k_f0", "cnd"); o.jif("cnd", u2); o.jump("iwb")
    o.L(u2); o.ex(FCMP % "GreaterThanOrEqual", "fv", "k_f2p31", "cnd"); o.jif("cnd", u3)
    o.ex(FOP % "Subtraction", "fv", "k_f2p32", "fv")
    o.L(u3); o.ex(F2I, "fv", "iv"); o.jump("iwb")

    # fcvt.s.w (rs2 0), fcvt.s.wu (1)
    o.L("f26"); fjt[26] = "f26"
    l1 = o.new()
    o.ex(EQ, "irs2", "x0", "cnd"); o.jif("cnd", l1); o.ex(I2F, "ia", "fv"); o.jump("fwbv")
    o.L(l1); o.ex(I2L, "ia", "l"); o.ex(I64 % "LogicalAnd", "l", "k_u32", "l"); o.ex(L2F, "l", "fv"); o.jump("fwbv")

    # fmv.x.w (f3 0), fclass.s (1); fmv.w.x
    o.L("f28"); fjt[28] = "f28"
    cl = o.new()
    o.ex(EQ, "if3", "x0", "cnd"); o.jif("cnd", cl); o.copy("fia", "iv"); o.jump("iwb")
    o.L(cl)
    o.ex(SHR, "fia", c(23), "t"); o.ex(AND, "t", c(255), "t"); o.ex(AND, "fia", c(0x7FFFFF), "t2")
    o.ex(LT, "fia", "x0", "cnd"); o.ex(B2I, "cnd", "t3")          # t3: the sign
    other, nanl, qn, normal, subl, put = o.new(), o.new(), o.new(), o.new(), o.new(), o.new()
    o.ex(EQ, "t", c(255), "cnd"); o.jif("cnd", other)
    o.ex(EQ, "t2", "x0", "cnd"); o.jif("cnd", nanl)
    o.ex(MUL, "t3", c(-7), "t"); o.ex(ADD, "t", c(7), "t"); o.jump(put)                    # infinity: 0 or 7
    o.L(nanl); o.ex(AND, "t2", c(0x400000), "t2"); o.copy(c(8), "t")
    o.ex(EQ, "t2", "x0", "cnd"); o.jif("cnd", qn); o.jump(put)
    o.L(qn); o.copy(c(9), "t"); o.jump(put)
    o.L(other); o.ex(EQ, "t", "x0", "cnd"); o.jif("cnd", normal)
    o.ex(EQ, "t2", "x0", "cnd"); o.jif("cnd", subl); o.ex(SUB, c(4), "t3", "t"); o.jump(put)  # zero: 3 or 4
    o.L(subl); o.ex(MUL, "t3", c(-3), "t"); o.ex(ADD, "t", c(5), "t"); o.jump(put)           # 2 or 5
    o.L(normal); o.ex(MUL, "t3", c(-5), "t"); o.ex(ADD, "t", c(6), "t")                       # 1 or 6
    o.L(put); o.ex(SHL, c(1), "t", "iv"); o.jump("iwb")
    o.L("f30"); fjt[30] = "f30"; o.copy("ia", "iv"); o.jump("fwb")

    # multiply-add: the product rounded, then rs3 added (fmadd, fmsub, fnmsub, fnmadd)
    for op, kind in ((0x43, 0), (0x47, 1), (0x4B, 2), (0x4F, 3)):
        lab = "fma%d" % kind
        o.L(lab); ijt[op] = lab
        o.ex(AGET, "fr", "irs1", "t"); o.ex(BITS2F, "t", "fa")
        o.ex(AGET, "fr", "irs2", "t"); o.ex(BITS2F, "t", "fb")
        o.ex(SHR, "iw", c(27), "t"); o.ex(AND, "t", c(31), "t"); o.ex(AGET, "fr", "t", "t"); o.ex(BITS2F, "t", "fc")
        o.ex(FOP % "Multiplication", "fa", "fb", "fa")
        if kind == 0: o.ex(FOP % "Addition", "fa", "fc", "fv")
        elif kind == 1: o.ex(FOP % "Subtraction", "fa", "fc", "fv")
        elif kind == 2: o.ex(FOP % "Subtraction", "fc", "fa", "fv")
        else: o.ex(FOP % "Addition", "fa", "fc", "fv"); o.ex(FNEG, "fv", "fv")
        o.jump("fwbv")


def system(o, tables):
    c = o.c
    o.L("system")
    o.ex(EQ, "if3", "x0", "cnd"); o.jif("cnd", "csrop")
    o.ex(ADD, "nsys", c(1), "nsys")
    for w, lab in ((0x00000073, "ecall"), (0x00100073, "ebreak"), (0x10200073, "sret"), (0x10500073, "wfi")):
        o.ex(EQ, "iw", c(w), "cnd"); nxt = o.new(); o.jif("cnd", nxt); o.jump(lab); o.L(nxt)
    o.ex(SHR, "iw", c(25), "t"); o.ex(EQ, "t", c(9), "cnd"); o.jif("cnd", "illegal"); o.jump("ilook")   # sfence.vma

    o.L("ecall"); o.ex(EQ, "priv", c(1), "cnd"); o.jif("cnd", "ecall_u"); o.jump("sbi")
    o.L("ecall_u"); o.copy(c(8), "tcause"); o.copy("x0", "tval"); o.jump("take_trap")
    o.L("ebreak"); o.copy(c(3), "tcause"); o.copy("ipc", "tval"); o.jump("take_trap")
    o.L("sret")
    o.ex(AGET, "csr", c(9), "inext")
    o.ex(AGET, "csr", c(0), "t"); o.ex(SHR, "t", c(8), "t2"); o.ex(AND, "t2", c(1), "priv")
    o.ex(AND, "t", c(~0x122), "t2"); o.ex(SHR, "t", c(4), "t3"); o.ex(AND, "t3", c(2), "t3"); o.ex(OR, "t2", "t3", "t2")
    o.ex(OR, "t2", c(0x20), "t2"); o.ex(ASET, "csr", c(0), "t2")
    o.ex(EQ, "priv", "x0", "cnd"); o.jif("cnd", "ilook"); o.jump("ubind")
    o.L("wfi"); o.copy("inext", "ipc"); o.copy(c(2), "status"); o.copy(o.u("iloop"), "resume"); o.copy("ipc", "rpc")
    o.jump("exit")

    # SBI: the timer and the base extension, as the machine answers them
    o.L("sbi")
    o.ex(AGET, "xr", c(17), "sext"); o.ex(AGET, "xr", c(16), "sfn"); o.ex(AGET, "xr", c(10), "sa0")
    o.ex(EQ, "sext", c(0x54494D45), "cnd"); o.jif("cnd", "sbi_t0"); o.jump("sbi_time")
    o.L("sbi_t0"); o.ex(EQ, "sext", "x0", "cnd"); o.jif("cnd", "sbi_val")
    o.L("sbi_time")
    o.copy("sa0", "tl"); o.ex(AGET, "xr", c(11), "th")
    o.ex(AGET, "csr", c(12), "t"); o.ex(AND, "t", c(~0x20), "t")
    o.ex(EQ, "th", "x0", "cnd"); o.jif("cnd", "sbi_ts")
    o.ex(GT, "tl", "x0", "cnd"); o.jif("cnd", "sbi_ts")
    o.ex(LE, "tl", "now", "cnd"); o.jif("cnd", "sbi_ts")
    o.ex(OR, "t", c(0x20), "t")
    o.L("sbi_ts"); o.ex(ASET, "csr", c(12), "t")
    o.L("sbi_val")
    o.copy("x0", "sval")
    o.ex(EQ, "sext", c(0x10), "cnd"); o.jif("cnd", "sbi_ret")
    for fn, v in ((0, 2), (1, 0x454D55), (2, 1)):
        o.ex(EQ, "sfn", c(fn), "cnd"); nxt = o.new(); o.jif("cnd", nxt); o.copy(c(v), "sval"); o.jump("sbi_ret"); o.L(nxt)
    o.ex(EQ, "sfn", c(3), "cnd"); o.jif("cnd", "sbi_ret")
    o.ex(EQ, "sa0", c(0x54494D45), "cnd"); o.jif("cnd", "sbi_p2"); o.copy(c(1), "sval"); o.jump("sbi_ret")
    o.L("sbi_p2")
    o.ex(XOR, "sa0", c(MIN), "t"); o.ex(LE, "t", c(2 ^ MIN), "cnd"); o.jif("cnd", "sbi_ret"); o.copy(c(1), "sval")
    o.L("sbi_ret")
    o.ex(XOR, "sext", c(MIN), "t"); o.ex(LT, "t", c(0x10 ^ MIN), "cnd"); o.jif("cnd", "sbi_new")
    o.ex(ASET, "xr", c(10), "sval"); o.jump("ilook")
    o.L("sbi_new")
    o.copy(c(0xFFFFFFFE), "t2")
    o.ex(EQ, "sext", c(0x10), "cnd"); nxt = o.new(); o.jif("cnd", nxt); o.copy("x0", "t2"); o.L(nxt)
    o.ex(EQ, "sext", c(0x54494D45), "cnd"); nxt = o.new(); o.jif("cnd", nxt); o.copy("x0", "t2"); o.L(nxt)
    o.ex(ASET, "xr", c(10), "t2"); o.ex(ASET, "xr", c(11), "sval"); o.jump("ilook")

    # CSRs through the tables: slot, mask, a read special and a write special for each number
    o.L("csrop")
    o.ex(ADD, "ncsr", c(1), "ncsr")
    o.ex(SHR, "iw", c(20), "t"); o.ex(AND, "t", c(0xFFF), "icsr")
    o.ex(AGET, "cslot", "icsr", "islot"); o.ex(AGET, "cmask", "icsr", "imask")
    o.ex(AGET, "csr", "islot", "t"); o.ex(AND, "t", "imask", "iv")
    o.ex(AND, "if3", c(4), "t"); o.ex(EQ, "t", "x0", "cnd"); o.jif("cnd", "csr_imm")
    o.copy("ia", "isrc"); o.jump("csr_src")
    o.L("csr_imm"); o.copy("irs1", "isrc")
    o.L("csr_src"); o.ex(UGET, "csrr", "icsr", "tgt"); o.jind("tgt")
    o.L("csr_rd")
    o.ex(AND, "if3", c(3), "t"); o.ex(EQ, "t", c(1), "cnd"); o.jif("cnd", "csr_sc")
    o.copy("isrc", "inv"); o.jump("csr_write")
    o.L("csr_sc")
    o.ex(EQ, "irs1", "x0", "cnd"); o.jif("cnd", "csr_sc2"); o.jump("csr_done")
    o.L("csr_sc2")
    o.ex(EQ, "t", c(2), "cnd"); o.jif("cnd", "csr_clr"); o.ex(OR, "iv", "isrc", "inv"); o.jump("csr_write")
    o.L("csr_clr"); o.ex(XOR, "isrc", c(MASK), "t2"); o.ex(AND, "iv", "t2", "inv")
    o.L("csr_write")
    o.ex(AGET, "csr", "islot", "t"); o.ex(XOR, "imask", c(MASK), "t2"); o.ex(AND, "t", "t2", "t")
    o.ex(AND, "inv", "imask", "t2"); o.ex(OR, "t", "t2", "t"); o.ex(ASET, "csr", "islot", "t")
    o.ex(UGET, "csrw", "icsr", "tgt"); o.jind("tgt")
    o.L("csr_done"); o.ex(ASET, "xr", "ird", "iv"); o.ex(ASET, "xr", "x0", "x0"); o.jump("ilook")

    csrr, csrw, cslot, cmask = ["csr_rd"] * 4096, ["csr_done"] * 4096, [0] * 4096, [0] * 4096
    for n in range(4096):
        s = (n >> 3 & 8) | (n & 7)
        cslot[n], cmask[n] = s, sx(csr_mask(n), 32)
        if s == 0: csrr[n] = "r_sd"
    o.L("r_sd")
    o.ex(AND, "iv", c(0x6000), "t"); o.ex(EQ, "t", "x0", "cnd"); o.jif("cnd", "r_sd1"); o.jump("csr_rd")
    o.L("r_sd1"); o.ex(OR, "iv", c(0x80006000), "iv"); o.jump("csr_rd")
    reads = {1: lambda: o.ex(AND, "fcsr", c(31), "iv"), 2: lambda: o.ex(SHR, "fcsr", c(5), "iv"),
             3: lambda: o.copy("fcsr", "iv"), 0x301: lambda: o.copy(c(0x40141101), "iv"),
             0x0BF: lambda: (o.ex(MUL, "ipc", c(2654435761), "t"), o.ex(XOR, "t", c(0x5EED1234), "iv")),
             0xC01: lambda: o.copy("now", "iv"), 0xC81: lambda: o.copy("x0", "iv"),
             0xC00: lambda: o.ex(ADD, "gcount", "icount", "iv"), 0xC02: lambda: o.ex(ADD, "gcount", "icount", "iv"),
             0xC80: lambda: o.copy("x0", "iv"), 0xC82: lambda: o.copy("x0", "iv")}
    for n, f in reads.items():
        o.L("r_%x" % n); csrr[n] = "r_%x" % n; f(); o.jump("csr_rd")
    writes = {1: lambda: (o.ex(AND, "fcsr", c(0xE0), "t"), o.ex(AND, "inv", c(0x1F), "t2"), o.ex(OR, "t", "t2", "fcsr")),
              2: lambda: (o.ex(AND, "fcsr", c(0x1F), "t"), o.ex(AND, "inv", c(7), "t2"), o.ex(SHL, "t2", c(5), "t2"),
                          o.ex(OR, "t", "t2", "fcsr")),
              3: lambda: o.ex(AND, "inv", c(0xFF), "fcsr"),
              0x0B1: lambda: o.copy("inv", "msrc"), 0x0B2: lambda: o.copy("inv", "mdst"), 0x0B3: lambda: o.copy("inv", "mn")}
    for n, f in writes.items():
        o.L("w_%x" % n); csrw[n] = "w_%x" % n; f(); o.jump("csr_done")
    csrw[0x0B0] = "memop"
    tables.update(csrr=csrr, csrw=csrw, cslot=cslot, cmask=cmask)
    memop(o)


def memop(o):
    """The copy and fill CSR: Array.Copy from RAM or the ROM, Array.Clear, else word by word."""
    c = o.c
    o.L("memop")
    o.ex(ADD, "nmemop", c(1), "nmemop")
    o.ex(ADD, "mn", c(3), "t"); o.ex(SHR, "t", c(2), "icnt"); o.ex(GT, "icnt", "x0", "cnd"); o.jif("cnd", "csr_done")
    o.ex(SHL, "icnt", c(2), "ibytes")
    o.ex(ADD, "mdst", c(MIN), "dt"); o.ex(GE, "dt", "x0", "cnd"); o.jif("cnd", "mo_slow")
    o.ex(ADD, "dt", "ibytes", "t2"); o.ex(LE, "t2", c(RAMSIZE), "cnd"); o.jif("cnd", "mo_slow")
    o.ex(SHR, "dt", c(2), "didx")
    o.ex(EQ, "inv", c(2), "cnd"); o.jif("cnd", "mo_copy")
    o.ex(EQ, "msrc", "x0", "cnd"); o.jif("cnd", "mo_slow")
    o.ex(ACLEAR, "mem", "didx", "icnt"); o.jump("csr_done")
    o.L("mo_copy")
    o.ex(ADD, "msrc", c(MIN), "st"); o.ex(GE, "st", "x0", "cnd"); o.jif("cnd", "mo_rom")
    o.ex(ADD, "st", "ibytes", "t2"); o.ex(LE, "t2", c(RAMSIZE), "cnd"); o.jif("cnd", "mo_slow")
    o.ex(SHR, "st", c(2), "sidx"); o.ex(ACOPY, "mem", "sidx", "mem", "didx", "icnt"); o.jump("csr_done")
    o.L("mo_rom")
    o.ex(SUB, "msrc", c(ROM), "st"); o.ex(GE, "st", "x0", "cnd"); o.jif("cnd", "mo_slow")
    o.ex(SHR, "st", c(2), "sidx"); o.ex(ADD, "sidx", "icnt", "t2"); o.ex(LE, "t2", "romlen", "cnd"); o.jif("cnd", "mo_slow")
    o.ex(ACOPY, "rom", "sidx", "mem", "didx", "icnt"); o.jump("csr_done")
    o.L("mo_slow")
    o.ex(ADD, "nmoslow", c(1), "nmoslow")
    o.copy("x0", "ii")
    o.L("mo_loop")
    o.ex(LT, "ii", "ibytes", "cnd"); o.jif("cnd", "csr_done")
    o.copy("msrc", "w"); o.ex(EQ, "inv", c(2), "cnd"); o.jif("cnd", "mo_rd"); o.jump("mo_wr")
    o.L("mo_rd"); o.ex(ADD, "msrc", "ii", "ga"); o.copy(o.u("mo_wr"), "mret"); o.jump("gread")
    o.L("mo_wr")
    o.ex(ADD, "mdst", "ii", "t"); o.ex(ADD, "t", c(MIN), "t")
    o.ex(GE, "t", "x0", "cnd"); o.jif("cnd", "mo_next"); o.ex(LT, "t", c(RAMSIZE), "cnd"); o.jif("cnd", "mo_next")
    o.ex(SHR, "t", c(2), "idx"); o.ex(ASET, "mem", "idx", "w")
    o.L("mo_next"); o.ex(ADD, "ii", c(4), "ii"); o.jump("mo_loop")


def csr_mask(n):
    s = (n >> 3 & 8) | (n & 7)
    if (n & ~0x47) == 0x100:
        return 0xDE162 if s == 0 else 0x222 if (s & 7) == 4 else MASK if (0xF60 >> s) & 1 else 0
    return MASK if n in (0x300, 0x304, 0x344) else 0


# ---------------------------------------------------------------- build

def romfs_files(rom):
    """{path: bytes} of a romfs image."""
    assert rom[:8] == b"-rom1fs-"
    def name_at(off):
        e = rom.index(b"\0", off)
        return rom[off:e].decode("latin1"), (e + 16) & ~15
    out = {}
    def walk(off, path):
        while off:
            nxt, spec, size, _ = struct.unpack(">4I", rom[off:off + 16])
            name, data = name_at(off + 16)
            if nxt & 7 == 1 and name not in (".", ".."): walk(spec, path + "/" + name)
            elif nxt & 7 == 2: out[path + "/" + name] = rom[data:data + size]
            off = nxt & ~15
    walk(name_at(16)[1], "")
    return out


def flat_segment(b, sid):
    """A FLAT binary's text as a Seg at its file offsets, with its entry, the targets its
    relocations give (leaders) and four words that tell it from another program."""
    entry, ds, reloc, nrel = (struct.unpack(">I", b[off:off + 4])[0] for off in (8, 12, 32, 36))
    sg = Seg(0x40, ds, True, sid)
    sg.entry = entry
    sg.words = struct.unpack_from("<%dI" % ((ds - 0x40) // 4), b, 0x40)
    rel = struct.unpack_from(">%dI" % nrel, b, reloc)
    sg.data = {struct.unpack_from(">I", b, 0x40 + r)[0] + 0x40 for r in rel if 0x40 + r + 4 <= len(b)}
    n = len(sg.words)
    sg.sig = [(0x40 + 4 * (n * k // 5), sg.words[n * k // 5]) for k in range(1, 5)]
    return sg


def main():
    global S
    payload, out_dir = sys.argv[1], sys.argv[2]
    t0 = time.time()
    img = open(payload, "rb").read()
    n = (TEND - TEXT) // 4
    words = struct.unpack_from("<%dI" % n, img, TEXT - RAM)

    o = Asm()
    o.ext_wanted, o.istubs, o.hstubs = set(), set(), set()
    o.pcpos = [0] * n
    o.var("x0", T_I)
    for r in range(1, 32): o.var(x(r), T_I)
    for r in range(32): o.var("o%d" % r, T_I); o.var("b%d" % r, T_I)
    for v in ("t", "t2", "t3", "u", "idx", "w", "sh", "budget", "status", "rpc", "ipc", "mode", "priv", "res",
              "now", "th", "tl", "fcsr", "msrc", "mdst", "mn", "romlen", "ga", "ip", "tcause", "tval",
              "iw", "iop", "ird", "irs1", "irs2", "if3", "ia", "ib", "iv", "inext", "iimm",
              "icsr", "islot", "imask", "isrc", "inv", "icnt", "ibytes", "dt", "st", "didx", "sidx", "ii",
              "sext", "sfn", "sa0", "sval", "icount", "gcount", "nint", "nirq", "ntrap", "nsys", "ncsr", "nmemop", "nmoslow"):
        o.var(v, T_I)
    for v in ("ud", "bi", "bseg", "bbase", "brel", "bk", "rr"): o.var(v, T_I)
    o.var("useg", T_I, -1)
    for v in ("cnd", "cnd2"): o.var(v, T_B)
    o.var("bt", T_BY)
    for v in ("tgt", "resume", "mret", "resume0"): o.var(v, T_U)
    for v in ("l", "l2", "l3", "k_u32", "k_l0"): o.var(v, T_L)
    for v in ("fia", "fib"): o.var(v, T_I)
    o.var("rtc0", T_I, RTC0); o.var("rtc1", T_I, RTC1)       # the clock chip: the host sets them
    for v in ("fa", "fb", "fc", "fv"): o.var(v, T_F)
    for v, bits in (("k_f0", 0), ("k_f2p31", 0x4F000000), ("k_fm2p31", sx(0xCF000000, 32)), ("k_f2p32", 0x4F800000)):
        o.var(v, T_F, bits)
    for v in ("mem", "rom", "xr", "fr", "csr", "tk", "cslot", "cmask"): o.var(v, T_IA)
    for v in ("jt", "ijt", "bjt", "ljt", "sjt", "oijt", "rjt", "ajt", "csrr", "csrw", "usjt", "fjt"): o.var(v, T_UA)
    for v in ("bb", "bs", "sglo", "sghi", "sgoff", "sgw"): o.var(v, T_IA)

    kseg = Seg(TEXT, TEND)
    kseg.words = words
    kseg.data = set(struct.unpack_from("<%dI" % ((len(img) - (TEND - RAM)) // 4), img, TEND - RAM))
    segs = [kseg]
    if len(sys.argv) > 4:
        files = romfs_files(open(sys.argv[3], "rb").read())
        for i, name in enumerate(sys.argv[4:]):
            segs.append(flat_segment(files[name], i))
            segs[-1].name = name
    users = segs[1:]
    for v in users: o.var(v.jt, T_UA); o.var(v.tk, T_IA)

    tables = {}
    S = kseg
    routines(o)
    interpreter(o, tables)
    ubind(o, users)
    stats = dict(translated=0, interp=0, unchecked=0, checked_mem=0, const_mem=0, folded=0, dead=0, blocks=0)
    checks = 0
    for sg in segs:
        S = sg
        if sg.user: seg_routines(o, sg)
        ds = [decode(w) for w in sg.words]
        o.leaders = leaders_of(ds, sg.data)
        checked = set()
        for i, d in enumerate(ds):
            pc = sg.lo + 4 * i
            if d["k"] == "branch" or (d["k"] == "jal" and d["rd"] == 0):    # a call cannot loop by itself
                t = (pc + d["imm"]) & MASK
                if t <= pc and in_text(t): checked.add(t)
        checks += len(checked)
        o.pcpos = [0] * len(ds)
        sg.tkv, sg.lead = emit_text(o, ds, checked, stats)
        sg.pcpos = o.pcpos
        while o.later:
            later, o.later = o.later, []
            for f in later: f()
    t1 = time.time()
    o.finish()
    o.heap[o.slots["resume0"]][1] = o.labels["p%x" % TEXT]
    for sg in segs:
        pad = o.labels[sg.pre + "nonleader"]
        nw = len(sg.words)
        sg.jtv = [pad] + [o.labels[sg.lab("p", sg.lo + 4 * i)] if sg.lo + 4 * i in sg.lead else pad for i in range(nw)] + [pad]
        sg.tkv = [0] + sg.tkv + [0]
    jt, tk, n = kseg.jtv, kseg.tkv, len(words)
    o.pcpos = kseg.pcpos
    tables["usjt"] = [sg.pre + "ichk" for sg in users] or ["iloop"]

    os.makedirs(out_dir, exist_ok=True)
    def path(name): return os.path.join(out_dir, name)
    open(path("prog.bc"), "wb").write(o.code)
    hb = bytearray(struct.pack("<I", len(o.heap)))
    for typ, val in o.heap: hb += struct.pack("<Bq", typ, val)
    open(path("prog.heap"), "wb").write(hb)
    open(path("prog.externs"), "w").write("\n".join(o.externs) + "\n")
    with open(path("prog.syms"), "w") as f:
        for name, slot in o.slots.items():
            if name[0] != "#": f.write("%s %d\n" % (name, slot))
        f.write("@entry %d\n@resume0 %d\n" % (o.labels["entry"], o.labels["p%x" % TEXT]))
        tsum = 0                      # every 61st text word, as EmuUdonCpu sums the image it loads
        for i in range(0, n, 61): tsum = (tsum * 31 + words[i]) & MASK
        f.write("@textwords %d\n@textsum %d\n" % (n, sx(tsum, 32)))
    open(path("jt.u32"), "wb").write(struct.pack("<%dI" % len(jt), *jt))
    open(path("pcpos.bin"), "wb").write(struct.pack("<%dI" % n, *o.pcpos))
    open(path("tk.i32"), "wb").write(struct.pack("<%di" % len(tk), *tk))
    for sg in users:
        open(path(sg.jt + ".u32"), "wb").write(struct.pack("<%dI" % len(sg.jtv), *sg.jtv))
        open(path(sg.tk + ".i32"), "wb").write(struct.pack("<%di" % len(sg.tkv), *sg.tkv))
    ints = dict(bb=[0] * 16, bs=[-1] * 16, sglo=[sg.lo for sg in users] or [0], sghi=[sg.hi for sg in users] or [0],
                sgoff=[off for sg in users for off, w in sg.sig] or [0], sgw=[sx(w, 32) for sg in users for off, w in sg.sig] or [0])
    for name, vals in ints.items(): open(path(name + ".i32"), "wb").write(struct.pack("<%di" % len(vals), *vals))
    for name, vals in tables.items():
        if name in ("cslot", "cmask"): open(path(name + ".i32"), "wb").write(struct.pack("<%di" % len(vals), *vals))
        else: open(path(name + ".u32"), "wb").write(struct.pack("<%dI" % len(vals), *(o.labels[v] for v in vals)))
    for sg in users: print("%s: %d words at file offsets %x to %x, entry %x" % (sg.name, len(sg.words), sg.lo, sg.hi, sg.entry))
    print("%d words of text in %d blocks: %d translated, %d to the interpreter; %d loop checks"
          % (sum(len(sg.words) for sg in segs), stats["blocks"], stats["translated"], stats["interp"], checks))
    print("loads and stores: %d checked, %d after a check or by sp/gp/tp, %d at known addresses; %d folded to constants, %d dead writes"
          % (stats["checked_mem"], stats["unchecked"], stats["const_mem"], stats["folded"], stats["dead"]))
    print("%d bytes of Udon code, %d heap slots, %d externs; translated in %.1f s, written in %.1f s"
          % (len(o.code), len(o.heap), len(o.externs), t1 - t0, time.time() - t1))


if __name__ == "__main__":
    main()
