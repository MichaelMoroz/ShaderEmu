# The video player

`nxvideo` plays an MPEG-1 file (video, with layer II sound), a Westwood VQA (Red Alert's
movies), Motion JPEG in AVI (with PCM sound) or a GIF animation on the worker cores. The films are libmedia's (`docs/system/media.md`,
"Films"): MPEG-1's decoder is
[pl_mpeg](https://github.com/phoboslab/pl_mpeg) (Dominic Szablewski, MIT), fetched by
`linux/libmedia/build.sh` at one commit, and `linux/libmedia/m_video_mpeg.c` arranges it for the
workers; `linux/video/nxvideo.c` is the player.

    / # nxvideo                 # the image's sample, again and again
    / # nxvideo FILE.mpg        # or FILE.vqa, FILE.avi, FILE.gif
    / # nxvideo --decode FILE   # no window: every picture decoded on this core, with sums

    wsl -- bash linux/nommu/build.sh video/build.sh     # after Nano-X's; then the image
    python tools\make_video.py [WIDTHxHEIGHT [SECONDS [KBITS]]]   # the sample, on the host
    python tools\make_avi.py --sample [SECONDS]                   # its MJPEG twin, from the same trailer

## How a film is shared out

One core cannot play it: a picture of the sample (320 x 176) with its share of the sound is 1.8
million instructions (`nxvideo --decode`, `rvc_cpu`, 31 October 2026), which is 43 million a second
at 24 pictures.

- **A group of pictures is a job.** The file must be encoded in closed groups with no B
  pictures (`-g 6 -bf 0 -flags +cgop`): a group then needs nothing outside itself. The first
  core takes the file apart once (the two streams, where each group begins, where each sound
  frame begins); after that a worker decodes a whole group into memory of its own.
- **A decoder is used again.** pl_mpeg's decoder is put back as it was made (a copy of its
  struct, its frames' memory kept) and pointed at the next group's bytes. A `sed` in the build
  stops pl_mpeg moving a stream's bytes down as it reads them: here the stream is every core's to
  read (the other changes are `plm_window.py` and `plm_fast.py`, below).
- **A frame is three planes of bytes**, and the GPU makes the colours (fragment mode 6,
  `gpu.md`): a worker stores a byte and a half a pixel, not four, and nothing converts.
- **Two stages.** A texture has a physical address, so frames to show are in GPU memory, and
  that holds 88 of the sample's (the 3.25 MB a program has, and the display's spare
  framebuffer). Twenty groups at once would not fit. So a worker decodes into ordinary memory
  and, when the frames before its group's have been shown, the group is copied into its
  places, a picture a job (`copy_one` in `m_video_mpeg.c`), by its own worker and by every worker
  whose group waits for its places. A group must be there within the time its places are free,
  which is the room less a group.
- **The sound is chunks.** Fourteen frames to a chunk, each chunk a job. A chunk decodes the
  frame before its first too and throws it away (the filter's past), and starts the filter's
  ring where sequential decoding would have it (`v_pos`): the samples are then the same to
  the last bit as the stream decoded in one piece.
- **The clock is the sound card's.** Decoded sound goes into a stream voice's ring
  (`sound.md`, as `nxplay` does) and the picture shown is the one the card has got to. A
  picture that is late is left out; when none is ready the sound waits.

With every worker the machine has, `nxvideo` takes 16 for pictures of the sample's size and
8 for the sound: 24 pictures a second need about 10, and more decoders are only more memory
to bring in before it starts (each is half a megabyte). Larger pictures get more decoders as far
as 32 MB of their groups go (`VIDEOS` 48 in `m_video_mpeg.c`). It starts some seconds after its
window: the first groups are being decoded. It plays on when the groups being decoded will be
there in time, each going on as fast as it has got to where it is (`in_time`; `NXVIDEO_START=1`
waits for 36 pictures in hand instead).

## What it costs

The sample on `rvc_cpu`, 31 October 2026: `nxvideo --decode` is 876 million instructions, on one
core; played, the first core runs 66 million and all cores 765 million, with 24 workers. Under the
calibrated clock (31 October 2026; the model's workers term rests on one game) it plays through
with 0 pictures left out.

- **The first core only looks after the others.** It asks the window system when the button or
  key words change, for a tenth of a second after, while a button is down, and else seven times a
  second; `ready()` goes on from where it stopped; the worker library looks before it takes a
  mailbox.
- **And then it rests** (`mcw_rest()`, `multicore.md`, "A core that rests"): a turn of its
  loop with no picture to show ends in the kernel. A picture is shown without waiting for
  the GPU to have drawn it (`seglSwapEarly()`; no group is copied into the shown picture's
  place, and `play_from()` settles first), and the sound goes into the ring 4 KB a turn while
  the card has an eighth of a second in hand (a chunk at once is more than a pass's stores).
- **The workers' passes** often end on a full write cache: a group copied into GPU memory is
  nothing but stores, and so is a block copied from the picture before. It costs the film nothing
  while there are more decoders than keep up.
- **Opening**: the stream's start codes are looked for in one pass, reading one byte in three (a
  byte over 1 where a code's 1 would be rules out the two places after it), with its notes of the
  headers and pictures.
- The film's clock between the sound card's steps is the kernel's (`clock_gettime`), a
  system call a turn.

## The decoder's loops

- `linux/libmedia` compiles the decoder `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops`.
- **Bits and codes from a word of the stream** (`plm_fast.py`, applied to the fetched
  `pl_mpeg.h` after `plm_window.py`). pl_mpeg reads a variable-length code by walking its
  table a bit a turn, each bit a call. Here four bytes are taken once and the same table walked on
  them in a register; a coefficient's code, its sign and the block's end come from one such word.
- **The transform only where a block has coefficients.** A column with none below its first
  row comes out as that row's value eight times, and a row with none beside its first
  likewise: the block's decoder notes which columns have any (`cols`, `tall`), and the
  transform fills those without the arithmetic; with none below the first row at all, one row
  is transformed and copied seven times.
- **A picture is decoded where it is kept** (`decode_into` in `m_video_mpeg.c`): pl_mpeg's
  `frame_current` is pointed at the picture's place in the group's memory before each
  picture, and the next one is predicted from it there. Two things pl_mpeg's three pictures did by
  the way are done by hand, for the same bytes: a block whose vector points outside the picture is
  not predicted and keeps the picture three before (`stale`), and so do the macroblocks after the
  last one a picture's slices came to.
- A group's pictures lie in pl_mpeg's order of planes (red chroma before blue), and
  `copy_one` copies them in the frame's: a block at a plane's edge is predicted from the
  bytes past it.
- **The sound's synthesis window** adds four blocks of products a turn with the sum in a register
  (`plm_window.py`): the same additions in the same order, so the same samples.

Decoders are all made before the first job: bringing memory in on the first core holds up the
workers that are running. Their buffers are touched, not cleared.

## The window

The strip has Play, Stop, Previous, Next, a seek bar showing what has
played, the time (a click shows the time left), mute, the volume, Open... and a menu key; the
right button or Shift+F10 opens the same menu (Open, Recent, Close, Repeat, Full window, Time
left). F or two clicks on the film fill the screen with it and Escape goes back; Page Up and
Down go a minute, 0 to 9 a tenth of the film, `.` a picture while paused, `n` and `p` the next
or previous film of the files given or of the film's folder, Ctrl+W closes it. The window is
resized by its corner and keeps the film's shape. Recent films, the volume, mute and repeat
are registry keys (`video.recent.N`, `video.volume`, `video.muted`, `video.repeat`). A file
that cannot be played is said so in a window and the player stays, empty (`nxvideo --none`),
with its recent films to click.

The seek bar is clicked or dragged to go elsewhere; Open is the file chooser, in `/root/Videos`.
Stop waits at the film's start. A click on the film pauses and plays on,
the wheel and the arrows up and down are the volume, `m` mutes, `s` stops, End goes to the
last picture, and going elsewhere while paused shows the picture there once it is decoded.
`NXVIDEO_PRINT=1` prints the volume at every change, for a test: `nxkey drag:535,426:570,426`
on the first window's slider prints it rising to 249, `wheel:300,200:1` 256; after `s` the
second-a-line `nxvideostat` says `picture -1` for two seconds and then `picture 0` at
`film at 0 ms` until Space.
Keys: Space pauses, Left and Right go five seconds back and on, Home to the start, `o` opens
another file, `q` leaves. The picture is a window of its own inside the program's, which is
what the GPU draws into; frames a second are in the caption. Made larger (the caption's box,
Ctrl+F10) the film is as large as fits above the strip, in the middle with black round it
(`fit` in `nxvideo.c`: the picture's window is moved and resized, and the GL library asked
where it is now). A file that is not
played is said to be so in a window (`refuse`): an AVI film's pictures by their codec
("MPEG-4 Part 2 (FMP4) pictures: not played (this plays MJPEG in AVI)", `not_mjpeg` in
`m_avi.c`), in the strip and in a question window over the player's.

- **The window fits the desktop.** The film is shown three times, twice or once as large, and a
  film larger than the desktop as large as fits, its shape kept. `NXVIDEO_SCREEN=WxH` stands for a smaller desktop and
  `NXVIDEO_PRINT=1` prints the sizes (`NXVIDEO_SCREEN=300x260`: the sample at 280 x 154).
- **One player draws at a time; a second start gives it the file.** The GPU's memory for programs
  is one program's (`docs/machine/gpu.md`). Open... runs the player again with the new file (an
  exec), so `gles.c` and `mcw.c` open the device close-on-exec. A second `nxvideo FILE` while a film plays writes the file to `/tmp/nxvideo.fifo`, which
  the player reads when it asks the window system for events, and goes ("nxvideo: given to
  the player that is playing"); the player plays it as Open... does, and with no file comes
  to the front. A start while another program draws (a game) is refused in a window before
  any worker is taken from that program (`gpu_taken`), and a game started while a film plays
  says so in a window (`nxask`, from `gles.c`'s `draw_claim`) and leaves the film playing.
- **Larger films.** MPEG-4 Part 2 (Xvid's kind, `FMP4`) would want a decoder of its own and, at
  960 x 540, more instructions and GPU memory than the machine has for it: not done. MPEG-1 at
  864 x 480 is killed for memory by the kernel before its first picture (its decoders' groups).

- The window is there before the file is taken apart (`ui_framed`, `nanox.md`): two seconds
  after the program is started, with "opening it..." in the strip. While the first pictures
  are decoded the strip says how far, every 0.25 s: the pictures in hand of those it waits for,
  and the seconds ("getting the first pictures ready: 40%, 1.2 s").
- Going elsewhere starts at the group of pictures the place is in. What was decoded is
  dropped, and the film goes on when a group has been decoded again, which is what one group
  takes one worker. The strip says so meanwhile.

## Checking it

(`NXVIDEO_SUM=1` has the first core sum every picture: on the shader machine the film then
leaves most pictures out. Look at "left out" in a run without it.)

- `nxvideo --decode FILE` under `rvc_cpu` prints a sum of all pictures and of all sound, and
  what each costs. libmedia's build makes `media_test-host`, whose `video FILE` must print the
  same sums. The sample's: pictures `d124dc14`, sound `96a296a2`; Red Alert's ENGLISH.VQA:
  `55d71396`, `a5e19ed4`; `Big Buck Bunny.avi`: `3129e37f`, `6dd01fde`.
- `NXVIDEO_SUM=1 NXVIDEO_ONCE=1 nxvideo` prints one sum of the pictures as they reached GPU
  memory, the same as `--decode`'s: `d124dc14` for the sample, with `NXVIDEO_WORKERS=0`, `3`
  and unset; `3129e37f` for `Big Buck Bunny.avi` with 64 workers.
- `NXVIDEO_HOLD=100 nxvideo` stops at a picture (`nxvideo: holding`): with `--save-state`
  and `--gpu-capture`, `tools\gpu_reference.py SNAP BMP --size 640x352` must find every pixel
  within 2 levels.
- `nxvideostat:` lines, one a second: the picture shown, how many were left out, how many are
  ready, how many workers are busy. The sample plays with none left out.
- `nxvideo & sleep 9; nxkey q; sleep 4; pidof nxvideo` must find none, and `mctest 255 20000`
  must pass after it.
- `nxvideo & sleep 12; nxkey right; sleep 6`: the `nxvideostat:` lines' "film at" goes five
  seconds on, stands for some seconds, and counts again.

## The sample

`/root/Videos/Big Buck Bunny.mpg`: the first 20 seconds of the trailer of Big Buck Bunny,
(c) copyright 2008, Blender Foundation / www.bigbuckbunny.org, Creative Commons Attribution
3.0; 320 x 176, 24 pictures a second, 895 KB. `tools/make_video.py` fetches the trailer from
Wikimedia Commons and encodes it; the original is not kept in the repository.

`/root/Videos/Big Buck Bunny.avi`: its first 4 seconds as Motion JPEG (`tools/make_avi.py
--sample`): 96 JPEGs of Pillow's at quality 70 in some 128 restart intervals each, and the
sound as 22,050 Hz mono PCM; 614 KB. A picture is shown as the GPU's words (`GL_RGBA`) where
libmedia decoded it. A picture with its sound is 4.7 million instructions (`nxvideo --decode`,
`rvc_cpu`, 31 October 2026), and it does not play at 24 pictures a second on the machine
(`docs/system/media.md`, "MJPEG in AVI").

## Not done

- Pictures much larger than the sample at full rate, for the workers and the GPU memory they want.
- B pictures, and groups that are not closed.
