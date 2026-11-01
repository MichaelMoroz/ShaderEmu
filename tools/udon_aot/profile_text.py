"""Where a translated kernel spends its Udon time: each text word's inline code, weighted by how
often the reference ran it (RvRef.PcCount), grouped by the kind of instruction.

python profile_text.py OUT_DIR linux_payload.bin pccount.i32
"""
import struct, sys
from collections import defaultdict
import numpy as np
import linux2udon as L

NS = {1: 6, 4: 16, 5: 10, 6: 64, 8: 14, 9: 21}      # editor-measured ns of each Udon opcode


def kind(d):
    k, f3 = d["k"], d["f3"]
    if k == "load": return "load " + {0: "lb", 1: "lh", 2: "lw", 4: "lbu", 5: "lhu"}[f3] + (" sp/gp/tp" if d["rs1"] in (2, 3, 4) else "")
    if k == "store": return "store " + {0: "sb", 1: "sh", 2: "sw"}[f3] + (" sp/gp/tp" if d["rs1"] in (2, 3, 4) else "")
    if k == "op": return "op mul" if d["f7"] == 1 else "op " + ["add/sub", "sll", "slt", "sltu", "xor", "srl/sra", "or", "and"][f3]
    if k == "opimm":
        if d["rd"] == 0: return "nop"
        if f3 == 0: return "addi (mv/li)" if d["imm"] == 0 or d["rs1"] == 0 else "addi"
        return "opimm " + ["", "slli", "slti", "sltiu", "xori", "srli/srai", "ori", "andi"][f3]
    if k == "branch": return "branch " + ("unsigned" if f3 >= 6 else "signed/eq")
    return k


def main():
    out, payload, pcc = sys.argv[1:4]
    code = np.frombuffer(open(out + "/prog.bc", "rb").read(), dtype=">u4").tolist()
    import os
    f = out + ("/pcpos.bin" if os.path.exists(out + "/pcpos.bin") else "/jt.u32")
    jt = struct.unpack("<%dI" % (os.path.getsize(f) // 4), open(f, "rb").read())
    counts = np.fromfile(pcc, dtype="<i4")
    img = open(payload, "rb").read()
    words = struct.unpack_from("<%dI" % len(jt), img, L.TEXT - L.RAM)
    by = defaultdict(lambda: [0, 0.0, 0])        # executions, ns, externs
    total_ns = 0.0
    for i in np.nonzero(counts)[0]:
        n = int(counts[i])
        a, b = jt[i] // 4, (jt[i + 1] // 4 if i + 1 < len(jt) else jt[i] // 4 + 64)
        ns, ex, p = 0.0, 0, a
        while p < b:
            op = code[p]; ns += NS.get(op, 0); ex += op == 6
            p += 1 if op in (0, 2, 9) else 2
        g = by[kind(L.decode(words[i]))]
        g[0] += n; g[1] += n * ns; g[2] += n * ex
        total_ns += n * ns
    print("%-22s %12s %8s %8s %7s" % ("kind", "executions", "ns each", "externs", "share"))
    for k, (n, ns, ex) in sorted(by.items(), key=lambda kv: -kv[1][1]):
        print("%-22s %12d %8.0f %8.2f %6.1f%%" % (k, n, ns / n, ex / n, 100 * ns / total_ns))
    n = int(counts.sum())
    print("all: %d supervisor instructions, %.2f s predicted inline (%.0f ns each)" % (n, total_ns / 1e9, total_ns / n))


if __name__ == "__main__":
    main()
