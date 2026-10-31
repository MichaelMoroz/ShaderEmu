#!/usr/bin/env python3
"""A code area of one kind of bundle, for wide.h's WIDE_SYNTH: what a trip of each kind costs with nothing of the machine's in it.
    python synth_code.py OUT.code KIND     KIND: sum, sumy (op 1 used), unit, branch (taken, to the next), fall (not taken), jump, load, nop"""
import sys
import numpy as np
W, ROWS, B0 = 2048, 5700, 2304 * 2048
SINK = 32
def x(rd, ra, rb, fn=0): return rd | ra << 6 | rb << 11 | fn << 16
out, kind = sys.argv[1:3]
area = np.zeros((ROWS * W, 4), dtype=np.uint32)
for i in range(128):
    X, Y, Z, path, ctl = x(5 + i % 3, 6, 7), SINK, 1, 0, 0
    if kind == 'nop': X = x(SINK, 0, 0); Z = 0
    if kind == 'sumy': Y = x(8 + i % 3, 9, 10)
    if kind == 'unit': path = 1; X = x(5 + i % 3, 6, 7, 2)
    if kind == 'branch': ctl = 1 << 6 | 0 << 8 | (1 << 21)            # beq x0, x0, next
    if kind == 'fall': ctl = 1 << 6 | 1 << 8 | (1 << 21)              # bne x0, x0, next
    if kind == 'jump': ctl = 2 << 6 | 1 << 8
    if kind == 'load': path = 2; X = x(5 + i % 3, 2, 0, 2); Z = (i % 8) * 4    # lw from the stack
    area[B0 + i] = (X, Y, Z, path | ctl)
area.reshape(ROWS, W, 4)[::-1].tofile(out)
