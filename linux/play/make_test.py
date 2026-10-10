"""Makes nxplay's test files in linux/play/test (docs/play.md): a sweep with noise as WAV, MP3
and FLAC (ffmpeg does those), and a small four-channel MOD written here. Run on the host:
    python linux\\play\\make_test.py
"""
import math
import os
import random
import struct
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "test")


def sweep(seconds, noise):
    left = "0.4*sin(2*PI*(200*t+100*t*t))+%g*(random(0)*2-1)" % noise
    right = "0.4*sin(2*PI*(300*t+80*t*t))+%g*(random(1)*2-1)" % noise
    return ["-f", "lavfi", "-i", "aevalsrc=%s|%s:s=44100:d=%g" % (left, right, seconds)]


def ffmpeg(source, more, name):
    path = os.path.join(OUT, name)
    subprocess.run(["ffmpeg", "-v", "error", "-y"] + source + ["-bitexact"] + more + [path], check=True)
    print("%-16s %7d bytes" % (name, os.path.getsize(path)))


def mod():
    """Two patterns played twice: a bass, a lead with slides and vibrato, a drum and a hat."""
    rng = random.Random(7)
    square = bytes([60 if i % 32 < 16 else 196 for i in range(64)])
    saw = bytes([(i % 64 * 3 - 96) & 255 for i in range(128)])
    hat = bytes([int(rng.uniform(-100, 100) * math.exp(-i / 200.0)) & 255 for i in range(800)])
    kick = bytes([int(120 * math.sin(i * 0.5 * math.exp(-i / 500.0)) * math.exp(-i / 600.0)) & 255 for i in range(1600)])
    samples = [("bass", square, 48, 0, 32), ("lead", saw, 40, 0, 64), ("hat", hat, 30, 0, 1), ("kick", kick, 64, 0, 1)]
    C1, G1, A1, C2, D2, E2, G2, A2, C3 = 856, 570, 508, 428, 381, 339, 285, 254, 214

    def cell(sample=0, period=0, effect=0, value=0):
        return bytes([(sample & 0xf0) | period >> 8, period & 255, (sample & 15) << 4 | effect, value])

    patterns = []
    for p in range(2):
        rows = []
        bass = [C1, C1, G1, A1] if p == 0 else [A1, A1, G1, C1]
        lead = [C2, E2, G2, C3, A2, G2, E2, D2]
        for r in range(64):
            a = cell(1, bass[r // 16], 0, 0) if r % 4 == 0 else cell(0, 0, 0xA, 0x02)
            if r % 8 == 0:
                b = cell(2, lead[(r // 8 + p * 3) % 8], 0xC, 40)
            elif r % 8 == 4:
                b = cell(0, lead[(r // 8 + 1) % 8], 3, 0x10) if p else cell(0, 0, 0, 0x47)
            else:
                b = cell(0, 0, 4, 0x46)
            c = cell(4, C2) if r % 8 == 0 else cell(4, C2, 0xC, 30) if r % 16 == 14 else cell()
            d = cell(3, C3, 0xC, 20 + r % 4 * 6) if r % 2 else cell()
            if r == 0 and p == 0:
                d = cell(0, 0, 0xF, 6)
            rows.append(a + b + c + d)
        patterns.append(b"".join(rows))
    order = [0, 1, 0, 1]
    out = b"nxplay test".ljust(20, b"\0")
    for i in range(31):
        if i < len(samples):
            name, data, volume, loop, loop_length = samples[i]
            repeat = loop_length // 2 if loop_length > 2 else 1
            out += name.encode().ljust(22, b"\0") + struct.pack(">HBBHH", len(data) // 2, 0, volume, loop // 2, repeat)
        else:
            out += bytes(22) + struct.pack(">HBBHH", 0, 0, 0, 0, 1)
    out += bytes([len(order), 127]) + bytes(order).ljust(128, b"\0") + b"M.K." + b"".join(patterns)
    out += b"".join(s[1] for s in samples)
    with open(os.path.join(OUT, "play-test.mod"), "wb") as f:
        f.write(out)
    print("%-16s %7d bytes" % ("play-test.mod", len(out)))


os.makedirs(OUT, exist_ok=True)
ffmpeg(sweep(1.6, 0.05), ["-c:a", "pcm_s16le"], "play-test.wav")
ffmpeg(sweep(18, 0.05), ["-c:a", "libmp3lame", "-b:a", "128k"], "play-test.mp3")
ffmpeg(sweep(2, 0.001), ["-c:a", "flac", "-compression_level", "5"], "play-test.flac")
mod()
