# The world's sounds (python world/sounds.py): rain against the window, computed, as a loop that
# joins itself exactly; thunder, the computer's own sounds and the keys' strokes, cut from recordings.
# Unity gets Sounds/*.wav.
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


# ---------------------------------------------------------------- recordings
# Thunder and the computer are recordings from freesound.org, all given to the public domain
# (CC0) by the people named. Freesound gives the whole file only to a member; its own preview
# of each (an MP3) is what is fetched, into build/sounds, and ffmpeg makes a WAV of it.
SOURCES = {
    "computer": ("273/273736_1149179", "computer startup.wav, by squashy555"),          # switch, a beep, disk and fans, switch off
    "thunder_near": ("534/534023_9395330", "Thunderclap, by Fission9"),
    "thunder_peal": ("243/243778_997601", "peal of thunder close, by bastipictures"),
    "thunder_far": ("397/397952_2247456", "Thunder Clap And Rumble #1, by Kinoton"),
    "keyboard": ("859/859264_9839964", "CMPTKey_Mechanical Computer Keyboard Typing, Fast Typing, by harrisonlace"),   # an old Dell's keys
}
CACHE = os.path.join(HERE, "..", "build", "sounds")


def source(name):
    """A source recording as samples (two channels, RATE), fetched once."""
    import subprocess
    import urllib.request
    os.makedirs(CACHE, exist_ok=True)
    wav = os.path.join(CACHE, name + ".wav")
    if not os.path.exists(wav):
        mp3 = os.path.join(CACHE, name + ".mp3")
        request = urllib.request.Request("https://cdn.freesound.org/previews/%s-hq.mp3" % SOURCES[name][0], headers={"User-Agent": "Mozilla/5.0"})
        open(mp3, "wb").write(urllib.request.urlopen(request).read())
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", mp3, "-ar", str(RATE), "-ac", "2", wav], check=True)
    with wave.open(wav, "rb") as w:
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, 2).astype(float) / 32768
    return data.T.copy()


def piece(sound, start, end, fade_in=0.01, fade_out=0.05):
    """Seconds `start` to `end` of a recording, its ends faded."""
    cut = sound[:, int(start * RATE):int(end * RATE)].copy()
    t = np.arange(cut.shape[1]) / RATE
    return cut * np.clip(t / max(fade_in, 1e-6), 0, 1) * np.clip((t[-1] - t) / max(fade_out, 1e-6), 0, 1)


def limited(sound, gain, ceiling=0.9):
    """Louder by `gain`; what would pass the ceiling is bent under it, not cut off."""
    return np.tanh(sound * gain / ceiling) * ceiling


def save(name, stereo):
    data = (np.clip(stereo, -1, 1).T * 32767).astype("<i2")
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(data.tobytes())
    print("%-10s %5.1f s  rms %6.1f dBFS  peak %5.1f dBFS" % (name, stereo.shape[1] / RATE, 20 * np.log10(np.sqrt((stereo ** 2).mean()) + 1e-9),
                                                             20 * np.log10(np.abs(stereo).max() + 1e-9)))


def without(sound, idle):
    """A piece of the recording less the machine's steady sound (taken from `idle`): what is
    left is what happened in it. Each band keeps what it has over the idle's level."""
    n, hop = 2048, 512
    window = np.hanning(n)
    floor = np.zeros(n // 2 + 1)
    frames = 0
    for i in range(0, idle.shape[1] - n, hop):
        floor += np.abs(np.fft.rfft(idle.mean(0)[i:i + n] * window))
        frames += 1
    floor /= frames
    out = np.zeros_like(sound)
    for c in range(2):
        for i in range(0, sound.shape[1] - n, hop):
            spectrum = np.fft.rfft(sound[c, i:i + n] * window)
            size = np.abs(spectrum)
            keep = np.clip(1 - 1.6 * floor / (size + 1e-12), 0, 1)
            out[c, i:i + n] += np.fft.irfft(spectrum * keep, n) * window * (hop / n) * 2.67   # a Hann window overlapped four times
    return out


def computer():
    """One recording of one computer, cut up (the times are from tools of measurement, not
    from listening: scan a new source for its beep and its busy half seconds before changing them)."""
    pc = source("computer")
    beep = np.abs(pc[:, int(5.7 * RATE):int(6.0 * RATE)]).max()
    gain = 0.8 / beep   # its beep is the loudest thing it does, bar the switch
    # switched on: the switch at 0.5 s, the beep at 5.75 s, and on to where the loop takes over
    save("PcBoot", limited(piece(pc, 0.3, 12.3, 0.02, 3.0), gain))
    # running: eight quiet seconds, the end laid over the start
    run, lap = piece(pc, 24.0, 33.0, 0.0, 0.0), RATE
    mix = np.linspace(0, 1, lap)
    run[:, :lap] = run[:, :lap] * np.sqrt(mix) + run[:, -lap:] * np.sqrt(1 - mix)
    save("PcRun", limited(run[:, :-lap], gain))
    # the disk at work: the recording's own busy moments, with the steady sound taken out
    idle = pc[:, int(24.0 * RATE):int(33.0 * RATE)]
    for i, (start, end) in enumerate(((41.2, 42.3), (48.8, 49.9), (59.3, 60.4), (63.3, 64.7))):
        save("PcSeek%d" % (i + 1), limited(without(piece(pc, start, end, 0.05, 0.1), idle), gain))
    # switched off: the switch and the run-down, to the recording's end
    save("PcOff", limited(piece(pc, 72.2, 76.1, 0.02, 0.3), gain))


def keys():
    """Four single strokes out of a recording of typing, for the keyboards' keys: the four
    loudest that begin out of quiet, found by measurement (the level in steps of 2 ms)."""
    typing = source("keyboard")
    hop = RATE // 500
    level = np.abs(typing).max(0)
    steps = level[:len(level) // hop * hop].reshape(-1, hop).max(1)
    found = []
    for i in range(20, len(steps) - 45):
        # a stroke: a step much louder than the 30 ms before it, and the loudest for 16 ms either way
        if steps[i] > 3.0 * steps[i - 15:i - 1].max() and steps[i] == steps[i - 8:i + 9].max():
            found.append((steps[i], i))
    found = sorted(sorted(found, reverse=True)[:4], key=lambda stroke: stroke[1])
    for n, (peak, i) in enumerate(found):
        start = (i - 2) * hop / RATE
        cut = piece(typing, start, start + 0.085, 0.002, 0.04)
        save("Key%d" % (n + 1), cut / np.abs(cut).max() * 0.8)
    print("keys: %d strokes found out of quiet, the four loudest kept (at %s s)" % (len(found), ", ".join("%.2f" % (i * hop / RATE) for _, i in found)))


def thunder():
    for name, key, most in (("Thunder1", "thunder_near", 13.0), ("Thunder2", "thunder_peal", 20.0), ("Thunder3", "thunder_far", 19.0)):
        sound = source(key)
        level = np.abs(sound).max(0)
        first = max(0, int(np.argmax(level > 0.03 * level.max())) - RATE // 20)   # it starts when the file does
        cut = piece(sound, first / RATE, min(first / RATE + most, sound.shape[1] / RATE), 0.01, 2.5)
        save(name, cut / np.abs(cut).max() * 0.9)


def main():
    os.makedirs(OUT, exist_ok=True)
    rain()
    thunder()
    computer()
    keys()


if __name__ == "__main__":
    main()
