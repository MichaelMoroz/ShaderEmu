"""RV32IMA bare-metal image -> Udon assembly, translated once at build time (README.md).

python rv2udon.py IMAGE.bin IMAGE.elf OUT_DIR
writes OUT_DIR/prog.uasm and OUT_DIR/prog.json (the jump table and what the runner needs).
"""
import json, os, struct, sys

RAM = 0x80000000
MIN = 0x80000000
MASK = 0xFFFFFFFF
SIZES = {"NOP": 4, "POP": 4, "COPY": 4}

UART_THR, UART_LSR, MTIME = 0x10000000, 0x10000005, 0x0200BFF8
CALLER_SAVED = {1, 5, 6, 7, 10, 11, 12, 13, 14, 15, 16, 17, 28, 29, 30, 31}

I32 = "SystemInt32.__op_%s__SystemInt32_SystemInt32__SystemInt32"
CMP = "SystemInt32.__op_%s__SystemInt32_SystemInt32__SystemBoolean"
I64 = "SystemInt64.__op_%s__SystemInt64_SystemInt64__SystemInt64"
I64SH = "SystemInt64.__op_%s__SystemInt64_SystemInt32__SystemInt64"
AGET = "SystemInt32Array.__Get__SystemInt32__SystemInt32"
ASET = "SystemInt32Array.__Set__SystemInt32_SystemInt32__SystemVoid"
UGET = "SystemUInt32Array.__Get__SystemInt32__SystemUInt32"
B2I = "SystemConvert.__ToInt32__SystemBoolean__SystemInt32"
L2I = "SystemConvert.__ToInt32__SystemInt64__SystemInt32"
I2L = "SystemConvert.__ToInt64__SystemInt32__SystemInt64"
BIGMUL = "SystemMath.__BigMul__SystemInt32_SystemInt32__SystemInt64"


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def decode(pc, w):
    op, rd, f3 = w & 0x7F, (w >> 7) & 31, (w >> 12) & 7
    rs1, rs2, f7 = (w >> 15) & 31, (w >> 20) & 31, w >> 25
    i_imm = sx(w >> 20, 12)
    s_imm = sx(((w >> 25) << 5) | ((w >> 7) & 31), 12)
    b_imm = sx(((w >> 31) << 12) | (((w >> 7) & 1) << 11) | (((w >> 25) & 63) << 5) | (((w >> 8) & 15) << 1), 13)
    j_imm = sx(((w >> 31) << 20) | (((w >> 12) & 255) << 12) | (((w >> 20) & 1) << 11) | (((w >> 21) & 1023) << 1), 21)
    d = dict(pc=pc, w=w, rd=rd, rs1=rs1, rs2=rs2, f3=f3, f7=f7)
    if op == 0x37: d.update(k="lui", imm=w & 0xFFFFF000)
    elif op == 0x17: d.update(k="auipc", imm=w & 0xFFFFF000)
    elif op == 0x6F: d.update(k="jal", imm=j_imm)
    elif op == 0x67: d.update(k="jalr", imm=i_imm)
    elif op == 0x63: d.update(k="branch", imm=b_imm)
    elif op == 0x03: d.update(k="load", imm=i_imm)
    elif op == 0x23: d.update(k="store", imm=s_imm)
    elif op == 0x13: d.update(k="opimm", imm=i_imm)
    elif op == 0x33: d.update(k="op")
    elif op == 0x0F: d.update(k="fence")
    elif op == 0x2F: d.update(k="amo", f5=w >> 27)
    elif w == 0x10500073: d.update(k="wfi")
    else: d.update(k="unsupported")
    return d


def text_range(elf):
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
    secs = [struct.unpack_from("<IIIIII", elf, shoff + i * shentsize) for i in range(shnum)]
    stroff = secs[shstrndx][4]
    for name, _, _, addr, _, size in secs:
        if elf[stroff + name:elf.index(b"\0", stroff + name)] == b".text":
            return addr, size
    raise SystemExit("no .text")


def successors(d):
    """Static successors as (pc, kind): kind 'call' marks a call's return point."""
    pc, k = d["pc"], d["k"]
    if k == "jal":
        t = (pc + d["imm"]) & MASK
        return [(t, "jump")] + ([(pc + 4, "call")] if d["rd"] else [])
    if k == "jalr":
        return [(pc + 4, "call")] if d["rd"] else []
    if k == "branch":
        return [((pc + d["imm"]) & MASK, "jump"), (pc + 4, "jump")]
    return [(pc + 4, "jump")]


def const_prop(ins, start):
    """Registers known to hold a constant before each instruction (MMIO detection only)."""
    state = {start: {}}
    work = [start]
    while work:
        pc = work.pop()
        if pc not in ins: continue
        d, s = ins[pc], dict(state[pc])
        rd, k = d["rd"], d["k"]
        if k == "lui": s[rd] = d["imm"]
        elif k == "auipc": s[rd] = (pc + d["imm"]) & MASK
        elif k == "opimm" and d["f3"] == 0 and (d["rs1"] == 0 or d["rs1"] in s):
            s[rd] = ((s.get(d["rs1"], 0)) + d["imm"]) & MASK
        elif k in ("branch", "store", "fence"): pass
        else: s.pop(rd, None)
        s.pop(0, None)
        for t, kind in successors(d):
            ns = {r: v for r, v in s.items() if r not in CALLER_SAVED} if kind == "call" else s
            if t not in state:
                state[t] = dict(ns); work.append(t)
            else:
                old = state[t]
                merged = {r: v for r, v in old.items() if ns.get(r) == v}
                if merged != old:
                    state[t] = merged; work.append(t)
    return state


class Out:
    def __init__(self):
        self.lines, self.pos, self.consts, self.externs, self.ycs = [], 0, {}, set(), set()

    def emit(self, op, arg=None):
        self.lines.append("    %s%s" % (op, ", " + str(arg) if arg is not None else ""))
        self.pos += SIZES.get(op, 8)

    def c(self, v):
        v &= MASK
        name = "c_%08x" % v
        self.consts[name] = v
        return name

    def ex(self, f, *args):
        for a in args: self.emit("PUSH", a)
        self.emit("EXTERN", '"%s"' % f)
        self.externs.add(f)

    def copy(self, src, dst):
        self.emit("PUSH", src); self.emit("PUSH", dst); self.emit("COPY")

    def hole(self, op):
        self.emit(op, "0x00000000")
        return len(self.lines) - 1, op

    def fill(self, h):
        self.lines[h[0]] = "    %s, 0x%08X" % (h[1], self.pos)

    def yc(self, pc):
        """A UInt32 constant: where guest pc's translation starts (resume addresses)."""
        self.ycs.add(pc)
        return "y_%08x" % pc

    def halt(self, pc_after):
        """End the slice; the next one starts at guest pc_after."""
        self.copy(self.yc(pc_after), "resume"); self.emit("JUMP", "0xFFFFFFFC")

    def jump(self, addr): self.emit("JUMP", "0x%08X" % addr)
    def jif(self, addr): self.emit("PUSH", "cnd"); self.emit("JUMP_IF_FALSE", "0x%08X" % addr)


def x(r): return "x%d" % r


def translate(image, text_addr, text_size):
    words = struct.unpack_from("<%dI" % (text_size // 4), image, text_addr - RAM)
    ins = {text_addr + 4 * i: decode(text_addr + 4 * i, w) for i, w in enumerate(words)}
    known = const_prop(ins, text_addr)
    # the budget is counted where a backward branch or jump lands, and in every jalr
    checked = {(d["pc"] + d["imm"]) & MASK for d in ins.values()
               if d["k"] in ("branch", "jal") and ((d["pc"] + d["imm"]) & MASK) <= d["pc"]}
    addr, yields = {}, {}
    for final in (False, True):          # the first pass lays out addresses for the second
        o = Out()
        o.addr, o.yields = addr, yields
        addr_here, ystubs, unsupported = {}, {}, []
        for pc in sorted(ins):
            addr_here[pc] = o.pos
            if pc in checked:
                o.ex(I32 % "Subtraction", "budget", o.c(1), "budget")
                o.ex(CMP % "GreaterThan", "budget", "x0", "cnd")
                o.jif(yields.get(pc, 0))
            emit_insn(o, ins[pc], known.get(pc, {}), lambda t: addr.get(t, 0), unsupported)
        o.halt_at = o.pos
        o.copy(o.c(4), "status"); o.emit("JUMP", "0xFFFFFFFC")       # ran off the text
        for pc in sorted(checked):
            ystubs[pc] = o.pos
            o.halt(pc)
        ystubs["jalr"] = o.pos
        o.copy("tgt", "resume"); o.emit("JUMP", "0xFFFFFFFC")
        if not final:
            addr, yields = addr_here, ystubs
    jt = [addr[text_addr + 4 * i] for i in range(text_size // 4)]
    return o, jt, ins, checked, unsupported

def emit_insn(o, d, known, target, unsupported):
    pc, k, rd, rs1, rs2, f3, f7 = d["pc"], d["k"], d["rd"], d["rs1"], d["rs2"], d["f3"], d["f7"]
    imm = d.get("imm", 0)
    if k == "lui":
        if rd: o.copy(o.c(imm), x(rd))
    elif k == "auipc":
        if rd: o.copy(o.c(pc + imm), x(rd))
    elif k == "jal":
        if rd: o.copy(o.c(pc + 4), x(rd))
        o.jump(target((pc + imm) & MASK))
    elif k == "jalr":
        o.ex(I32 % "Addition", x(rs1), o.c(imm + MIN), "t")
        o.ex(I32 % "RightShift", "t", o.c(2), "idx")
        if rd: o.copy(o.c(pc + 4), x(rd))
        o.ex(I32 % "Subtraction", "budget", o.c(1), "budget")
        o.ex(CMP % "GreaterThan", "budget", "x0", "cnd")
        o.ex(UGET, "jt", "idx", "tgt")
        o.emit("PUSH", "cnd"); o.emit("JUMP_IF_FALSE", "0x%08X" % o.yields.get("jalr", 0))
        o.emit("JUMP_INDIRECT", "tgt")
    elif k == "branch":
        emit_branch(o, d, target((pc + imm) & MASK))
    elif k == "opimm":
        if rd: emit_opimm(o, d)
    elif k == "op":
        if rd: emit_op(o, d)
    elif k == "load":
        emit_load(o, d, known)
    elif k == "store":
        emit_store(o, d, known)
    elif k == "fence":
        pass
    elif k == "amo":
        emit_amo(o, d, unsupported)
    elif k == "wfi":
        pass                      # nothing to wait for here: the loop around it polls
    else:
        unsupported.append(pc)
        o.copy(o.c(pc), "badpc"); o.copy(o.c(3), "status"); o.emit("JUMP", "0xFFFFFFFC")


def emit_branch(o, d, tgt):
    a, b, f3 = x(d["rs1"]), x(d["rs2"]), d["f3"]
    # JUMP_IF_FALSE jumps when the test is false: test the opposite of the branch
    if f3 in (6, 7):              # bltu, bgeu
        if d["rs2"] == 0:
            if f3 == 7: o.jump(tgt)
            return
        if d["rs1"] == 0:
            o.ex(CMP % ("Equality" if f3 == 6 else "Inequality"), b, "x0", "cnd"); o.jif(tgt); return
        o.ex(I32 % "LogicalXor", a, o.c(MIN), "t")
        o.ex(I32 % "LogicalXor", b, o.c(MIN), "t2")
        a, b = "t", "t2"
    inv = {0: "Inequality", 1: "Equality", 4: "GreaterThanOrEqual", 5: "LessThan",
           6: "GreaterThanOrEqual", 7: "LessThan"}[f3]
    o.ex(CMP % inv, a, b, "cnd"); o.jif(tgt)


def emit_opimm(o, d):
    rd, rs1, imm, f3 = x(d["rd"]), x(d["rs1"]), d["imm"], d["f3"]
    sh = imm & 31
    if f3 == 0:
        if imm == 0: o.copy(rs1, rd)
        elif d["rs1"] == 0: o.copy(o.c(imm), rd)
        else: o.ex(I32 % "Addition", rs1, o.c(imm), rd)
    elif f3 == 1: o.ex(I32 % "LeftShift", rs1, o.c(sh), rd)
    elif f3 == 5:
        if d["w"] >> 30 & 1: o.ex(I32 % "RightShift", rs1, o.c(sh), rd)
        elif sh == 0: o.copy(rs1, rd)
        else:
            o.ex(I32 % "RightShift", rs1, o.c(sh), "t")
            o.ex(I32 % "LogicalAnd", "t", o.c(MASK >> sh), rd)
    elif f3 == 2:
        o.ex(CMP % "LessThan", rs1, o.c(imm), "cnd"); o.ex(B2I, "cnd", rd)
    elif f3 == 3:
        if imm == 1: o.ex(CMP % "Equality", rs1, "x0", "cnd")
        else:
            o.ex(I32 % "LogicalXor", rs1, o.c(MIN), "t")
            o.ex(CMP % "LessThan", "t", o.c((imm & MASK) ^ MIN), "cnd")
        o.ex(B2I, "cnd", rd)
    else:
        o.ex(I32 % {4: "LogicalXor", 6: "LogicalOr", 7: "LogicalAnd"}[f3], rs1, o.c(imm), rd)


def emit_op(o, d):
    rd, a, b, f3, f7 = x(d["rd"]), x(d["rs1"]), x(d["rs2"]), d["f3"], d["f7"]
    if f7 == 1:
        emit_mul(o, d); return
    if f3 == 0: o.ex(I32 % ("Subtraction" if f7 == 0x20 else "Addition"), a, b, rd)
    elif f3 == 1: o.ex(I32 % "LeftShift", a, b, rd)
    elif f3 == 2: o.ex(CMP % "LessThan", a, b, "cnd"); o.ex(B2I, "cnd", rd)
    elif f3 == 3:
        if d["rs1"] == 0: o.ex(CMP % "Inequality", b, "x0", "cnd")
        else:
            o.ex(I32 % "LogicalXor", a, o.c(MIN), "t")
            o.ex(I32 % "LogicalXor", b, o.c(MIN), "t2")
            o.ex(CMP % "LessThan", "t", "t2", "cnd")
        o.ex(B2I, "cnd", rd)
    elif f3 == 4: o.ex(I32 % "LogicalXor", a, b, rd)
    elif f3 == 5:
        if f7 == 0x20: o.ex(I32 % "RightShift", a, b, rd)
        else:   # logical: arithmetic shift, then clear the top bits ~((MIN >> s) << 1)
            o.ex(I32 % "RightShift", o.c(MIN), b, "t")
            o.ex(I32 % "LeftShift", "t", o.c(1), "t")
            o.ex(I32 % "LogicalXor", "t", o.c(MASK), "t")
            o.ex(I32 % "RightShift", a, b, "t2")
            o.ex(I32 % "LogicalAnd", "t2", "t", rd)
    elif f3 == 6: o.ex(I32 % "LogicalOr", a, b, rd)
    else: o.ex(I32 % "LogicalAnd", a, b, rd)


def emit_mul(o, d):
    rd, a, b, f3 = x(d["rd"]), x(d["rs1"]), x(d["rs2"]), d["f3"]
    if f3 == 0: o.ex(I32 % "Multiplication", a, b, rd); return
    if f3 <= 3:
        o.ex(BIGMUL, a, b, "l")
        o.ex(I64SH % "RightShift", "l", o.c(32), "l")
        o.ex(L2I, "l", "t3")
        # unsigned high halves from the signed one: + b where a < 0 (mulhu), + a where b < 0
        if f3 == 3:
            o.ex(I32 % "RightShift", a, o.c(31), "t"); o.ex(I32 % "LogicalAnd", "t", b, "t")
            o.ex(I32 % "Addition", "t3", "t", "t3")
        if f3 in (2, 3):
            o.ex(I32 % "RightShift", b, o.c(31), "t"); o.ex(I32 % "LogicalAnd", "t", a, "t")
            o.ex(I32 % "Addition", "t3", "t", "t3")
        o.copy("t3", rd); return
    # div, divu, rem, remu in 64 bits, where MIN / -1 cannot throw; x / 0 apart
    o.ex(I2L, a, "l"); o.ex(I2L, b, "l2")
    if f3 in (5, 7):
        o.ex(I64 % "LogicalAnd", "l", "k_u32", "l"); o.ex(I64 % "LogicalAnd", "l2", "k_u32", "l2")
    o.ex("SystemInt64.__op_Equality__SystemInt64_SystemInt64__SystemBoolean", "l2", "k_l0", "cnd")
    o.emit("PUSH", "cnd"); j1 = o.hole("JUMP_IF_FALSE")
    if f3 < 6: o.copy(o.c(MASK), "t3")
    else: o.copy(a, "t3")
    j2 = o.hole("JUMP")
    o.fill(j1)
    if f3 < 6: o.ex(I64 % "Division", "l", "l2", "l")
    else:                                   # Int64 has no remainder extern: l - l / l2 * l2
        o.ex(I64 % "Division", "l", "l2", "l3")
        o.ex(I64 % "Multiplication", "l3", "l2", "l3")
        o.ex(I64 % "Subtraction", "l", "l3", "l")
    o.ex(I64SH % "LeftShift", "l", o.c(32), "l")
    o.ex(I64SH % "RightShift", "l", o.c(32), "l")
    o.ex(L2I, "l", "t3")
    o.fill(j2)
    o.copy("t3", rd)

def ram_index(o, d, imm):
    o.ex(I32 % "Addition", x(d["rs1"]), o.c(imm + MIN), "t")
    o.ex(I32 % "RightShift", "t", o.c(2), "idx")


def mmio_addr(d, known):
    base = 0 if d["rs1"] == 0 else known.get(d["rs1"])
    if base is None: return None
    a = (base + d["imm"]) & MASK
    return None if a >= RAM else a


def emit_load(o, d, known):
    rd, f3 = d["rd"], d["f3"]
    a = mmio_addr(d, known)
    if a is not None:
        if a == UART_LSR:          # a second poll with no input and no write between: it waits
            o.ex(CMP % "LessThan", "inpos", "inlen", "cnd")
            o.emit("PUSH", "cnd"); nodata = o.hole("JUMP_IF_FALSE")
            if rd: o.copy(o.c(0x21), x(rd))
            done = o.hole("JUMP")
            o.fill(nodata)
            o.ex(CMP % "GreaterThan", "polls", "x0", "cnd")
            o.emit("PUSH", "cnd"); firstpoll = o.hole("JUMP_IF_FALSE")
            o.copy(o.c(5), "status"); o.halt(d["pc"])
            o.fill(firstpoll)
            o.copy(o.c(1), "polls")
            if rd: o.copy(o.c(0x20), x(rd))
            o.fill(done)
        elif a == UART_THR:
            if rd: o.ex(AGET, "inbuf", "inpos", x(rd))
            o.ex(I32 % "Addition", "inpos", o.c(1), "inpos")
        elif a == MTIME:
            if rd: o.copy("mtime", x(rd))
        else:
            o.copy(o.c(d["pc"]), "badpc"); o.copy(o.c(2), "status"); o.emit("JUMP", "0xFFFFFFFC")
        return
    if not rd: return
    ram_index(o, d, d["imm"])
    if f3 == 2:
        o.ex(AGET, "mem", "idx", x(rd)); return
    o.ex(AGET, "mem", "idx", "w")
    width = 8 if f3 in (0, 4) else 16
    o.ex(I32 % "LogicalAnd", "t", o.c(3 if width == 8 else 2), "sh")
    o.ex(I32 % "LeftShift", "sh", o.c(3), "sh")
    if f3 in (4, 5):
        o.ex(I32 % "RightShift", "w", "sh", "w")
        o.ex(I32 % "LogicalAnd", "w", o.c((1 << width) - 1), x(rd))
    else:
        o.ex(I32 % "Subtraction", o.c(32 - width), "sh", "sh")
        o.ex(I32 % "LeftShift", "w", "sh", "w")
        o.ex(I32 % "RightShift", "w", o.c(32 - width), x(rd))


def emit_store(o, d, known):
    f3, v = d["f3"], x(d["rs2"])
    a = mmio_addr(d, known)
    if a is not None:
        if a == UART_THR:
            o.ex(ASET, "outbuf", "outpos", v)
            o.ex(I32 % "Addition", "outpos", o.c(1), "outpos")
            o.copy("x0", "polls")
            o.halt(d["pc"] + 4)
        else:
            o.copy(o.c(d["pc"]), "badpc"); o.copy(o.c(2), "status"); o.emit("JUMP", "0xFFFFFFFC")
        return
    ram_index(o, d, d["imm"])
    if f3 == 2:
        o.ex(ASET, "mem", "idx", v); return
    width = 8 if f3 == 0 else 16
    o.ex(AGET, "mem", "idx", "w")
    o.ex(I32 % "LogicalAnd", "t", o.c(3 if width == 8 else 2), "sh")
    o.ex(I32 % "LeftShift", "sh", o.c(3), "sh")
    o.ex(I32 % "LeftShift", o.c((1 << width) - 1), "sh", "t2")
    o.ex(I32 % "LogicalXor", "t2", o.c(MASK), "t2")
    o.ex(I32 % "LogicalAnd", "w", "t2", "w")
    o.ex(I32 % "LogicalAnd", v, o.c((1 << width) - 1), "t2")
    o.ex(I32 % "LeftShift", "t2", "sh", "t2")
    o.ex(I32 % "LogicalOr", "w", "t2", "w")
    o.ex(ASET, "mem", "idx", "w")


def emit_amo(o, d, unsupported):
    f5, rd, v = d["f5"], d["rd"], x(d["rs2"])
    ram_index(o, d, 0)
    if f5 == 2:                                    # lr.w
        if rd: o.ex(AGET, "mem", "idx", x(rd))
        return
    if f5 == 3:                                    # sc.w: one hart, it always succeeds
        o.ex(ASET, "mem", "idx", v)
        if rd: o.copy("x0", x(rd))
        return
    ops = {0: "Addition", 4: "LogicalXor", 8: "LogicalOr", 12: "LogicalAnd"}
    o.ex(AGET, "mem", "idx", "w")
    if f5 == 1: o.ex(ASET, "mem", "idx", v)
    elif f5 in ops:
        o.ex(I32 % ops[f5], "w", v, "t2"); o.ex(ASET, "mem", "idx", "t2")
    else:
        unsupported.append(d["pc"])
    if rd: o.copy("w", x(rd))


def main():
    image_path, elf_path, out_dir = sys.argv[1:4]
    image = open(image_path, "rb").read()
    if elf_path == "-": text_addr, text_size = RAM, len(image) & ~3    # no ELF: all of it as code
    else: text_addr, text_size = text_range(open(elf_path, "rb").read())
    o, jt, ins, checked, unsupported = translate(image, text_addr, text_size)
    if unsupported:   # padding and data decode as these; they stop the run only if reached
        print("%d words are no instruction it translates (first at %08x)" % (len(unsupported), unsupported[0]))
    data = ["  %s: %%SystemInt32, 0" % x(r) for r in range(32)]
    for v in ("t", "t2", "t3", "idx", "w", "sh", "budget", "status", "badpc", "polls", "inpos", "inlen", "outpos", "mtime"):
        data.append("  %s: %%SystemInt32, 0" % v)
    data += ["  cnd: %SystemBoolean, null", "  tgt: %SystemUInt32, null", "  resume: %SystemUInt32, null",
             "  l: %SystemInt64, null", "  l2: %SystemInt64, null", "  l3: %SystemInt64, null",
             "  k_u32: %SystemInt64, null", "  k_l0: %SystemInt64, null",
             "  mem: %SystemInt32Array, null", "  jt: %SystemUInt32Array, null",
             "  inbuf: %SystemInt32Array, null", "  outbuf: %SystemInt32Array, null"]
    data += ["  %s: %%SystemInt32, 0x%08X" % (n, v) for n, v in sorted(o.consts.items())]
    data += ["  y_%08x: %%SystemUInt32, %d" % (pc, o.addr.get(pc, o.halt_at)) for pc in sorted(o.ycs)]
    asm = ".data_start\n" + "\n".join(data) + "\n.data_end\n.code_start\n  .export _start\n  _start:\n" + \
        "\n".join(o.lines) + "\n.code_end\n"
    os.makedirs(out_dir, exist_ok=True)
    open(os.path.join(out_dir, "prog.uasm"), "w").write(asm)
    json.dump({"jt": jt, "entry": jt[0], "text": [text_addr, text_size], "externs": sorted(o.externs),
               "udon_bytes": o.pos, "guest_insns": len(ins), "checks": len(checked)},
              open(os.path.join(out_dir, "prog.json"), "w"))
    open(os.path.join(out_dir, "prog.jt"), "wb").write(struct.pack("<%dI" % len(jt), *jt))
    open(os.path.join(out_dir, "prog.externs"), "w").write("\n".join(sorted(o.externs)))
    print("%d guest instructions -> %d bytes of Udon code (%d Udon instructions), %d loop checks"
          % (len(ins), o.pos, len(o.lines), len(checked)))


if __name__ == "__main__":
    main()
