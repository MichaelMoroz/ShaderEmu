# Fetch: pages from the host

The machine has no network. A program that wants a page asks the host for it through a few
words of the control block, and the host puts the answer into the machine's memory in one
pass of the GPU device's control shader. `nxweb`, the browser, is the one user.

| Address | Contents |
|---|---|
| `0x87000100` | requests made: the guest adds 1 to ask |
| `0x87000104` | length of the address in bytes |
| `0x87000110` | requests answered: the host sets it to the request's number |
| `0x87000114` | bytes of the answer (at most 262,144; a longer page is cut) |
| `0x87000118` | status: 200 for a page, the HTTP status otherwise; 1: an address this host may not open until its user hands it over (ask again); 3: one it never may; 0 or 2: no answer |
| `0x87000120` | the address, up to 255 bytes |
| `0x876c0000` | the answer, 256 KB |

- The guest writes the address and its length, then the request number. The host reads these
  words back with the rest of the control block (48 texels now), fetches, and in one frame
  sets `_FetchDeliver` with the answer's number, length and status and a 256 x 256 RGBA8
  texture of its bytes (`_HostData`, four bytes a texel in memory order).
- The control pass writes the three answer words and the 0x4000 texels of data in that frame
  (`gpu_control` in `src/gpu.h`). Words and bytes arrive together, so a guest that sees its
  number in the answer word can read the data at once.
- Under Linux a program maps both through `/dev/gpu`: the words at offset `0x01000000`, the
  data at `0x016c0000`. Nano-X's texture area ends below that.
- One request at a time; there is no lock between programs.

## Hosts

- The harness fetches with WinINet on a thread (`rvc_harness.cpp`), any `http` or `https`
  address. Its backends draw a second zone over the data's eight rows in the frame of delivery.
- The VRChat world (`EmuMachine.cs`) uses `VRCStringDownloader`. A world can only load
  addresses it was built with, or one a visitor types into a `VRCUrlInputField`: so it knows
  the sites of `linux/apps/web/sites.txt` (the browser's home page is made from the same
  file) and answers `world:typed` with the address typed on the panel. Any other address
  gets status 1 and is shown in a field on the panel: the visitor copies it into the URL
  field beside it, and the browser, which goes on asking for it every three seconds, then
  gets its page (the world opens a typed address that equals the one asked for). An
  address that is not `https` gets status 3: VRChat throws on those and reports nothing to
  the world, which is also why a request gives up after 20 seconds with status 2. VRChat
  loads from few hosts unless the visitor allows untrusted URLs.

## The browser

`linux/apps/nxweb.c`: text, headings, bold, links, lists, rules, preformatted text, tables as
rows of text. No style sheets, no scripts. A picture is shown only when it is a PPM file in
the image, which the GPU samples from the ROM; any other is its description in brackets.
A page that is a file (`/usr/share/web-*.html`) is read directly. Layout uses the font's
width table, with no round trips to the server, and only lines in view are drawn.
