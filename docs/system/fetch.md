# Fetch: pages from the host

The machine's own network is its LAN and itself (`docs/system/lan.md`); it has no way to the internet.
A program that wants a page from there asks the host for it through a few
words of the control block, and the host puts the answer into the machine's memory in one
pass of the GPU device's control shader. `nxweb`, the browser, asks for pages (and opens `http` addresses
of the machine's own network itself: `docs/apps/web.md`, "Addresses the machine reaches itself"); `open`
(`docs/system/open.md`) for files.

| Address | Contents |
|---|---|
| `0x81000100` | requests made: the guest adds 1 to ask |
| `0x81000104` | length of the address in bytes |
| `0x81000108` | kind: 0 for a page's bytes, 1 for a picture's pixels, 2 for a style sheet's bytes, 3 for a file |
| `0x8100010c` | offset: the part of the answer wanted begins at this byte (0 for the first) |
| `0x81000110` | requests answered: the host sets it to the request's number |
| `0x81000114` | bytes of the whole answer; 262,144 of them at most are handed over at a time |
| `0x81000118` | status: 200 for a page, 201 for a file with its header (below), the HTTP status otherwise; 1: an address this host may not open until its user hands it over (ask again); 3: one it never may; 0 or 2: no answer; 415: a picture the host could not decode |
| `0x8100011c` | a picture's width, and its height from bit 16 |
| `0x81000120` | the address, up to 255 bytes |
| `0x816c0000` | the answer, 256 KB |

- The guest writes the address and its length, then the request number. The host reads these
  words back with the rest of the control block (256 texels in the harness, 64 in the world), fetches, and in one frame
  sets `_FetchDeliver` with the answer's number, length and status and a 256 x 256 RGBA8
  texture of its bytes (`_HostData`, four bytes a texel in memory order).
- The control pass writes the three answer words and the 0x4000 texels of data in that frame
  (`gpu_control` in `src/gpu.h`). Words and bytes arrive together, so a guest that sees its
  number in the answer word can read the data at once.
- A picture is fetched and decoded by the host and arrives as pixels, a word each (red in the
  low byte, sRGB, over white), every pixel of it as it was decoded: nothing is scaled. The
  host sets `_FetchDeliver` to 2 with the picture in `_HostImage` and its size in `_FetchW`,
  `_FetchH` (the answer's fourth word; its length is four bytes a pixel). A part is 65,536
  pixels: the guest asks for the rest with the same address and an offset in bytes, as for a
  file (below), and the host keeps the texture until the last part is delivered (`open.c`,
  and `check_reply` in `nxweb.c`, which writes a picture's parts to its file as they come).
  The guest decodes nothing. The harness alone shrinks a picture to fit 256 x 256 (WIC, 65,536
  pixels at most, `_FetchDeliver` 1). VRChat's own loader gives a world at most 2048 pixels a side.
- Under Linux a program maps both through `/dev/gpu`: the words at offset `0x01000000`, the
  data at `0x016c0000`. Nano-X's texture area ends below that.
- One request at a time; there is no lock between programs. A program waits until the
  request and reply words are equal before it asks.

## Files, and answers longer than 256 KB

- **Kind 3** is whatever the address holds, as bytes, of any length. The reply's length is
  the whole answer's; the guest takes the first 256 KB and asks again with the same address
  and the offset of the next part. The host keeps the answer and downloads nothing twice.
- **A file inside a picture.** A VRChat world gets arbitrary bytes only as a picture, so a
  file can travel as a PNG whose pixels are its bytes, three a pixel (red, green, blue), rows
  from the top. The first 256 bytes are a header:

  | Bytes | Contents |
  |---|---|
  | 0 to 11 | the mark: `SHADEREMU`, 0x1a, `F1` |
  | 12 to 15 | the file's length |
  | 16 to 19 | its CRC-32 |
  | 20 to 255 | its name, ended by zeros |

  Asked for as a file, a picture with the mark is answered with status 201 and those bytes,
  header first (256 + the file's length in all). A picture without it is answered as a
  picture is: pixels, status 200.
- `python tools\file_to_png.py FILE` makes such a picture (and `--back`, `--check`);
  `tools\file_to_png.html` does the same in a browser, with no server. Up to 2048 x 2048
  pixels, 12 MB. The picture must arrive unchanged: a host that scales it or makes a JPEG of
  it destroys the file, and the CRC says so.

## Hosts

- An address that begins `persist:` is no network's: every host answers it itself, from what
  it keeps of the guest's folder `/persistence` (`docs/system/persist.md`). So is `host:name`, the
  player's name (`docs/system/persist.md`, "The player's name").
- The harness fetches with WinINet on a thread (`rvc_harness.cpp`), any `http` or `https`
  address or a file of its own that `--open` named (`file://` and a path; any other gets status 3), up to 16 MB, and decodes pictures with
  WIC: a PNG asked for as a file is looked into for the mark there. Its backends draw a second zone over the data's
  eight rows in the frame of delivery.
- The VRChat world (`EmuMachine.cs`) uses `VRCStringDownloader` for pages and
  `VRCImageDownloader` for pictures (VRChat starts one download of each kind every five
  seconds). A file is asked for through the picture loader when its address ends in `.png`,
  `.jpg` or `.jpeg`, without smaller copies, and the control pass reads the file's bytes out
  of the texture itself (`packed_word` in `src/gpu.h`, `_FetchDeliver` 3, `_FetchOffset`):
  no Udon loop touches them. Any other address is a text download, whose bytes Udon packs
  6,000 a frame. It hands a binary file over unchanged, and the picture loader refuses a GIF, so a
  GIF stays a text download (`EmuMachine.fetchTest` loads `fetchTestUrl` both ways eight seconds
  after start and logs `[FetchTest]` lines). Discord's attachments want their signature
  (`?ex=...&is=...&hm=...`): without it Discord answers 404. A world can only load addresses it was built with, or one a visitor types into a
  `VRCUrlInputField`: so it knows the sites of `linux/apps/web/sites.txt` (the browser's home
  page is made from the same file). Any other address gets status 1 and a slot.
- There are four slots: slot 0 is the page, 1 to 3 what it wants next, its style sheets before
  its pictures, each in the page's order (the browser keeps at most three such requests open,
  and a new page empties all four). A sheet is kind 2: bytes like a page, a slot like a picture. While
  any slot is filled a button shows at the display's lower left; it opens a panel in front of
  the display with four rows, the address to copy and a URL field to paste it into. The
  browser goes on asking every three seconds, and the world opens a pasted address that
  equals the one asked for. A slot nobody asked about for eight seconds is emptied.
- An address that is not `https` gets status 3: VRChat throws on those and reports nothing to
  the world, which is also why a request gives up after 20 seconds with status 2. VRChat
  loads from few hosts unless the visitor allows untrusted URLs.

## The browser

`linux/apps/nxweb.c`, `docs/apps/web.md`: what it does with a page is there. What it asks of the
host is what it cannot reach itself (`net_route` in `web_net.h`): `https`, and `http` to a name nothing here resolves or a
number with no route. Over the host there is no POST and there are no cookies, the words above having no room for either:

- **The page** (kind 0), then its style sheets (kind 2), then its pictures (kind 1), each in
  the page's order, at most three such requests open at a time (the host has that many
  slots). An address is asked for without its `#fragment`, made whole with the page's own or
  its `<base>`, its `./` and `../` folded; one over 255 bytes is not asked for.
- **Text longer than 256 KB comes in parts.** The answer's length word is the whole answer's;
  the browser takes the first 256 KB and asks again with the same address, the same kind
  and the offset of the next part, up to 1.5 MB of a page and 768 KB of a sheet (`part_text`
  in `nxweb.c`). The world (`EmuMachine.cs`) keeps a page whole as it keeps a file.
- A picture's pixels are written as a PPM file in `/tmp`; when it is first drawn it is decoded
  from that file to the size the page draws it at, into the GPU's memory, as the image's
  pictures are (`docs/apps/web.md`, "Pictures"). Its place is kept from the `width` and `height`
  the page gives; until it arrives its description stands there, and when it fails a box that
  says why (the status, `docs/apps/web.md`, "Loading, and what did not come").

## What the image keeps of the network

VRChat lets a world open only the addresses it was built with, and starts one download of
each kind every five seconds: without help each of a listed site's sheets and pictures would be a
paste by hand and five seconds. `python linux/apps/web/preload.py` reads
the sites of `linux/apps/web/sites.txt` as the browser would and writes two things:

- **Addresses for the world**, `build/web/links.txt`: the listed pages, the pages one link
  on from them on the same site, and every sheet and picture those ask for
  (`kind<TAB>address` a line: P, S or I; pictures only as VRChat's loader takes them, PNG or
  JPEG up to 2048 a side). `build/web/links-stable.txt` is the part of it that does not go
  stale in days (no address with a content's hash or a date in it, no story of the hour):
  the world's builder (`ShaderEmuBuilder.cs`) adds it to the sites, and `EmuMachine` finds
  an address among them by halving (they are sorted). A hashed address is right until its
  site next changes its sheet: then that sheet is a paste again, and nothing breaks.
- **Copies in the ROM**, with `--cache DIR` (`build/images/linux/root/usr/share`, the shared root, for the
  image; `--budget BYTES`, 8 MB if not said): the sheets as they came (`webc-N.css`), the
  pictures (any format the host's Pillow reads, SVG too where it can, laid on white, no wider
  than 800) and `web-cache.idx`, a line an address in the order of their bytes: kind, file, a
  picture's size, the address. The browser looks an address up there before it asks the host
  (`web_cache_find` in `web.h`, which halves the index where it lies): a sheet is read as a
  file and a picture is drawn from the ROM, with no host, no paste and no five seconds.
- **A picture is kept** (`kept_file`) as a PPM when it has 4,096 pixels or fewer (`webc-N.ppm`:
  the GPU samples it where it lies, with no decoding on the first core), as a PNG in segments when it has 256 colours or fewer (with a
  palette) or its PNG is no larger than 1.25 times its JPEG (`webc-N.png`), else as a JPEG of
  quality 80 with its colours at full size, in restart intervals (`webc-N.jpg`). Both are laid
  out for libmedia's workers (`docs/system/media.md`); the browser decodes them on the worker cores
  (`docs/apps/web.md`, "Pictures"). Full-size colours, since libmedia stretches half-size ones from
  the nearest sample.
- **What the budget holds**: the pictures are chosen smallest first by a guess of their size
  (`GUESS_BYTES`), then kept smallest first while the budget lasts. `--pages` keeps the pages too, for tests of the browser on real sites in a
  machine without a network (`rvc_cpu`, `tools/web_test.c`); the browser shows a kept page
  and says so, and Reload asks the host.
- Run it before the image and the world are made. What it fetches is not kept in this
  repository.

`/usr/share/web-css.html` and `web-layout.html` are the browser's own test pages.
