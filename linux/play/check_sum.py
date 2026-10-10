"""The sum nxplay prints with NXPLAY_SUM=1, worked out from a WAV file on the host:
    python linux\\play\\check_sum.py linux\\play\\test\\play-test.wav [SECONDS]
FNV-1a taken a 16-bit sample at a time, over the samples in the order they are played (the
first two channels; the top 16 bits of wider samples; a byte b as (b - 128) * 256).
"""
import struct
import sys


def samples(path, seconds=0):
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit("not a WAV file")
    at, fmt, body = 12, None, None
    while at + 8 <= len(data):
        name, size = data[at:at + 4], struct.unpack_from("<I", data, at + 4)[0]
        size = min(size, len(data) - at - 8)
        if name == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", data, at + 8)
        elif name == b"data":
            body = data[at + 8:at + 8 + size]
            break
        at += 8 + (size + 1 & ~1)
    kind, channels, rate, _, _, bits = fmt
    width = bits // 8
    frames = len(body) // (width * channels)
    if seconds:
        frames = min(frames, seconds * rate)
    for f in range(frames):
        for c in range(min(channels, 2)):
            s = body[(f * channels + c) * width:(f * channels + c + 1) * width]
            yield (s[0] - 128) << 8 & 0xffff if width == 1 else s[-2] | s[-1] << 8


def fnv(values):
    h, n = 2166136261, 0
    for v in values:
        h = (h ^ v) * 16777619 & 0xffffffff
        n += 1
    return h, n


if __name__ == "__main__":
    print("sum %08x of %u samples" % fnv(samples(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 0)))
