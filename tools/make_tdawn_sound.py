"""Tiberian Dawn's sounds in a form the sound card plays where they lie in the ROM (docs/sound.md).

  python3 tools/make_tdawn_sound.py OUT.pak SOUNDS.MIX SPEECH.MIX ... [TUNE.AUD ...] [FOLDER ...]

(Red Alert's go through it too, linux/ralert/build.sh: folders of its MIX files' contents,
every file of which that is an AUD file is taken, whatever its name ends in.)

The game's AUD files are IMA ADPCM in chunks with headers between them. The card's ADPCM
voice wants the codes alone, and the decoder's state every 32 samples so that any sample can
be reached in a few steps: that is what goes into OUT.pak, with an index the game looks a
sound up in by a hash of how its AUD file starts (linux/tdawn/soundio_shaderemu.cpp).
The few files in Westwood's own 8-bit coding are stored decoded, as 8-bit PCM.

  'TDSN', count, then count entries sorted by key:
      key, kind (4 ADPCM, 1 PCM8), rate, samples, data offset, checkpoints offset (0 for PCM8)

--check decodes every entry again, the way the device does, and compares with a plain decode.
"""
import os
import struct
import sys

STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
         130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
         1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
         7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
KEY_BYTES = 44
# for each step index and code: the change in value, and the next index
TABLE = []
for index, size in enumerate(STEPS):
    row = []
    for code in range(16):
        change = (size >> 3) + (size if code & 4 else 0) + (size >> 1 if code & 2 else 0) + (size >> 2 if code & 1 else 0)
        row.append((-change if code & 8 else change, min(max(index + (2 * (code & 3) + 2 if code & 4 else -1), 0), 88)))
    TABLE.append(row)


def fnv(data):
    h = 0x811c9dc5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def mix_entries(path):
    """The files of a MIX archive: a count, the body's size, then (id, offset, size) each."""
    data = open(path, 'rb').read()
    count, = struct.unpack_from('<H', data, 0)
    body = 6 + 12 * count
    for n in range(count):
        _, offset, size = struct.unpack_from('<III', data, 6 + 12 * n)
        yield data[body + offset:body + offset + size]


def chunks(aud):
    """The data of an AUD file's chunks: each has its size, its size decoded and a mark."""
    at = 12
    while at + 8 <= len(aud):
        size, out, mark = struct.unpack_from('<HHI', aud, at)
        if mark != 0xdeaf:
            break
        yield aud[at + 8:at + 8 + size], out
        at += 8 + size


def unzap(data, out):
    """Westwood's 8-bit coding: runs of 2-bit and 4-bit steps, copies and repeats."""
    if len(data) == out:
        return bytearray(data)
    pcm, value, at = bytearray(), 0x80, 0
    step2, step4 = (-2, -1, 0, 1), (-9, -8, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 8)
    while len(pcm) < out and at < len(data):
        command, count = data[at] >> 6, data[at] & 0x3f
        at += 1
        if command == 0:
            for b in data[at:at + count + 1]:
                for shift in (0, 2, 4, 6):
                    value = min(max(value + step2[(b >> shift) & 3], 0), 255)
                    pcm.append(value)
            at += count + 1
        elif command == 1:
            for b in data[at:at + count + 1]:
                for shift in (0, 4):
                    value = min(max(value + step4[(b >> shift) & 15], 0), 255)
                    pcm.append(value)
            at += count + 1
        elif command == 2 and count & 0x20:
            value = (value + ((count & 0x1f) - 32 if count & 0x10 else count & 0x1f)) & 0xff
            pcm.append(value)
        elif command == 2:
            pcm += data[at:at + count + 1]
            value = pcm[-1]
            at += count + 1
        else:
            pcm += bytes([value]) * (count + 1)
    return pcm[:out]


def convert(aud):
    """An AUD file as (kind, rate, samples, data, checkpoints), or None if it is not one we play."""
    if len(aud) < 20:
        return None
    rate, size, out, flags, coding = struct.unpack_from('<HIIBB', aud, 0)
    if size + 12 != len(aud) or coding not in (1, 99) or flags & 1 or not 4000 <= rate <= 48000:
        return None
    if 20000 < rate < 24000:
        rate = 22050   # as the game does
    if coding == 1:
        pcm = bytearray()
        for data, decoded in chunks(aud):
            pcm += unzap(data, decoded)
        return 1, rate, len(pcm), bytes(pcm), b''
    codes = b''.join(data for data, _ in chunks(aud))
    samples = min(2 * len(codes), out // 2)
    marks, value, index = [], 0, 0
    for n in range(samples):
        if n & 31 == 0:
            marks.append((value & 0xffff) | index << 16)
        change, index = TABLE[index][(codes[n >> 1] >> (4 * (n & 1))) & 15]
        value = min(max(value + change, -32768), 32767)
    return 4, rate, samples, codes[:(samples + 1) // 2], struct.pack('<%dI' % len(marks), *marks)


def decode_plain(codes, samples):
    pcm, value, index = [], 0, 0
    for n in range(samples):
        change, index = TABLE[index][(codes[n >> 1] >> (4 * (n & 1))) & 15]
        value = min(max(value + change, -32768), 32767)
        pcm.append(value)
    return pcm


def decode_seeking(codes, marks, p):
    """Sample p as the device finds it: from the checkpoint before it."""
    mark, = struct.unpack_from('<I', marks, 4 * (p >> 5))
    value, index = ((mark & 0xffff) ^ 0x8000) - 0x8000, mark >> 16
    for n in range(p & ~31, p + 1):
        change, index = TABLE[index][(codes[n >> 1] >> (4 * (n & 1))) & 15]
        value = min(max(value + change, -32768), 32767)
    return value


def main():
    args = [a for a in sys.argv[1:] if a != '--check']
    out, sounds = args[0], {}
    for path in args[1:]:
        # an archive of them, one AUD file by itself (a tune), or a folder of files
        if os.path.isdir(path):
            found = [open(os.path.join(path, name), 'rb').read() for name in sorted(os.listdir(path))]
        else:
            found = [open(path, 'rb').read()] if path.upper().endswith('.AUD') else mix_entries(path)
        for aud in found:
            made = convert(aud)
            if made:
                key = fnv(aud[:KEY_BYTES])
                if key in sounds and sounds[key] != made:
                    sys.exit('two sounds begin alike: lengthen KEY_BYTES')
                sounds[key] = made
    at = 8 + 24 * len(sounds)
    index, body = b'', b''
    for key in sorted(sounds):
        kind, rate, samples, data, marks = sounds[key]
        data += b'\0' * (-len(data) % 4)
        index += struct.pack('<6I', key, kind, rate, samples, at + len(body), at + len(body) + len(data) if marks else 0)
        body += data + marks
    open(out, 'wb').write(b'TDSN' + struct.pack('<I', len(sounds)) + index + body)
    coded = sum(1 for s in sounds.values() if s[0] == 4)
    print('%s: %d sounds (%d ADPCM, %d PCM), %d samples, %d bytes' % (
        out, len(sounds), coded, len(sounds) - coded, sum(s[2] for s in sounds.values()), at + len(body)))
    if '--check' in sys.argv:
        wrong = 0
        for kind, rate, samples, data, marks in sounds.values():
            if kind == 4:
                plain = decode_plain(data, samples)
                wrong += sum(1 for p in range(0, samples, 7) if decode_seeking(data, marks, p) != plain[p])
        print('check: %d samples reached from a checkpoint differ from a plain decode' % wrong)
        sys.exit(1 if wrong else 0)


if __name__ == '__main__':
    main()
