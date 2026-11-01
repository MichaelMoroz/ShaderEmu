"""Age of Empires' sounds in a run's console (AOE_SOUND_LOG=1; docs/games/aoe.md, "Sound"): is what each voice was pointed at a real
wave of the trial, whole, and did it play as long as it lasts?  python tools/aoe_sound_check.py LOG IMAGE
It looks up each `aoesnd: play` address in the image's rootfs.bin (the ROM, as the card reads it from 0x40000000) and compares the samples
with the wave of that length in the image's Sounds.drs and Interfac.drs."""
import hashlib, os, re, struct, sys


def waves(drs):
    d = open(drs, 'rb').read()
    out = {}
    for i in range(struct.unpack_from('<I', d, 0x38)[0]):
        kind, table, count = struct.unpack_from('<4sII', d, 0x40 + i * 12)
        if kind != b' vaw':
            continue
        for k in range(count):
            rid, at, size = struct.unpack_from('<III', d, table + k * 12)
            o = at + 12
            while o + 8 <= at + size:
                n = struct.unpack_from('<I', d, o + 4)[0]
                if d[o:o + 4] == b'data':
                    out[hashlib.md5(d[o + 8:o + 8 + n]).hexdigest()] = (rid, n)
                    break
                o += 8 + n + (n & 1)
    return out


def main(log, image):
    known = {}
    for name in ('Sounds.drs', 'Interfac.drs'):
        known.update(waves(os.path.join(image, 'overlay', 'usr', 'share', 'aoe', name)))
    rom = open(os.path.join(image, 'rootfs.bin'), 'rb')
    plays = wrong = 0
    ids = {}
    silent = 0
    for m in re.finditer(r'aoesnd: play \d+ at ([0-9a-f]+), (\d+) bytes, (\d+) Hz, (\d+) bits, voice (\d+), volume ([0-9a-f]+), at (\d+) ms', open(log, encoding='latin-1').read()):
        phys, n = int(m[1], 16), int(m[2])
        rom.seek(phys - 0x40000000)
        samples = rom.read(n)
        got = known.get(hashlib.md5(samples).hexdigest())
        plays += 1
        if not got:
            wrong += 1
            print('not a wave of the trial:', m[0])
            continue
        ids[got[0]] = ids.get(got[0], 0) + 1
        peak = max(abs(v) for v in struct.unpack('<%dh' % (n // 2), samples[:n & ~1])) if m[4] == '16' else max(abs(v - 128) for v in samples)
        silent += peak < 8
    text = open(log, encoding='latin-1').read()
    ended = [(int(a), int(b)) for a, b in re.findall(r'aoesnd: ended \d+ at [0-9a-f]+ after (\d+) ms of (\d+)', text)]
    short = sum(a * 2 < b for a, b in ended)
    print(f'{plays} plays of {len(ids)} waves, {wrong} not of the trial, {silent} of silence; {len(ended)} seen to end, {short} in under half their length')
    print('most played:', sorted(ids.items(), key=lambda kv: -kv[1])[:8])
    return 1 if wrong or not plays else 0


sys.exit(main(sys.argv[1], sys.argv[2]))
