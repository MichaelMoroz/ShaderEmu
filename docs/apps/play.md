# An audio player: `nxplay`

A program of the desktop that plays WAV, MOD, FLAC, MP3 and Ogg Vorbis files through the
sound card's stream voice (`sound.md`), decoded by libmedia (`media.md`, "Sound"). WAV and
tracker tunes play as they are decoded. FLAC, MP3 and Vorbis at 44.1 kHz stereo cost more than
the machine has, with every core at work: the player decodes them ahead, a file's pieces on
worker cores (`multicore.md`), and plays when what is left can be decoded while it plays.

    wsl -- bash linux/nommu/build.sh nanox/build.sh     (once)
    wsl -- bash linux/nommu/build.sh libmedia/build.sh  (the decoders; the first time it also fetches)
    wsl -- bash linux/nommu/build.sh play/build.sh
    wsl -- bash linux/nommu/root.sh
    pwsh tools\make_image.ps1                                  (then a new shell snapshot)

In the guest: `nxplay FILE` plays at once; the Start menu's Other has it as "Music player", and
`open` starts it for a sound file (`nxopen.84-play`). The window has the file's name, the
place in the tune (a bar that can be dragged), keys with signs for Previous, Play or Pause,
Stop and Next (`ui_glyph.h`), Repeat, Shuffle, Open..., a loudspeaker key that mutes and the
volume; under them a list of the tunes of the folder the tune is in (`/root/Music` when no
file was asked for), the one played picked out. A click on the picked row, or Enter, plays
it; the tune after it follows by itself when one ends (the list's first after its last only
with Repeat; any other with Shuffle). Space pauses, Left and Right go ten seconds back and
on, the arrows up and down and the paging keys are the list's, `+` and `-` (and the wheel
outside the list) the volume, `m` mutes, `n` and `p` are Next and Previous, `r` and `s`
Repeat and Shuffle, `o` opens a file, `q` or Escape leaves. Without a window system, or with
`NXPLAY_NOWINDOW=1`, it plays the file and leaves.

The list is a playlist of any tunes (up to 512, Title and Length, sorted
by a click on a heading, moved by a drag or Alt+Up and Down), kept in the registry
(`nxplay.list.N`, at most 12 keys of 250 bytes, `nxplay.tune`, `nxplay.at`, the settings), each
with its length, measured while the first core is idle. Files are added in a file window that
is a process of its own (`nxplay --choose`), so the tune plays on. A second `nxplay FILE` gives
the file to the player that is open (`/tmp/nxplay.fifo`). A seek bar, menus (File, Play, List,
Help; F10), Shuffle (every tune once a round), Repeat (off, the list, one tune), and a level
meter that reads the peaks of the ring where the card plays. Previous more than 3 s into a tune
starts it again. Keys: Space, X, N, P, Left, Right, 0 to 9, `+` or `=`, `-`, M, S, R, O, A,
Delete, Alt+Up and Alt+Down, F1, F10, Q. `NXPLAY_PRINT=1` prints where the menus, keys, sliders
and rows are.

## What the build is made of (`linux/play`)

- `nxplay.c`: the window, the pieces and the ring, the card. It links
  `~/nm/work/src/libmedia/libmedia.a` (`-Wl,--gc-sections`: the pictures' decoders
  are left out).
- The decoders are libmedia's (`linux/libmedia`, `media.md`): the tracker (`m_audio_mod.c`: 31 samples; 4 channels, or the tags `6CHN`,
  `8CHN`, `nnCH`; mixed at 22,050 Hz, which the card brings to its own rate; XM is not
  played), [dr_flac](https://github.com/mackron/dr_libs) with `flac_bound.py`,
  [minimp3](https://github.com/lieff/minimp3) compiled twice, as it is and at half rate
  (`minimp3.patch`: the lower 16 of the 32 bands through the transform, every second sample
  out of the filter, and frames given for their bits only, `MP3D_SKIP`), and
  [stb_vorbis](https://github.com/nothings/stb). Everything that has floats is built with
  `-fsingle-precision-constant`: minimp3 compares each sample with `32766.5`, a double
  otherwise, and the machine has no doubles.
- `make_test.py` (on the host, with ffmpeg) makes `test/play-test.*`, which the build puts
  in `/usr/share`: a sweep with noise as WAV (1.6 s), FLAC (2 s) and MP3 (18 s, 128 kbit/s),
  all 44.1 kHz stereo, and a four-channel MOD of 31 s written by the script.
- `check_sum.py` works out a WAV file's sum on the host.

## How it plays

A tune is decoded in **pieces** of about half a second, and a piece is decoded where it will
lie in the stream's ring (262,144 frames of sound memory, six seconds at 44.1 kHz; 65,536
when sound memory has no megabyte left). The stream's frames are the pieces' in order, so a
piece's place is known before it is made (`piece_place`); when it is its turn the card is
told it is there (`feed`), and nothing is copied. The voice is 16-bit, mono or stereo, at
the file's own rate: the card resamples. A step of 0 holds the voice (pause, and while more
is decoded). While it waits to play, the panel says how much of what it waits for is decoded
("(decoding ahead: 40%)"), drawn again when that changes.

- A core keeps about 6 KB of new stores a pass of the machine (`docs/machine/boot.md`, "Passes that
  end early"), so a piece made elsewhere and copied into the ring would be several passes ended
  early.
- A piece has no place in the ring when it would go round the ring's end, when the ring is full
  of sound not yet heard, or when there is no card: it is decoded into memory of its own and
  copied. With workers a piece
  the ring has no room for waits instead (there are seconds in hand then).
- A worker stores into the ring itself. A piece's bytes are a multiple of 16 whatever the
  file (a frame of 576 or 1,152 samples, one or two channels, whole or half rate), so two
  cores never store to the same 16 bytes; the ring's pages are touched before the first job.
- After going elsewhere in the tune no piece is given out until the workers have dropped
  theirs (`dropping`): a dropped piece's place is another piece's in the new stream.

- WAV and MOD are decoded in order by the first core, a slice at a time between two looks at
  the window; so is a FLAC file with no workers, or one whose blocks are not all as long.
- A FLAC file's blocks stand by themselves, so a piece is whole blocks (one of 4,096 frames
  with twelve workers or more, else half a second) and any core's (`flac_job`; `flac.c` from
  "its blocks"). It asks for 24 workers; each has a decoder of its own, made by the first
  core when the worker gets its first piece. A block's frame has no length in the file: the
  first core finds where blocks begin by their headers (the two sync bytes, the block's
  number, the header's check byte), a word of the file at a time, as far ahead as pieces are
  given out, and after going elsewhere by guesses at where a block lies (a header found
  there says which block it is, and counts when the next one follows it).
- An MP3 file is cut where its frames begin (a table made when it is opened). A piece is 20
  frames, and any core can decode any piece: it starts 12 frames early, gives the first eleven
  for their bits only and the last for what it leaves (`MP3D_WARM` in `minimp3.patch`: a
  granule's overlap comes of its own coefficients, and the filter's past is the last
  granule's transform, so the two granules go as far as that and no samples are made), after
  which its samples are those of a decoder that began at the file's start.
- With eight workers or more the first core takes no piece: it gives them out, and is back at
  the window in a turn (`OWN_UNDER`; `NXPLAY_OWN=1` has it take pieces too).
- With 24 workers or more (it asks for 48, `NXPLAY_WORKERS`) a piece is 4 frames, not 20, and 44.1
  kHz stereo plays at its full rate with no gap. What sets the time to the first sound is how long
  one piece takes a worker, not how many there are. It asks for 48 and not all 255 for that first
  sound's sake.
- The player asks for its workers when an MP3 file is opened and gives them back with the tune. A
  job is one piece: its decoder is on the worker's stack, its samples go into the piece's own
  place, and it says how far it is in 16 bytes of its own, which is how the speed is known before
  a piece is finished. With fewer than eight workers the first core takes pieces as well. No
  workers: the first core decodes in order, with one decoder and no early frames.
- It measures how much sound a second of decoding makes. Fast enough: it plays with half a
  second in hand and decodes a few pieces ahead. With workers it says nothing about speed
  from the first pieces: they are being started then. It plays when one and a half seconds are ready, and only a ring that has run
  dry twice sends an MP3 file to half the rate. With no workers and too slow: it says so,
  decodes ahead as far as memory goes (half of what is free), and plays once the rest will
  be decoded by the time it is needed. A ring that runs dry holds the voice ("it ran dry:
  waiting for more to be decoded").
- Its window is there before the file is opened ("opening it..."), as `nanox.md` has it.

## Variables

| | |
|---|---|
| `NXPLAY_EXIT=1` | leaves when the tune ends |
| `NXPLAY_SECONDS=N` | plays only the first N seconds |
| `NXPLAY_NOWINDOW=1` | no window: plays and leaves |
| `NXPLAY_WORKERS=N` | asks for N workers (48); 0: this core alone |
| `NXPLAY_SUM=1` | prints a sum of every sample in the order played (FNV-1a, a 16-bit sample at a time) |
| `NXPLAY_HALF=1`, `0` | an MP3 file at half rate from the start, or never |
| `NXPLAY_PIECE=N` | MP3 frames a piece (20) |
| `NXPLAY_MODRATE=N` | the rate a tracker tune is mixed at (22050) |
| `NXPLAY_MEMORY=N` | megabytes of decoded sound it may keep |
| `NXPLAY_VOLUME=N` | 0 to 256 (200) |
| `NXPLAY_OWN=1` | the first core takes pieces too, whatever the workers |

It prints when it starts to play (`nxplay: plays from ...`), and at a tune's end a
`playstat:` line: the sound decoded, the time, and the instructions a second of sound cost
(`rdcycle` round the decoding on this core and in each job).

## What a second of sound costs

Thousands of instructions for a second of sound, by `playstat` on `rvc_cpu` with the test files, 31
October 2026:

| | thousand instructions | |
|---|---|---|
| WAV, 16-bit stereo | 99 | a copy into the piece |
| MOD, mixed at 22,050 Hz | 1,124 | the first core |
| FLAC, 44.1 kHz stereo | 11,579 | the first core; 12,853 on 24 workers |
| MP3, 44.1 kHz stereo | 12,640 | in order, the first core; 14,009 on 48 workers |
| MP3 at half rate | | 9,573 on 48 workers |
| Ogg Vorbis, 44.1 kHz stereo | 15,560 | the first core; 18,103 on 48 workers |

Under the calibrated clock with a sound card (31 October 2026; the model's workers term rests on
one game, so these are rough), with no window: `play-test.mp3` plays 678 ms after it is opened, on 48
workers, and `play-test.flac` 386 ms, on 24.

- **WAV and MOD play as they are decoded** on one core.
- **FLAC's blocks and MP3's and Vorbis's pieces are the workers'.** MP3 at 44.1 kHz stereo costs more
  than one core has; half rate is what a ring that has run dry twice falls back on.
- **The tracker's mixer** (`mod_mix4` in `linux/libmedia/m_audio_mod.c`): a tune of four channels is
  mixed a frame at a time, the four samples added in registers and stored once, for as long as no
  channel comes to its sample's end.
- A FLAC subframe is predicted with 32-bit sums whenever its own coefficients allow it
  (`flac_bound.py`, applied to the fetched `dr_flac.h` by the build): the sum of the coefficients'
  sizes times the largest sample bounds every partial sum, and under 2^31 the samples are the same.
- The decoders and `nxplay.c`, with the tracker's mixer, are `-O3 -fno-tree-vectorize
  -fsched-pressure -funroll-loops`. A warm MP3 frame is a function of its own: a test in the filter's
  loop would slow every frame decoded, in order too.
- Where the decoders' instructions go is minimp3's filter window and transform, and dr_flac's Rice
  codes and predictor.

## The pieces

The player asks libmedia for a tune (`media_audio_open`) and decodes it either in order on
the first core (`media_audio_read`, a slice between two looks at the window: WAV, MOD, and any
kind without workers) or in the library's pieces: an MP3, Ogg Vorbis or FLAC file's, given to
the workers, each into its place in the ring. A job (`piece_job`) is `media_audio_piece_step` until
the piece is done or the player drops it: one step is an MP3 frame, a FLAC block or 4,096 frames of
Vorbis, and how far it is goes in the slot's head, which is the speed's measure. The decoders are
made when the tune is opened, one a worker and one for the first core, which takes pieces with
fewer than eight workers. A Vorbis piece is half a second with 24 workers or more, else a second:
each begins with a seek. At half rate the library's frames are the half rate's: the rate on the
window and in `playstat` is the one played (22,050 Hz for a 44.1 kHz file).

`RVC_RACES` over the Vorbis file and the FLAC and MP3 test files must leave nothing undone. `rvc_cpu`
has a sound card only with `RVC_SOUND=1`: without it a tune is decoded and ends at once, so the keys
and seeks are tried there or on the card.

## Checking it

Under `rvc_cpu`, a few seconds each (start it from PowerShell):

    bin\rvc_cpu.exe --quiet --uart-log logs\play_x.log --expect "/ # " --send "NXPLAY_SUM=1 nxplay /usr/share/play-test.mp3; NXPLAY_SUM=1 NXPLAY_WORKERS=0 nxplay /usr/share/play-test.mp3; echo LX-''DONE\n" --until "LX-DONE"

| File | Sum |
|---|---|
| `play-test.wav` | `e5fd8f8c` of 141,120 samples; `python linux\play\check_sum.py linux\play\test\play-test.wav` says the same |
| `play-test.flac` | `1a14a562` of 176,400: the same script on ffmpeg's decoding of the file to WAV |
| `play-test.mod` | `98e66905` of 1,354,752 |
| `play-test.mp3` | `809816e7` of 1,594,368, with any number of workers and with none |
| the same with `NXPLAY_HALF=1` | `9efedb8f` of 797,184 |
| `Grieg - Hall of the Mountain King.ogg` | `34eeed95` of 4,410,000 with 48 workers, `01ba4206` with none (they differ: `media.md`, "Sound") |

- The MP3 sums are also what the same two files compiled on the host give (x86-64, gcc):
  the machine's float instructions under `rvc_cpu` decode as the host does.
- **On the shader machine an MP3 file's sum is another**: its floats are the card's (`fpu.md`). What must
  hold there is that the sum with workers is the sum with `NXPLAY_WORKERS=0` (`NXPLAY_SECONDS=3`), also
  after an `nxplay` that had them was ended by `kill -9`.
- The card: `NXPLAY_SECONDS=1 NXPLAY_EXIT=1 nxplay /usr/share/play-test.wav` with
  `--fixed-dt 0.004 --sound-capture F.wav --save-state F.snap`, then
  `python tools\sound_reference.py F.wav F.snap --rom build\images\nommu\rootfs.bin`: every
  sample and device word matches. (A second fits the ring, so the snapshot still has it.)
  The same holds for a second of the MP3 and of the MOD file decoded in the ring.
- The window: `nano-X -p & sleep 3; nxplay /usr/share/play-test.mod & sleep 4; nxkey right`
  must print `plays from` a second time, ten seconds on. A click of `nxkey` is only let go
  when the host's pointer moves: with `--pointer-sweep 0`, `nxkey 370,101` (the bar, three
  quarters along) prints `plays from` some 23 s into the tune and `nxkey 130,128` (Stop) goes
  back to 0. The list: two copies of the test tune in a folder, `nxplay` on one, `nxkey n`
  and `p` each print a `playstat` line (the tune left) and play the other;
  `NXPLAY_SECONDS=3` makes one end, and the other follows. The keys by the pointer, the
  window being the server's first, at 480 x 420: the key row is at y 114 of the window, its
  centres Previous 23, Play 57, Stop 91, Next 125, Shuffle 183, Repeat 260, mute 346; the
  volume runs from x 362 to 436, the seek bar from 52 to 428 at y 88, the rows from y 154
  (`NXPLAY_PRINT=1` says where they are on the screen), `wheel:300,60:1` `volume 144`; the wheel over the list
  (`wheel:200,250:-1`) prints nothing.

## Not done

XM. Playing a WAV file where it lies in the ROM (the card can; a stream keeps one way for every
kind).
