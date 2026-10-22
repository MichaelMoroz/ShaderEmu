#!/usr/bin/env python3
"""A bFLT (rev 4) from a program linked at 0 that needs no relocations: mkflat.py RAW.bin ENTRY_OFFSET OUT [STACK].

RAW.bin is the program's bytes from address 0 (llvm-objcopy -O binary); the text is placed after the 64-byte header,
so the entry is 0x40 + its offset. No data or bss of its own, no relocation records, FLAT_FLAG_RAM."""
import struct
import sys
import time

raw = open(sys.argv[1], 'rb').read()
entry = int(sys.argv[2], 0)
stack = int(sys.argv[4], 0) if len(sys.argv) > 4 else 8192
raw += b'\0' * (-len(raw) % 16)
end = 0x40 + len(raw)
hdr = b'bFLT' + struct.pack('>11I', 4, 0x40 + entry, end, end, end, stack, end, 0, 0x1, int(time.time()), 0)
hdr += b'\0' * (0x40 - len(hdr))
open(sys.argv[3], 'wb').write(hdr + raw)
