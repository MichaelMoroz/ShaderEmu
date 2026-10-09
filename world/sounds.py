# The weather's sounds, computed (python world/sounds.py): rain against the window as a loop
# that joins itself exactly, and three rolls of thunder. Unity gets Sounds/*.wav.
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


def thunder(name, seed, length, crack):
    count = int(RATE * length)
    rng = np.random.default_rng(seed)
    t = np.arange(count) / RATE
    channels = []
    # a roll is several claps arriving one after another, each longer and duller than the last
    claps = [(0.0, 1.0)] + [(rng.uniform(0.3, length * 0.5), rng.uniform(0.3, 0.8)) for _ in range(rng.integers(3, 6))]
    for side in range(2):
        rumble = shaped(count, seed * 10 + side, lambda f: band(f, 28, 150, 1.5) / f ** 0.6)
        sharp = shaped(count, seed * 10 + 5 + side, lambda f: band(f, 250, 2400, 1.0) / f ** 0.5)
        low, high = np.zeros(count), np.zeros(count)
        for at, size in claps:
            after = np.clip(t - at, 0, None)
            on = (t >= at) * (1 - np.exp(-after / 0.012))
            low += size * on * np.exp(-after / (1.1 + at * 0.6))
            high += size * on * np.exp(-after / 0.16) * (crack if at == 0.0 else crack * 0.35)
        fade = np.clip((length - t) / 1.5, 0, 1)
        channels.append((rumble * low * 1.6 + sharp * high) * fade)
    write(name, np.array(channels), 0.8)


def main():
    os.makedirs(OUT, exist_ok=True)
    rain()
    thunder("Thunder1", 1, 9.0, 0.9)    # close
    thunder("Thunder2", 2, 11.0, 0.35)  # further off
    thunder("Thunder3", 3, 8.0, 0.15)   # a long way away: only the roll


if __name__ == "__main__":
    main()
