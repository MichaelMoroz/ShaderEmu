# The sound card

The CPU cannot afford to mix sound: one of Doom's channels at 11 kHz would cost a quarter of a
million instructions a second. So sound is a device with a shader pass of its own, a wavetable
card: a program points a voice at samples that are already in memory, or in the ROM, and the
card mixes. Starting a sound is a few stores.

The host plays what the card mixes: XAudio2 in the harness, an `AudioSource` in the VRChat
world. Its design follows lox9973's [ShaderAudio](https://gitlab.com/lox9973/ShaderAudio).

## How it works

The card's output is a ring of 16,384 stereo samples at the host's own rate (48,000 a second),
drawn into a 128 x 128 target of two floats a pixel (`sound.shader`, `src/sound.h`). A pixel is
a sample: the mix of every voice at that sample.

- The host's audio thread pulls samples from its copy of the ring. How far it has read is the
  clock; nothing is queued, so nothing drifts.
- Every few milliseconds the host names a **cursor** a little ahead of the audio thread, and the
  card draws every sample from the cursor on, as the voices will sound if nothing changes.
  A frame that comes late leaves that prediction playing instead of a gap, and a sound started
  now replaces it at the next mix: latency is the lead, about 20 ms in the harness.
- The GPU device's control pass (`sound_control` in `gpu.shader`'s GPUControl) then moves the
  card's clock and each voice's position up to the cursor. It does that only in a frame the
  host mixed, and only while the guest has the card enabled, so a machine that makes no sound
  is bit for bit what it was.

The CPU's shader and the commit pass know nothing of it.

## Registers

All in the control words' page (`programs/common/sound.h` has them for C). The guest writes
the first group, the device the second, and neither the other's: nothing needs a lock.

| Address | Whose | |
|---|---|---|
| `0x87000220` | guest | enable (not 0: the card mixes) |
| `0x87000224` | guest | master volume, 0..256 |
| `0x87000228` | guest | voices the card looks at, from voice 0 (at most 32) |
| `0x87000230` | device | the clock: output samples so far |
| `0x87000234` | device | output samples a second; 0 until the host has mixed once |
| `0x87000800 + 32v` | guest | voice v: key, kind, address, length, loop, step, volume, aux |
| `0x87000c00 + 32v` | device | voice v: key seen, state (bit 0 playing), position, fraction, and what it latched |
| `0x87b00000` - `0x87e00000` | guest | sound memory: samples, rings, tracks |

A voice starts when its **key** changes to a value with bit 0 set and stops when it changes to
one with bit 0 clear. At that moment the card takes the kind, address, length, loop and aux;
**step** (samples per output sample, 16.16) and **volume** (left | right << 16, 0..256 each)
are read all the time. A voice is playing if the device has not seen its key yet or its state
says so: the device's words are a frame old at best.

| Kind | Samples |
|---|---|
| 1 | unsigned bytes, at any address |
| 2 | signed 16-bit, at an even address |
| 3 | a stream: 16-bit, a ring of `length` samples (a power of two). `loop` is how many the program has written so far, read all the time; the card waits there when it runs dry |
| 4 | IMA ADPCM, four bits a sample. `aux` points at checkpoints, a word for every 32 samples (the decoder's value, and its step index << 16), so any sample is at most 32 steps away |
| 5 | FM: `address` is a track of notes, `aux` a table of two-operator instruments, `length` the tune's in output samples. Envelopes are worked out from how long a note has been on, so a voice keeps no state but its place in the tune |

Flags: `0x100` loop (back to `loop` at the end), `0x200` interpolate, `0x400` stereo (kinds 1 to 3).

Addresses are physical: RAM, or the ROM from `0x40000000`. A file in the ROM is in one piece,
so its samples can be played where they lie.

## From Linux

`/dev/sound` (`linux/kernel/shaderemu_sound.c`; `linux/userland/shaderemu_sound.h` wraps it):

- `mmap` at offset 0 is the registers' page; at `address - 0x87000000` it is sound memory.
- ioctls claim and free voices and hand out sound memory. When a program ends, however it
  ends, its voices stop and its memory is free; the card is off when nobody has it open.
- `write()` plays 16-bit PCM through a stream voice (22,050 Hz mono unless an ioctl says
  otherwise) and blocks while the ring is full: `cat file.raw > /dev/sound`.
- Opening fails with ENODEV on a host with no sound card (`--no-sound`).

## What uses it

- **Doom** (`linux/nanox/doom_sound.c`): an effect is a voice pointed at its lump in the WAD.
  Music: a MUS score becomes tracks for twelve FM voices when the game registers it, with the
  WAD's own OPL instruments (GENMIDI). The pitch wheel is not followed.
- **Quake** (`linux/quake/snd_shaderemu.c`, `docs/quake.md`): a sound is a voice pointed at its
  WAV in the pak file in the ROM, looped where the WAV says; 28 voices (8 for things, 2 for
  water and sky, 18 for the loudest of a level's own).
- **Red Alert** (`docs/ralert.md`): Tiberian Dawn's driver and tool, with a pack of its own
  (`ralert-sound.pak`: 294 sounds, among them the demo's two tunes, 12.8 MB).
- **Tiberian Dawn** (`linux/tdawn/soundio_shaderemu.cpp`): `tools/make_tdawn_sound.py` turns the
  game's AUD files into ADPCM with checkpoints when the image is built (3.8 MB). The game's
  sounds are found in that pack by a hash of how their AUD file begins. A sound out of view
  is played at a tenth of the volume, so a game left to itself is quiet; `TDAWN_SAY=1` makes
  EVA speak. No music: the demo's data has none for its missions, only the map screen's and
  the score screen's tunes (`MAP1.AUD`, `WIN1.AUD`), which the build does not install.

## The host

- Harness: silent unless `--sound` (the terminal mode plays). `--volume P` (default 10), and in
  the window Ctrl+F11 / Ctrl+F12; Ctrl+F9 switches the card off, which costs nothing then.
  `--no-sound` leaves it out of the machine.
- VRChat: `EmuSound` mixes once a Unity frame and reads the ring back; `EmuSoundOut`, on the
  audio source's object, is run by the audio thread (`_onAudioFilterRead`: UdonSharp has no
  event of that name, VRChat's runtime does) and copies from the ring. It must have no
  `Update`. The panel has the switch and the volume; the shader applies the volume.
  Only the player whose machine it is hears it.
  A mix's cursor is never before the last one (`lastCursor` in `EmuSound`): the lead comes down
  eight samples at a time while the audio thread's place stands still for a frame, and a
  cursor that went back made the card move its voices by a negative count, which put every
  voice that does not loop past its end. A looping tone does not show it; test with one that ends.

## Checking it

`tools/sound_reference.py` is the card in Python, in the same integers. A run with
`--fixed-dt 0.004 --sound-capture out.wav --save-state out.snap` writes the samples and, per
mix, the cursor and the control words; the model mixes them again from the snapshot's memory
(and `--rom rootfs.bin`) and must agree on every sample and on the device's words.

    bin\rvc_harness.exe --dxc --image sound --no-stdin --fixed-dt 0.004 --until "sound: " --sound-capture s.wav --save-state s.snap
    python tools\sound_reference.py s.wav s.snap --stream-test

`programs\sound` is the test card: every kind of voice for half a second. From Linux,
`sndtest FILE` plays a tone from sound memory and FILE from the ROM.

## Not done

Sound for players watching someone else's machine. Tiberian Dawn's music and movies. Doom's
pitch wheel, and the second voice of its two-voice instruments.
