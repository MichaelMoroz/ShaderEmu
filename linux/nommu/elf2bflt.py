"""A static-PIE RV32 ELF linked at 0x40 (so that addresses are file offsets) to a bFLT v4 file, loaded whole into RAM.
    python elf2bflt.py IN OUT [STACK]"""
import struct, sys

data = open(sys.argv[1], 'rb').read()
stack = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x20000
assert data[:4] == b'\x7fELF' and data[4] == 1 and data[5] == 1, 'not a little-endian ELF32'
e_entry, e_phoff, e_shoff = struct.unpack_from('<III', data, 24)
e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHHHH', data, 42)

loads, dynamic = [], None
for i in range(e_phnum):
    p_type, p_off, p_va, _, p_filesz, p_memsz, p_flags, _ = struct.unpack_from('<8I', data, e_phoff + i * e_phentsize)
    if p_type == 1:
        loads.append((p_va, p_off, p_filesz, p_memsz, p_flags))
    elif p_type == 2:
        dynamic = (p_off, p_filesz)
    elif p_type == 7 and p_memsz:
        print('WARNING: %s has thread-local storage (%d bytes), which flatcrt does not set up' % (sys.argv[1], p_memsz))
loads.sort()
# at 0 the first 64 bytes are the ELF header, which nothing reads once loaded: the bFLT header takes them
assert loads[0][0] in (0, 0x40), 'the first segment must be linked at 0 or 0x40, got %#x' % loads[0][0]
assert loads[0][0] == 0x40 or loads[0][1] == 0, 'a segment at 0 must start with the ELF header'

# the image: every loaded byte at its address; the last segment's memory past its file size is bss
end_file = max(va + fs for va, _, fs, _, _ in loads)
end_mem = max(va + ms for va, _, _, ms, _ in loads)
img = bytearray(end_file)
for va, off, fs, ms, fl in loads:
    img[va:va + fs] = data[off:off + fs]
    if ms > fs and va + ms < end_file:            # a bss inside the image (between segments)
        img[va + fs:va + ms] = bytes(va + ms - va - fs)

# text is the first segment (executable); data starts at the first writable one, aligned as the loader wants
wseg = [s for s in loads if s[4] & 2]
# (loaded whole, the split is only where the loader puts data: rounded down, every address stays where it is)
data_start = (wseg[0][0] if wseg else end_file) & ~31
# binfmt_flat (5.17, BINFMT_FLAT_NO_DATA_START_OFFSET or not: fs/binfmt_flat.c's shared-library table) stores the
# data's start into the word just below it: nothing of the program may lie there (it can overwrite a variable such as musl's
# decfloat and broke strtod). The linker leaves a page between text and data, so with it nothing ever does; where
# something does (tcc's .eh_frame_hdr runs to its data), the data starts lower, at the first 32 bytes with a word of
# nothing below (loaded whole, the split moves no address), and with none the program is refused
covered = []
if e_shoff and e_shnum:
    names = struct.unpack_from('<10I', data, e_shoff + e_shstrndx * e_shentsize)[4]
    for k in range(e_shnum):
        sh_name, sh_type, sh_flags, sh_addr, _, sh_size = struct.unpack_from('<6I', data, e_shoff + k * e_shentsize)
        if sh_flags & 2 and sh_size and not sh_flags & 0x400:   # (ALLOC, not TLS)
            covered.append((sh_addr, sh_addr + sh_size, data[names + sh_name:data.index(b'\0', names + sh_name)].decode()))
else:
    covered = [(va, va + ms, 'the segment at %#x' % va) for va, _, _, ms, _ in loads]
start, text_end = data_start, max(va + ms for va, _, _, ms, fl in loads if fl & 1)   # (not into the text)
while data_start >= max(4, text_end) and any(a < data_start and b > data_start - 4 for a, b, _ in covered):
    data_start -= 32
if data_start < max(4, text_end):
    sys.exit("%s: every 32 bytes from the text's end to the data's (%#x) has a section below it, which the kernel overwrites (%s)" %
             (sys.argv[1], start, ', '.join(n for a, b, n in covered if a < start and b > start - 4)))

relocs = []
if dynamic:
    tags = {}
    off, size = dynamic
    for k in range(0, size, 8):
        tag, val = struct.unpack_from('<iI', data, off + k)
        if tag == 0:
            break
        tags[tag] = val
    rela, relasz = tags.get(7), tags.get(8, 0)          # DT_RELA, DT_RELASZ
    if rela is not None:
        # the table's address is a virtual one: find its file bytes in the image
        for k in range(0, relasz, 12):
            r_off, r_info, r_add = struct.unpack_from('<IIi', img, rela + k)
            kind, sym = r_info & 0xff, r_info >> 8
            assert kind in (1, 3), 'only R_RISCV_32 and R_RISCV_RELATIVE, got %d at %#x' % (kind, r_off)
            if kind == 1:   # a word naming a defined symbol: its address, relocated like any other
                _, st_value, _, st_shndx = struct.unpack_from('<IIIxxH', img, tags[6] + 16 * sym)   # DT_SYMTAB
                if st_shndx == 0:   # a weak symbol nothing defines: 0, and no relocation
                    assert img[tags[6] + 16 * sym + 12] >> 4 == 2, 'R_RISCV_32 to an undefined symbol at %#x' % r_off
                    struct.pack_into('<I', img, r_off, r_add & 0xffffffff)
                    continue
                r_add += st_value
            struct.pack_into('<I', img, r_off, r_add & 0xffffffff)
            relocs.append(r_off)

# The loader takes addresses from the end of the 64-byte header: the image follows it, its address 0 there,
# and the header's offsets are the file's (the image's plus 64).
H = 0x40
# (the relocations and the words they name in the machine's order: patch_flatreloc.py; the kernel made a word by four shifts, twice a relocation)
LE_RELOCS = 0x40000000
# (but the entry is taken from the file's start, header included)
hdr = struct.pack('>4sIIIIIIIIII', b'bFLT', 4, H + e_entry, H + data_start, H + end_file, H + end_mem, stack, H + len(img), len(relocs), 1 | LE_RELOCS, 0)
out = bytearray(hdr) + bytes(H - len(hdr)) + img
for r in relocs:
    out += struct.pack('<I', r)
open(sys.argv[2], 'wb').write(out)
print('%s: text %#x, data %#x..%#x, bss to %#x, %d relocations, entry %#x' % (sys.argv[2], data_start, data_start, end_file, end_mem, len(relocs), e_entry))
