# Fetch: pages from the host

The machine has no network. A program that wants a page asks the host for it through a few
words of the control block, and the host puts the answer into the machine's memory in one
pass of the GPU device's control shader. `nxweb`, the browser, is the one user.

| Address | Contents |
|---|---|
| `0x87000100` | requests made: the guest adds 1 to ask |
| `0x87000104` | length of the address in bytes |
| `0x87000108` | kind: 0 for a page's bytes, 1 for a picture's pixels, 2 for a style sheet's bytes |
| `0x87000110` | requests answered: the host sets it to the request's number |
| `0x87000114` | bytes of the answer (at most 262,144; a longer page is cut) |
| `0x87000118` | status: 200 for a page, the HTTP status otherwise; 1: an address this host may not open until its user hands it over (ask again); 3: one it never may; 0 or 2: no answer; 415: a picture the host could not decode |
| `0x8700011c` | a picture's width, and its height from bit 16 |
| `0x87000120` | the address, up to 255 bytes |
| `0x876c0000` | the answer, 256 KB |

- The guest writes the address and its length, then the request number. The host reads these
  words back with the rest of the control block (48 texels now), fetches, and in one frame
  sets `_FetchDeliver` with the answer's number, length and status and a 256 x 256 RGBA8
  texture of its bytes (`_HostData`, four bytes a texel in memory order).
- The control pass writes the three answer words and the 0x4000 texels of data in that frame
  (`gpu_control` in `src/gpu.h`). Words and bytes arrive together, so a guest that sees its
  number in the answer word can read the data at once.
- A picture is fetched and decoded by the host and arrives as pixels, a word each (red in the
  low byte, sRGB, over white), scaled to fit 256 x 256 and 65,536 pixels. The host sets
  `_FetchDeliver` to 2 with the picture in `_HostImage` and its size in `_FetchW`, `_FetchH`;
  the control pass does the scaling. The guest decodes nothing.
- Under Linux a program maps both through `/dev/gpu`: the words at offset `0x01000000`, the
  data at `0x016c0000`. Nano-X's texture area ends below that.
- One request at a time; there is no lock between programs.

## Hosts

- The harness fetches with WinINet on a thread (`rvc_harness.cpp`), any `http` or `https`
  address, and decodes pictures with WIC. Its backends draw a second zone over the data's
  eight rows in the frame of delivery.
- The VRChat world (`EmuMachine.cs`) uses `VRCStringDownloader` for pages and
  `VRCImageDownloader` for pictures (VRChat starts one download of each kind every five
  seconds). A world can only load addresses it was built with, or one a visitor types into a
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

`linux/apps/nxweb.c`: text, headings, bold, links, lists, rules, preformatted text, tables as
rows of text. No scripts. Style sheets in part (below). A picture's pixels from the host are written as a PPM file in `/tmp` and drawn by
the GPU from that file, as one in the image is from the ROM. Its place is kept from the
`width` and `height` the page gives, so the page is laid out once; until it arrives, and when
it fails, its description stands there in brackets.
A page that is a file (`/usr/share/web-*.html`) is read directly. Layout uses the font's
width table, with no round trips to the server, and only lines in view are drawn.

## Style sheets

`linux/apps/css.h`. The page's `<style>` text and up to four linked sheets (256 KB each, the
host's limit; an address over 255 bytes is not asked for) are read into rules, kept by the last
step of their selector, so an element only meets the rules that name its tag, id or classes.
Every sheet is read again when another arrives: a rule may use a custom property a later one sets.

| Understood | |
|---|---|
| Selectors | tag, `.class`, `#id`, `*`, descendants and `>` children, lists with commas, `:root`, `:link` |
| Left out, with their whole rule | `:hover` and other states, `[attributes]`, `+` and `~`, `:not()`, nesting |
| Properties | `display` (none, block, inline), `color`, `background` (a colour), `font-weight`, margins, padding, `width`, `max-width`, `text-align`, `text-decoration`, `border` (and top, bottom), `list-style`, `white-space: pre`, `visibility` |
| Values | px, em, rem, pt, %, `auto`; `#rgb`, `rgb()`, `hsl()`, names, `light-dark()`; `var()` with fallbacks; `!important` |
| At-rules | `@media` with widths, `screen`, `print`, `prefers-color-scheme` (light); `@supports` and `@layer` are read as if true |

- Lengths are scaled by 13/16, our font against the 16 pixels pages are written for, and a
  vertical margin or padding is at most two lines. Media queries are answered with the page
  area's real width (about 570), so sites give their narrow layout.
- Nothing goes side by side: flex, grid, floats and table cells come one under the other or in
  a line. A width is kept only when it leaves at least half of its container.
- `position: absolute` with a clip is taken as hidden (text meant for screen readers).
- An element that is not shown and has no closing tag (`li`, `td`, `p`) ends where the next
  one begins. The old `bgcolor` attribute is a background.
- `/usr/share/web-css.html` is the test page. `NXWEB_TIME=1 nxweb ...` prints what reading the
  sheets and laying out cost: lobste.rs, 207 rules, 0.65 s and 1.15 s; without sheets 0.76 s.
- To try the engine on a real sheet without the machine, compile `css.h` natively with a small
  `main` under `-fsanitize=address`; it has no part of Nano-X in it.
