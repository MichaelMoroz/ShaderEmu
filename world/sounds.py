# The weather's sounds, computed (python world/sounds.py): rain against the window as a loop
# that joins itself exactly, three rolls of thunder, and the computer's own sounds. Unity gets Sounds/*.wav.
import os
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "unity", "ShaderEmu", "Sounds")
RATE = 44100


def shaped(count, seed, gain):
    """Noise whose spectrum is `gain(frequency in Hz)`; periodic over `count` samples."""
    rng = np.random.default_rng(seed)
    f = np.fft.rfftfreq(count, 1.0 / RATE)
    spectrum = gain(np.maximum(f, 1.0)) * np.exp(2j * np.pi * rng.random(f.size))
    spectrum[0] = 0
    signal = np.fft.irfft(spectrum, count)
    return signal / np.abs(signal).max()


def band(f, low, high, slope=2.0):
    return 1.0 / (1.0 + (low / f) ** (2 * slope)) / (1.0 + (f / high) ** (2 * slope))


def write(name, stereo, peak):
    stereo = stereo / np.abs(stereo).max() * peak
    data = (stereo.T * 32767).astype("<i2")
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(data.tobytes())
    rms = 20 * np.log10(np.sqrt((stereo ** 2).mean()))
    print("%-10s %5.1f s  rms %6.1f dBFS  peak %5.1f dBFS" % (name, stereo.shape[1] / RATE, rms, 20 * np.log10(np.abs(stereo).max())))


def rain():
    count = RATE * 16
    channels = []
    for side in range(2):
        hiss = shaped(count, 10 + side, lambda f: band(f, 900, 7000) / np.sqrt(f))      # the fall outside
        roof = shaped(count, 20 + side, lambda f: band(f, 90, 420) / f ** 0.3)          # its weight on the building
        # drops on the glass: short ticks, each a decaying burst, some nearer one ear
        rng = np.random.default_rng(30 + side)
        ticks = np.zeros(count)
        for start in rng.integers(0, count, 2600):
            length = int(rng.uniform(0.004, 0.02) * RATE)
            t = np.arange(length) / RATE
            pitch = rng.uniform(1800, 5200)
            drop = np.sin(2 * np.pi * pitch * t) * np.exp(-t / rng.uniform(0.0015, 0.006)) * rng.uniform(0.15, 1.0) ** 2
            index = (start + np.arange(length)) % count   # over the loop's end and round
            ticks[index] += drop
        # the fall swells and thins slowly; a whole number of swells keeps the loop
        swell = 1.0 + 0.18 * np.sin(2 * np.pi * 3 * np.arange(count) / count + side) + 0.1 * np.sin(2 * np.pi * 7 * np.arange(count) / count)
        channels.append((0.75 * hiss + 0.45 * roof) * swell + 0.55 * ticks / np.abs(ticks).max())
    write("Rain", np.array(channels), 0.35)


def slow(count, seed, rate):
    """A value that wanders between 0 and 1 about `rate` times a second."""
    rng = np.random.default_rng(seed)
    knots = rng.random(int(count / RATE * rate) + 3)
    at = np.arange(count) / RATE * rate
    i = at.astype(int)
    f = at - i
    f = f * f * (3 - 2 * f)
    return knots[i] * (1 - f) + knots[i + 1] * f


def thunder(name, seed, length, near):
    """Thunder from a stroke `near` (1 overhead, 0 far off). The channel is kilometres long, so its
    sound arrives over seconds: nothing starts at once but a near stroke's crackle, and the roll
    is many arrivals, each swelling and dying, under a level that wanders."""
    count = int(RATE * length)
    rng = np.random.default_rng(seed)
    t = np.arange(count) / RATE
    channels = []
    spread = 1.2 + 3.5 * (1 - near)                      # how long the arrivals go on coming
    arrivals = [(rng.uniform(0, spread) ** 1.0 + (0.0 if near > 0.5 else 0.6), rng.uniform(0.3, 1.0),
                 rng.uniform(0.05, 0.2) + 0.3 * (1 - near), rng.uniform(0.25, 1.1)) for _ in range(40)]
    cracks = [(rng.uniform(0.0, 0.45), rng.uniform(0.3, 1.0), rng.uniform(0.012, 0.05)) for _ in range(int(14 * near))]
    top = 90 + 240 * near                                # air takes the high sound out of a far one
    for side in range(2):
        rumble = shaped(count, seed * 10 + side, lambda f: band(f, 24, top, 1.5) / f ** 0.7)
        body = shaped(count, seed * 10 + 3 + side, lambda f: band(f, 140, 500 + 900 * near, 1.0) / f ** 0.6)
        tear = shaped(count, seed * 10 + 6 + side, lambda f: band(f, 500, 7000, 1.0) / f ** 0.35)
        low, mid, high = np.zeros(count), np.zeros(count), np.zeros(count)
        for at, size, rise, fall in arrivals:
            after = np.clip(t - at, 0, None)
            swell = (t >= at) * (1 - np.exp(-after / rise)) * np.exp(-after / fall)
            low += size * swell
            mid += size * swell * np.exp(-after / (0.25 + 0.3 * rise))
        for at, size, fall in cracks:
            after = np.clip(t - at, 0, None)
            high += size * (t >= at) * (1 - np.exp(-after / 0.0015)) * np.exp(-after / fall)
        roll = (0.25 + 0.75 * slow(count, seed * 7 + side, 3.5)) * (0.35 + 0.65 * slow(count, seed * 9 + side, 11.0))   # it rattles as it rolls
        fade = np.clip((length - t) / 2.5, 0, 1) ** 1.5
        channels.append((rumble * low * roll * 0.9 + body * low * roll * (0.5 + 0.5 * near) + body * mid * (0.4 + 0.6 * near) + tear * high * 0.9 * near) * fade)
    write(name, np.array(channels), 0.8)


# ---------------------------------------------------------------- the computer's own sounds
def mono(name, signal, peak):
    write(name, np.array([signal, signal]), peak)


def tick(rng, pitch, ring):
    """A head's arm stopping: a knock in the drive's case that rings a moment."""
    length = int(0.03 * RATE)
    t = np.arange(length) / RATE
    return (np.sin(2 * np.pi * pitch * t) * np.exp(-t / ring) + 0.5 * np.sin(2 * np.pi * pitch * 0.31 * t) * np.exp(-t / (ring * 2.5))
            + 0.3 * rng.standard_normal(length) * np.exp(-t / 0.0012))


def seeks(rng, length, busy):
    """A disk at work: runs of the arm's knocks, a few at a time."""
    out = np.zeros(int(length * RATE))
    at = rng.uniform(0.0, 0.05)
    while at < length - 0.05:
        for _ in range(rng.integers(1, 6)):
            knock = tick(rng, rng.uniform(1700, 3300), rng.uniform(0.002, 0.005)) * rng.uniform(0.35, 1.0)
            start = int(at * RATE)
            end = min(out.size, start + knock.size)
            out[start:end] += knock[:end - start]
            at += rng.uniform(0.012, 0.045)
            if at >= length - 0.05:
                break
        at += rng.uniform(0.03, 0.4) / busy
    return out


def whine(count, start, end, seconds):
    """A drive's spindle and its bearings, from one speed to another over `seconds`."""
    t = np.arange(count) / RATE
    speed = start + (end - start) * (1 - np.exp(-t / (seconds / 3.5)))   # revolutions a second
    phase = 2 * np.pi * np.cumsum(speed) / RATE
    out = np.zeros(count)
    for harmonic, level in ((1, 0.25), (2, 0.12), (8, 0.5), (16, 0.22), (24, 0.3), (48, 0.12)):   # the motor's poles and the bearings' balls
        out += level * np.sin(harmonic * phase + harmonic)
    return out * (speed / max(start, end)) ** 1.5


def beep(seconds, pitch=1000.0):
    """The loudspeaker in the case: a square wave through a paper cone of 6 cm."""
    count = int(seconds * RATE)
    t = np.arange(count) / RATE
    wave_ = np.sign(np.sin(2 * np.pi * pitch * t))
    spectrum = np.fft.rfft(wave_)
    f = np.maximum(np.fft.rfftfreq(count, 1.0 / RATE), 1.0)
    out = np.fft.irfft(spectrum * band(f, 600, 4500, 1.0), count)
    edge = np.clip(np.minimum(t, seconds - t) / 0.003, 0, 1)
    return out / np.abs(out).max() * edge


def put(track, at, sound, level=1.0):
    start = int(at * RATE)
    end = min(track.size, start + sound.size)
    track[start:end] += sound[:end - start] * level


def computer():
    rng = np.random.default_rng(486)
    # running: the supply's fan and the air through the case, the mains' hum, the disk's spindle (3,600 rpm)
    count = RATE * 8
    t = np.arange(count) / RATE
    air = shaped(count, 71, lambda f: band(f, 120, 2600, 1.0) / f ** 0.75)
    blades = sum(level * np.sin(2 * np.pi * pitch * t) for pitch, level in ((50, 0.05), (100, 0.07), (245, 0.05), (490, 0.025)))   # 2,100 rpm, seven blades
    spindle = whine(count, 60, 60, 1.0)
    mono("PcRun", air * 0.9 + blades + spindle * 0.022, 0.25)

    # switched on: the switch, the fan and the disk coming up to speed, the memory counted on the
    # loudspeaker, the floppy drive's head sent home, one beep, and the disk read
    length = 8.0
    count = int(length * RATE)
    t = np.arange(count) / RATE
    boot = np.zeros(count)
    clunk = tick(rng, 900, 0.003) + 0.6 * tick(rng, 2400, 0.002)   # a rocker switch: a snap, not a knock
    put(boot, 0.02, clunk, 0.35)
    rise = 1 - np.exp(-t / 0.9)
    boot += shaped(count, 72, lambda f: band(f, 120, 2600, 1.0) / f ** 0.75) * 0.5 * rise * np.clip((length - t) / 1.5, 0, 1)   # hands over to the running sound
    put(boot, 0.25, whine(int(4.2 * RATE), 2, 60, 3.6) * 0.06 * np.clip((4.2 - np.arange(int(4.2 * RATE)) / RATE) / 1.2, 0, 1))
    for i in range(46):   # the memory test ticks as it counts
        put(boot, 1.9 + i * 0.03, beep(0.004, 1600), 0.2)
    step = np.zeros(int(0.5 * RATE))   # the floppy's stepper: 80 tracks out and back, a buzz of steps
    for i in range(40):
        put(step, i * 0.006, tick(rng, 900, 0.0025), 0.5)
    put(boot, 3.5, step, 0.55)
    put(boot, 4.0, step[::-1].copy(), 0.45)
    put(boot, 4.75, beep(0.25), 1.0)
    put(boot, 5.2, seeks(rng, 2.6, 2.5), 0.5)
    mono("PcBoot", boot, 0.7)

    # the disk at work, a few ways
    for i in range(4):
        mono("PcSeek%d" % (i + 1), seeks(rng, rng.uniform(0.25, 0.9), rng.uniform(1.0, 2.5)), 0.45)

    # switched off: the switch, and everything running down
    length = 4.0
    count = int(length * RATE)
    t = np.arange(count) / RATE
    off = shaped(count, 73, lambda f: band(f, 120, 2600, 1.0) / f ** 0.75) * 0.5 * np.exp(-t / 0.7)
    off += whine(count, 60, 1, 3.2) * 0.05 * np.clip((length - t) / 1.0, 0, 1)
    put(off, 0.01, clunk, 0.35)
    mono("PcOff", off, 0.6)


def main():
    os.makedirs(OUT, exist_ok=True)
    rain()
    thunder("Thunder1", 1, 10.0, 1.0)   # close
    thunder("Thunder2", 2, 13.0, 0.45)  # further off
    thunder("Thunder3", 3, 14.0, 0.1)   # a long way away: only the roll
    computer()


if __name__ == "__main__":
    main()
