# The browser

`nxweb` (`linux/apps/nxweb.c`) shows pages of the web as they are written: HTML read into a
tree, style sheets, text in the fonts of the ROM (`docs/system/fonts.md`), tables, boxes side by
side, floats, forms. It runs no scripts ("Scripts" below). An `http` address the machine can reach
itself it opens over Linux's sockets ("Addresses the machine reaches itself" below); every other
goes through the host (`docs/system/fetch.md`).

    nxweb [ADDRESS or FILE]

A menu bar, a bar of keys with the address, the page, a status line ("The window" below).
A click in a text field types into it and Enter sends its form; a click on a `<summary>`
opens and closes what it folds away.

## The window

- **Menus** (`ui_menu.h`, made when they open: `menu_make`). File: Open address, Open file,
  Save page as, Close. Edit: Copy the address, Paste an address, Find in page, Find next.
  View: Stop, Reload, Pictures (a tick), What was not loaded, Page source (the page's bytes in
  the editor), Options. Go: Back, Forward, Home, then the session's pages (16 at most, the newest first, a
  tick at the one shown). Bookmarks: Add this page or Remove this page, Remove (a menu of
  them all), then the bookmarks. Help: Keys (`about:keys`), About this machine. A row that
  cannot apply is grey. F10 opens File; Left and Right go to the menu beside.
- **The bar**: Back, Forward, Reload (Stop while something is still to come), Home, Bookmark
  (pressed in when the page has one; a click adds it or takes it away), then the address. A
  click, Ctrl+L or F6 opens the address shown selected: a typed character replaces it, a
  second click or Backspace edits it; a long one shows its end. What is typed may be a path, an address
  with its scheme, `10.0.0.11`, `localhost:8080/x` or a word with no dot or a name `/etc/hosts` has
  (a machine of this network, by `http`), or a name with dots (a site, by `https`). A line with spaces in
  it stays in the field and the status line says why: the browser has no search of its own. `nxweb ADDRESS` takes the same.
- **The status line**: what the browser has to say (where the page came from, a link picked
  with Tab, what a command did, what Find is told), and in its right part what is still to
  come ("3 of 8", "Loading...", "Loading" below), how many of the page's pictures and sheets
  did not come ("5 not loaded") or the page's size. It does not follow the pointer: that
  would be the pointer's moves all the time (`ui_moves` in `ui.h`). The right button on a
  link shows its address there, with a menu: Open the link, Copy the link's address,
  Bookmark the link, and the page's Back, Forward, Reload, Bookmark, Page source.
- **Find** looks through the page's texts as they are drawn, without regard to case, from the
  view's top and round again; the place is shown white on blue and scrolled to. What is
  sought must lie in one piece of text (one line, one face).
- **Options** (View, Options): the home page's address (typed, or "This page", or "The first
  one") and whether pictures are loaded; without them a page says what each would be (its
  `alt`), and is laid out anew at once.
- **Kept** in the desktop's registry (`registry.h`, `/persistence/registry`, `docs/system/persist.md`):
  `web.home`, `web.pictures` (0 or 1), `web.bookmarks` (how many) and `web.bookmark.0` on,
  each `ADDRESS TITLE`. 24 bookmarks at most (a menu of them fits the screen), no address
  with a space or over 180 characters. Where `/persistence` cannot be written the status
  line says so and the browser goes on with what it has until it closes.

### Loading, and what did not come

While the page, a sheet or a picture is fetched, or a picture is decoded (`progress`,
`loading_shown` in `nxweb.c`):

- **A bar** fills the address field from the left in light blue and a line of three rows under
  the bar in dark blue, as much as has come (a sheet or a picture each; a page in parts by its
  kilobytes); while that cannot be counted (the page's text not here yet) a block moves along
  them, a step every 100 ms.
- **The status line** says what: "Loading example.com: 1 of 1 style sheet, 3 of 7 pictures (2
  not loaded)", and its right part "4 of 8". A message of a command after the page came stays
  instead (the bar still says it).
- **The window's caption** begins "Loading:", **the pointer** over the window is the
  hourglass of a program starting (`ui_launch_cursor` in `ui_launch.h`; it changes when the
  pointer next moves), the Stop key is in the bar, and Escape gives up what is still to come.
  Only what waits for a visitor's paste in the VRChat world (`docs/system/fetch.md`) is not that: the
  status line says how many are to be pasted, the pointer and the caption stay as they are.

What did not come is said, never left blank:

- **A picture** that could not be had is a grey box with a sign (a picture crossed out in red)
  and why, as `why_of` and libmedia say it: "not found (404)", "refused (403)", "the server's
  error (503)", "unsupported format" (the host could not decode it, 415, or it is not a picture
  libmedia reads), "cannot be decoded: ...", "no answer" (the host gave up), "no answer in 30
  s", "an address the host may not open", "stopped", "no network". Where the page gives its size
  the box is that size, its description on the first line and why on the second; where it does
  not, the box is one line, "description: why", as wide as that (`web_broken_size` in
  `web_layout.h`, at most 300 pixels), and the page is laid out again for it. A strip with such
  a box is drawn by requests.
- **The status line**, once all has come: "4 pictures and 1 style sheet could not be loaded:
  click here to see why", and its right part "5 not loaded" in red. A click on the status line,
  or View, What was not loaded, opens a window that lists each, sheets first, with why and its
  address (16 rows, then "and N more"); Close, Enter or Escape closes it.
- A picture of the image itself (`/usr/share/...`) that is not a PPM is decoded as the cache's
  are (`picture_local`); before, it was missing. `web-about.html` shows the fox's JPEG so.
- `NXWEB_DUMP=1` prints what is loading as it changes ("nxweb: loading: ..."), each failure and
  why ("nxweb: not loaded: picture ADDRESS: WHY") and "all here, N not loaded";
  `NXWEB_PRINT=1` the list window's rows and where it is. `web-missing.html` is the test page
  (a sheet and three pictures not there, a file that is not a picture, one that decodes);
  `tools/web_test.c --failed` lays a page out with a box for each picture not in the cache.
- **A short first page** (15 October 2026): a page shorter than the view shown before any
  worker had been asked for, as "No such page" or the empty view while the first page is
  fetched, was never drawn and the browser spun: its strips waited for a worker that
  `strips_work` never looked for (`w` 0 taken for "every worker has one").

| Key | |
|---|---|
| Ctrl+L, F6 | the address |
| Alt+Left, Backspace; Alt+Right; Alt+Home | back; forward; the home page |
| F5, Ctrl+R; Escape | the page again, from the network; stop |
| Ctrl+D | a bookmark for this page, or none any more |
| Ctrl+F, F3 | find in the page, the next place |
| Ctrl+O, Ctrl+S, Ctrl+U | open a file, save the page, its source in the editor |
| Ctrl+C, Ctrl+V | the address to the clipboard, an address from it |
| Tab, Shift+Tab, Enter | from link to link and field to field (a link's address in the status line), follow the link or send the form |
| Space on a field | a check box, a radio button, the next option of a select, a file's window, a button |
| arrows, Page Up, Page Down, Space, Home, End | scroll |
| F10; Ctrl+Q | the menus; close |

The window is 600 x 440: the menu bar's 20 rows came on top of the 420 of before, and the
page's view is the 586 x 376 it was, so `NXWEB_DUMP=1` and `tools/web_test.c` count as before.
`NXWEB_PRINT=1` prints after every key, click and menu's row where the browser is (address,
page of the session, bookmarks, options, status), a menu's rows as it opens, where the
options' window is, and where a link picked with Tab stands, for a test with `nxkey`.

The start with the home page was 1,799 thousand instructions and is 1,873 (13 October 2026,
`rvc_cpu`, a run with the browser less the same run without): the two bars and the status
line's cells are some hundred requests more. An icon in the address field, as Files has one,
was 56 thousand more for the icons' list read and was left out.

## Addresses the machine reaches itself

GitHub issue 1: a page served in the machine, by `nc` in a shell loop, was fetched by `curl` and `wget`
there and refused by the browser, which sent every address to the host. `web_net.h` is HTTP/1.1 over
the machine's own sockets; `net_route` says where each address goes, a page's sheets and pictures too:

| Address | Goes |
|---|---|
| `http://` a number, `localhost`, the machine's own name, a name in `/etc/hosts`, with a route in the kernel (`10.0.x.x`, `127.x.x.x`, what `route` added) | this machine's socket |
| `http://` a name a name server gives, if `/etc/resolv.conf` names one (none does in the image): a UDP question that does not wait | this machine's socket |
| `http://` any other name, or a number with no route | the host's fetch: as before in the harness, status 3 in the world (it opens `https` alone) |
| `http://10.0.x.x` with the link down | a page that says so: never the host's fetch, which would open a real 10.0.x.x of the host's |
| `https://` to the machine's own network | a page that says so: there is no TLS in the machine |
| `https://` any other | the host's fetch, the slots, the pastes, all as before |

- **Nothing waits** (`net_poll`): connect, send and read are non-blocking, a read takes 16 KB at most, and
  the socket is in the window system's wait once it can have something to read (`GrRegisterInput`,
  `net_watch`; 30 ms polls until then). The page scrolls, menus open and Escape stops meanwhile.
  While a window of the browser's own is up (a file's, the options) the event still reads the socket (`others`).
- **What it reads**: a status line and headers ended by `\r\n` or `\n`; a body by its `Content-Length`,
  by chunks or by the connection's end; `100 Continue`; a reply with no head at all (the first five bytes are
  not `HTTP/`), which is the page. One request at a time, `Connection: close`, no compression asked for.
  A connection refused is tried three times, 350 ms apart: a shell's `nc -l` loop is not listening between
  two answers. 15 s to connect, 30 s to a first byte and between two bytes, then what came is the page.
- **A page that takes its time** is shown as far as it came 1.5 s after its head, once, and again at its end
  where the view was (`net_shown`), not again if nothing more came. Escape keeps what came. Its sheets and pictures are asked for after it ends.
- **What the head's kind makes of it** (`net_kind_of`): HTML is the page. `text/*`, JSON, XML and the like are a listing
  (`Save` and the source give what came, not the listing). `image/*` is a page with that picture. A kind not said is looked at (`net_sniff`).
  Anything else, or `Content-Disposition: attachment`, is saved in `/root/Downloads` under its own name
  (a name taken gets a number), the page shown stays, and the status line says where. A file cut short is not kept.
- **An answer's status**: a redirect (301, 302, 303, 307, 308, relative too, eight in a row) moves the address and the
  history's entry; 303 and the others but 307 and 308 turn a POST into a GET. An error with a body is its body, the status
  line saying which; with none, a page says it. What failed is one page in words, with "Try again"
  ("refused the connection: nothing listens on its port 81", "No answer ... in 15 seconds: is a machine there?").
- **Pictures and sheets** go by the same way, a picture written to `/tmp` as it comes and decoded by libmedia on
  the workers as the ROM's are. They are kept for the session (48 files, 8 MB, the oldest go; `cached_*`), so a
  site's next page does not ask again; `Cache-Control: no-store` is not kept, and Reload asks anew.
- **Cookies** (`net_cookie_set`, `net_cookie_header`): `Path`, `Domain` (never an address's, never a name's top), `Max-Age`, `Expires`,
  the longest path first, 64 at most, `Secure` ones never sent (no TLS here), in `/tmp/.nxweb-cookies` so that the browser
  opened again in the session has them. Not across a machine's stop. The host's fetch carries none: `https` has no cookies.
- **Forms**: GET as before; POST as `application/x-www-form-urlencoded` or `multipart/form-data` (a file field opens the
  file's window), to machines reached this way only, since the host's fetch only asks for addresses: elsewhere the status line says so.
  Radio buttons exclude each other, Reset resets, `type=button` does nothing, `disabled`, `readonly`, `maxlength` and
  `<button name value>` are honoured, a `<textarea>` has lines and Enter is a line in it, Ctrl+V pastes into a field.
  Back to a form's answer, or Reload of it, does not send it again by itself: a page offers to.
- **Back** puts a page where it was left, also one that had to be fetched (`top_wanted`). A page from the
  network may not open this computer's files by a link.

The start with the home page: 3,422 thousand instructions of the whole run before and 3,396 after (13,649,874 and
13,623,793 less 10,228,001 of the run with no browser, the same each of three runs, 31 October 2026, `rvc_cpu`):
the sockets' code costs nothing until an address needs it (the cookies' table is made at the first request). The program is 632 KB, 45 more.

    gcc -O1 -g -fsanitize=address,undefined -Ilinux/apps tools/web_net_test.c -o web_net_test && ./web_net_test

serves each kind of answer from a child over a socket in pieces of 1 to 37 bytes, and tries addresses,
cookies and kinds of bytes: `PASS`. In the machine `tools/test_all.py web`: `nxweb-issue` is the issue's
shell loop by `127.0.0.1` and by the LAN address, `nxweb-http` is `thttpd` by name, by `/etc/hosts` and by the LAN
address with a sheet, a picture, a POST through a 303 with a cookie that comes back, a 404, a file saved, text, Back; `nxweb-slow` a page
that takes its time, scrolled meanwhile and stopped, and chunks. Two machines (`--net 47340 --net-id 11` serving the
loop, `--net-id 2` with `nxweb http://10.0.0.11`): the same page across the link.

The reporter's steps now: serve the loop on one machine; on another (or the same) type `10.0.0.11` into the address bar and Enter.

## The parts

| File | What |
|---|---|
| `css.h` | style sheets: rules read from text, kept by the last step of their selector, found for an element of the tree |
| `web.h` | the page: HTML into a tree, each element's style, addresses made whole, the fonts' advances, the sheets a page names, the copies the image keeps |
| `web_layout.h` | the tree laid out for a width: a list of things to draw at places of the page |
| `web_net.h` | HTTP over the machine's sockets, a step at a time, and the cookies |
| `nxweb.c` | the window, the host, the pixmap the page is drawn in, the keys and the pointer |
| `tools/web_test.c` | the first three on the host, with no window system and no machine |

The first three have nothing of the window system in them. `tools/web_test.c` builds them
natively (`gcc -fsanitize=address -Ilinux/apps tools/web_test.c`) against the image's own
fonts and the cache of `web/preload.py`: it lays a page out in milliseconds, says how many
texts, boxes and pictures came of it, how many texts lie past the right edge or over another
text (both are mistakes of the layout's), lists every item (`--items`), says what an item is
inside and with what style (`--why X Y`), and draws the page into a PPM file (the cache's
JPEGs and PNGs in it with libmedia compiled in, `-DWEB_TEST_MEDIA`: the line is at its head). A change to the
engine is tried there on every cached page first: the machine then gives the same numbers.
A `.md` file is made HTML as the editor's reader makes it (`nxedit_md.h`), and `--sections` lays
it out a section at a time as the reader does (`nxedit_page.h`; `docs/system/nanox.md`, "The
editor's reader"), printing the reader's `NXEDIT_DUMP` line.

## The tree and the styles

- **The tree** (`web_parse`): elements and texts, with what HTML leaves unsaid (an `li` ends
  the one before it, a `td` its neighbour, a block a paragraph left open). Text is kept as
  16-bit characters (UTF-8, or Latin-1 when the page says so), white space as written: the
  style says later whether a run of it is one space or a listing's lines and columns.
  `<noscript>` in the body is read as what it is for a reader without scripts.
- **A tag's attributes are walked once** (`web_note_names`): a bit for each name (by its first
  letter and its length) answers most later questions about an attribute at once, and the id
  and the classes are taken in the same walk.
- **Styles** (`web_style_all`): parents before what is inside them. What the tag says of old
  (`web_plain_style`: headings, lists, `bgcolor`, `width`, `align`, `cellpadding`, `<font>`),
  then the sheets' rules, the weakest first, then the element's own `style`. A font's size is
  applied before the rest, since an `em` of the rest is that.
- **A rule is not looked at when what it must be inside is not there**: the names (tags, ids,
  classes) of the elements round the one being styled are counted as the walk goes down and
  up (`css_inside`), and a rule with an ancestor step that names none of them ends there.
  Hacker News' sheet has a dozen rules that end in `a`: each was tried on each of 230 links,
  climbing to the root.
- **Styles on the worker cores** (`web_style_elsewhere`, `styles_elsewhere` in `nxweb.c`): the
  first core styles the tree in its order, and an element with more than 48 nodes inside
  (`WEB_STYLE_LEAST`) has its inside styled by a worker that has nothing to do
  (`web_style_range`), with a count of the names round it of its own, while the first core
  goes on after it. Each element's slot is numbered first, a node is whole 16 bytes (96) so
  that two cores never store to one texel, and an inside with a grid in a style attribute
  stays on the first core (its columns are kept in `css_text`, which grows). The first core's
  styles, in thousands of instructions (15 October 2026, `rvc_cpu`): Hacker News 1,664 to 533,
  Lobsters 2,225 to 673, Wikipedia's main page 1,446 to 582, Gutenberg's top 100 4,971 to 900,
  BBC News 4,061 to 1,041; a page's text to its first picture 7,264 to 5,804, 12,169 to 9,819,
  9,450 to 8,163, 21,559 to 17,109 and 21,398 to 18,024 (`NXWEB_STYLES=here` the old way, in
  the same binary). The tree costs 3% more for the larger nodes. `NXWEB_CHECK=1` styles the
  page again on the first core alone and compares: 0 elements differ on those five pages, also
  with `RVC_RACES` (nothing read as the pass began, nothing undone).
- **The sheets are not read beside the tree**: both put names into one table of atoms
  (`css_atom`), and nothing keeps two cores out of it in one pass. That would want the tree's
  names gathered apart and joined to the sheets' afterwards.

| Understood | |
|---|---|
| Selectors | tag, `.class`, `#id`, `*`, `[attr]` and `[attr=v]` (also `^= $= *= ~= |=`), descendants, `>` children, `+` and `~` siblings, `:root`, `:link`, `:first-child`, `:last-child`, `:only-child`, `:nth-child()`, `:empty`, `:not()` of one step |
| Left out, with their whole rule | states (`:hover`, `:focus`, `:checked`), `::before` and `::after`, `:is()`, `:has()`, nesting |
| Text | `color`, `font-size`, `font-family` (to one of three faces), `font-weight`, `font-style`, `font`, `line-height`, `text-align`, `text-decoration`, `text-transform`, `white-space`, `vertical-align` (sub, super, and a cell's) |
| Boxes | `display` (block, inline, inline-block, flex, grid, table and its parts, list-item, none), margins, padding, borders a side (width, colour), `background` (a colour), `width`, `height`, `min-` and `max-width`, `min-height`, `box-sizing`, `float`, `clear`, `visibility` |
| Rows and grids | `flex-direction`, `flex-wrap`, `flex`, `flex-grow`, `flex-basis`, `justify-content`, `align-items`, `gap`; `grid-template-columns` with `fr`, lengths, `repeat()` and `minmax()` |
| Values | px, em, rem, pt, %, vw, vh, `calc()` of a sum, `min()`/`max()`/`clamp()` (their first), `auto`; `#rgb`, `rgb()`, `hsl()`, names, `light-dark()`, `currentcolor`; `var()` with fallbacks; `!important` |
| At-rules | `@media` with widths (also `width <= N`), `screen`, `print`, `prefers-color-scheme` (light), `scripting` (none), `hover`; `@supports` and `@layer` are read as if true |

- Lengths are 13/16 of the page's: text of 16 pixels is 13 here, and a page is as if 704
  pixels wide in a window of 600, which is what media queries are told. A picture is that
  much smaller too, except a sign of up to 32 pixels, which stays pixel for pixel.
- A font's family is one of three faces: the first name of the list that is known says
  which (Arial, Verdana, system-ui: without serifs; Georgia, Times: with; anything "mono",
  Courier, Consolas: fixed). A page that names none is in the face with serifs, as in any
  browser.

## Layout

`web_layout()` gives items (`struct web_item`): a text in one face and colour, a filled or
framed box, a picture, a form's field. Their order is the order to draw them in.

- **Boxes one under the other**, margins between them collapsing (`web_gap`), a width when the
  style gives one, centred by `margin: auto` or `<center>`.
- **Lines** (`struct web_flow`): words, pictures and boxes in a line stand on one baseline
  (`web_line_break`); a word's characters that its face lacks are in the face of every script
  (`web_char_face`); a word no line holds is broken where it must be. Words that follow each
  other in the page's text, in one face and colour, are one item: a line of plain text is one
  request to the window system.
- **Tables** (`web_table`): each column's narrowest and widest from its cells, the table as
  wide as they like up to its room or as its `width` says, the columns sharing what there is
  between narrowest and widest; `colspan`, `rowspan` (a cell keeps its columns in the rows
  below), a row as high as its highest cell, a cell's content at its top, middle or bottom.
- **Rows** (`web_flex_row`): each box its `flex-basis`, its `width` or its content's width;
  what is left over shared by `flex-grow` or placed by `justify-content`; shrunk towards
  their narrowest when too wide, and on further rows when even that is too wide or the row
  may wrap. A column is boxes one under the other with the gap between. **Grids** are the
  columns their template names, filled row by row; a template whose columns would be under
  40 pixels is one column.
- **Floats** (`web_float_box`): a box at the left or right edge of its column; the lines
  after it are shorter beside it, a table or a row stands beside it, `clear` goes under it.
  A box of nothing but floats holds them (a bar of floated items).
- **Nothing is cut off and nothing is placed by coordinates.** There is no clipping and no
  scrolling sideways: text that may not be broken is broken all the same where its box ends,
  a picture wider than its column is made smaller, a negative margin is none (a side column
  pulled into place by one comes under the content). `position: absolute` and `fixed` are
  boxes where they are written, except one with a clip, which is text for screen readers and
  not shown. A listing wider than the page (`white-space: pre`) is the one thing that runs
  off the right edge.
- **Forms**: text fields, buttons, check boxes, selects (a click takes the next option). A
  form whose method is GET is sent as an address with the fields' values after it, one whose
  method is POST to a machine of this network as a request's body (`submit` in `nxweb.c`, above).
- A page that sends its reader on at once (`<meta http-equiv=refresh>`, often inside
  `<noscript>`) is followed, three times in a row at most.

## Strips, scrolled as a copy

The page is not drawn into the window. `nxweb` asks for a pixmap as wide as its view and as
high as 3.2 MB of GPU memory allow and no higher than the GPU's target on this host
(`tile_make`: 2,048 rows in the harness, 720 in the Unity world), in whole strips of 64 rows
(21 strips, 1,344 rows, at the usual width; 11, 704 rows, in the world), and the view is a
copy of a part of it (`show_view`): one textured rectangle of the GPU's, two where the ring of
strips wraps, whatever is on the page.

- **The GPU draws no row past its target's.** The host says the target's size in its flags
  (`0x8100003c`, `docs/machine/gpu.md`). A pixmap of 1,365 rows in the world had its rows from 720 on
  never drawn, black as a new pixmap is: the home page scrolled to its end showed 152 black
  rows of its 376 (13 October 2026). The driver gives no pixmap larger than the target the
  GPU's memory any more, and the browser asks for none. (A strip's own list is drawn into the
  target's top 64 rows and copied to its slot, so it could land lower; a strip drawn by
  requests and the window system's other drawing on the pixmap still draw it whole.)
- **A pixmap in the GPU's memory** (`pixmap_map` in `scr_shaderemu.c`, `docs/system/nanox.md`): a
  pixmap of the screen's kind gets its pixels from the pool the windows' buffers come from,
  and is drawn on by the GPU as a window is; a copy from it to a window is a rectangle with
  it as its texture. The driver counts such pixmaps in a word a program can read (`REG_PIXMAPS`):
  if the pool had no room the browser draws into its window as before (so does `NXWEB_DIRECT=1`).
- **A scroll is steps**, a third of what is left each 15 ms (`scroll_step`): a notch of the
  wheel or a page key moves the page smoothly, and a step asks the window system for a copy
  and the scroll bar, nothing else.

### Strips

The pixmap is a ring (`strips_work` in `nxweb.c`):
strip k of the page is in slot k modulo the slots, so a scroll keeps every row already drawn,
and the strips coming into reach are made ahead of the view while the browser waits for its
next event (`strips_meanwhile`), the view's first, then the way the scroll goes. Only a jump
(End, Home, the bar's thumb) makes the view's strips at once, on the first core.

- **A strip is a GPU list of its own** (`strip_job`): the page's colour, then its items as
  `draw_item` draws them, at their rows less the strip's: a rectangle a box, four for a frame,
  one text command and a word a character for a text (its glyphs from a table of each face's
  first 256, made once: `strip_face_ready`), and a picture as a rectangle of fragment mode 4:
  a PPM file of the ROM sampled where it lies, any other decoded to its drawn size into the
  GPU's memory and copied pixel for pixel (`strip_picture_ready`, "Pictures" below; the texture's
  coordinates are those the window system would give it). Commands before the first text of coverage are
  in the opaque pass and the rest in pass 5, as `romfont_draw` has them. A slot's list and its
  strings are 24 KB of a second pixmap used as memory (256 commands, 2,048 characters).
- **Worker cores make them**: up to four, lent (`mcw_lend`): a program that keeps its workers
  has them back at once and the first core makes the strips. Sixteen are opened, the other
  twelve for the pictures' decoding (`PICTURE_WORKERS`). A job reads the items and the
  page's text and writes nothing but its slot's memory; what it draws besides the items (what
  Find found, the link picked) is copied for it while no job is out (`strips_shown`).
- **The first core submits them**, as `gles.c` does: the lock word, the list, the copy into
  the strip's 64 rows of the pixmap (`strip_submit`). A worker may not take the lock: an atomic
  swap does not keep two cores out of it in the same pass (`docs/machine/multicore.md`, "Races").
  Once submitted, a strip is there for the window system's copy of the view, which is drawn
  after it. A slot's memory is written again only after its last list was drawn.
- **A strip with a field, a picture neither in the ROM nor decoded to its size in the GPU's
  memory, or more than its list holds is drawn by requests** (`strip_requests`): on a pixmap of 64 rows, then copied into its slot, so that
  nothing of it reaches the strips beside. Before the next list into a slot drawn so, a pixel of
  the pixmap is read: the window system's drawing there must reach the GPU before the list.
  That pixmap has 32 rows more over and under the strip (`STRIP_ROOM`): the window system draws no string a
  pixmap's edge would cut, and until 31 October 2026 a field's text across two strips was in neither (a blank field,
  a button with no words, as the capture of a form showed; `NXWEB_CHECK` looks only at strips that are lists). On
  `web-layout.html` under WARP `NXWEB_CHECK=1` said 7 strips, 7,591 pixels differ, 7,048 by more than 8 levels at the commit before
  (so the 0 above no longer held, and the text across strips was the difference); with the room 10 strips, 61 pixels, none by more than 8
  levels: not 0, the same drawing 32 rows lower in the target rounding otherwise, which was not chased.
- A field typed into, a picture that came and a page laid out anew make their strips again
  (`strips_forget_rows`, `strips_forget`); every change waits for the jobs out first.
- **The window system says where a pixmap's pixels are**: `GrGetWindowInfo` on a pixmap the
  GPU keeps gives its address as for a window (`docs/system/nanox.md`). On a server from before, every
  strip is drawn by requests.
- `NXWEB_STRIPS=requests` draws every strip by requests, `=here` makes their lists on the first
  core alone: the earlier steps, in the same binary. `NXWEB_ROWS=N` holds the pixmap to N rows
  (720: as in the world). `NXWEB_CHECK=1` makes every strip of the page both ways, its list and
  by requests, reads both back from GPU memory and says how many pixels differ.

The longest time between two steps of a scroll, before and after (15 October 2026, `rvc_cpu`
with the world's clock, `--pass-ms 2.7`, and the world's pixmap, `NXWEB_ROWS=720`; four notches
of the wheel and three page keys down, then End and Home; ms of the guest's clock, and the
first core's instructions between the two steps, the window system's included):

| | before: the pixmap drawn again | strips by requests | strips as lists, by workers |
|---|---|---|---|
| `web-long.html` (8,718 rows of plain text), notches and page keys | 35 to 59 ms, 112 to 123 thousand at a redraw | 34 to 45 ms, 40 to 69 thousand | 34 to 39 ms, 28 to 35 thousand |
| the same, End and Home | 104 and 84 ms | 67 and 66 ms | 56 and 59 ms |
| Hacker News (1,115 rows: six of the nine scrolls move) | 34 to 197 ms, up to 364 thousand | 34 to 73 ms, up to 118 thousand | 34 to 35 ms, 28 to 44 thousand |
| Wikipedia's main page (16 pictures), notches and page keys | 35 to 83 ms, up to 191 thousand | 37 to 62 ms | 34 to 54 ms, one page key 113 |
| the same, End and Home | 146 and 177 ms | 59 and 142 ms | 35 and 67 ms |

A step's own work is 1 thousand instructions however the strips are made; what is left between
two steps is the window system's copy of the view and the scroll bar (26 to 28 thousand before
as after) and the submits of strips made meanwhile. On `rvc_cpu`'s own clock (a pass 0.25 ms)
the first core makes many more strips between two steps, and the same scrolls measure 31 to 43
thousand a notch and 37 to 43 a page key on the long page against 136 to 149 at a redraw before.

- **On the card** (15 October 2026, RTX 5090, D3D11 and fxc2, through the queue, the world's
  pixmap; the same scrolls): on the long page the longest gap between two steps was 23 to 43
  ms before and 25 to 40 after, but before, one to three of a notch's nine steps never reached
  the screen (the GPU's copy of the step was not made before the next) and half of End's and
  Home's; after, every step's picture was there before the next but one, in the first scroll
  after the page came. Hacker News: 26 to 82 ms
  before, a redraw leaving one or two of eight steps seen, and 21 to 37 ms after, all seen.
  The first core's instructions between two steps there: 196 to 402 thousand at a redraw
  before, 41 to 120 thousand after.
- **Pixels** (`NXWEB_CHECK=1`, the harness with `--cpu --warp`): every strip that is a list is
  the same, pixel for pixel, as the strip drawn by requests: `web-long.html` 137 strips,
  Hacker News 17 (its 18th has the search field), `web-layout.html` 15 (its 16th has a field),
  Wikipedia's main page 40 with its pictures. With the pictures decoded (15 October 2026, the
  desktop's image): Wikipedia's main page 40, xkcd.com 19, BBC's front page 358, 0 pixels differ.
- **Races**: a scroll of the long page with `RVC_RACES` read nothing as the pass began and undid
  nothing; workers' faults 0.
- **Beside a program that takes every worker**: `nxray` started while the browser scrolled had
  all 255; the browser's next strips were made on the first core.
- **Beside another program**: glxgears' 5 s lines while the browser scrolls the long page for
  half a minute (`rvc_cpu`, the world's clock) were 285 to 298 frames a second before and 321
  to 339 after, of 370 to 387 alone.

### Pictures

The image keeps a page's pictures as `web/preload.py` makes them (`docs/system/fetch.md`): one of 4,096
pixels or fewer as a PPM, a graphic as a PNG in segments, a photograph as a JPEG in restart
intervals. A PPM of the ROM is sampled where it lies, at any size, as before. Every other
picture, the image's JPEGs and PNGs and what the host fetched (its PPM in `/tmp`), is decoded
with libmedia (`docs/system/media.md`) on the worker cores **to the size the page draws it at**
(`picture_decode`): the mean of the pixels under each when smaller (`media_image_fit`), a shape
more than 2% off stretched from the nearest pixels. Its pixels go into the GPU's memory in
fragment mode 4's layout, and both a strip's list and the window system copy them pixel for
pixel, the window system from a PPM of them written when it first draws one
(`/tmp/nxweb-PID-N-drawn.ppm`). Drawn scaled, the two did not agree: the list's rows come from
the GPU's interpolation and the window system's from a band of the file the processor chose,
and `NXWEB_CHECK=1` under WARP found 821 pixels of 9 strips differing on Wikipedia's main page.

- **The memory** is one pixmap of the GPU's used as memory, 2 MB (or half, down to 256 KB, as
  the pool allows), taken from in turn and given back whole with the page: the window system
  has eight such pixmaps for all programs, and the browser holds three. A picture that finds no
  room is decoded into its PPM alone, and its strips are drawn by requests.
- **The page first**: a picture not decoded yet is a grey placeholder, in a list as by requests.
  While the browser waits for events, the pictures asked for are decoded one at a time in the
  order they were asked for, the view's first, and their strips made again
  (`pictures_meanwhile`). `NXWEB_CHECK=1` decodes them as their strips are made.
- **One size a picture**: another item drawing it at another size is drawn by requests, scaled
  from the PPM. A layout that leaves no item at that size has it decoded again.
- The library decodes on the workers the browser has open: sixteen. A 250 x 180 JPEG took
  2,366 ms on the first core alone, 646 ms with four workers and 275 with sixteen; 32 and 63
  were no faster (`rvc_cpu`).

From the image's copy of the page (15 October 2026, `rvc_cpu`, `NXWEB_TIME=1`; ms of the guest
after the page's text, the first core's instructions decoding): the cache of 13 October's 201
pictures as PPM before, and the same pictures as `preload.py` keeps them now after.

| | Wikipedia's main page | xkcd.com | BBC's front page |
|---|---|---|---|
| the first picture of the page, before / after | 3,070 / 3,059 ms | 1,104 / 1,056 ms | 12,046 / 12,022 ms |
| the view's pictures there, before / after | 3,070 / 4,062 ms | 1,104 / 3,277 ms | 12,046 / 13,044 ms |
| pictures decoded for the view, instructions | 4, 3.0 million | 4, 7.1 million | 2, 3.0 million |

A picture: a 120 x 163 JPEG drawn 98 x 132 672 thousand instructions and 209 ms, a 540 x 100
JPEG drawn 439 x 81 1,850 thousand and 576 ms (1,150 thousand of it the decoding, the rest the
mean), a 185 x 83 PNG drawn 150 x 67 1,590 thousand and 479 ms, a 480 x 270 JPEG drawn 58 x 32
1,431 thousand and 527 ms. What libmedia lacks that would cut these: a JPEG decoded at a half,
a quarter or an eighth of its size, and a small picture's fixed cost (a 40 x 40 JPEG was 500
thousand instructions of the first core, which is why the small ones stay PPMs).

## What a page costs

Instructions of the first core (`NXWEB_TIME=1`; `rdcycle` round each step), a page from the
cache in the ROM, the window 600 x 420:

| Thousands of instructions | Hacker News | Lobsters | Wikipedia's main page | danluu.com | Gutenberg's top 100 |
|---|---|---|---|---|---|
| bytes of HTML, of sheets | 35 KB, 7 KB | 59 KB, 59 KB | 75 KB, in the page | 23 KB, none | 60 KB, 190 KB |
| elements and texts, rules | 1,299, 88 | 2,005, 276 | 1,274, 84 | 1,048, 4 | 2,267, 518 |
| the sheets found and read from the ROM | 361 | 533 | 484 | 224 | 721 |
| the sheets' rules | 733 | 3,357 | 919 | 84 | 7,762 |
| the tree | 1,819 | 2,573 | 2,222 | 1,159 | 2,780 |
| the styles | 1,665 | 2,224 | 1,439 | 605 | 4,970 |
| the layout | 1,540 | 1,816 | 3,780 | 1,760 | 3,987 |
| the pixmap's rows asked of the window system | 354 | 582 | 671 | 73 | 361 |
| all, and at 3 million a second | 6,472: 2.2 s | 11,085: 3.7 s | 9,515: 3.2 s | 3,905: 1.3 s | 20,581: 6.9 s |

- A site's next page has the same sheets: they are not read again (`web_read_sheets` keeps a
  sum of them), so Lobsters' second page is 54 thousand where the first was 3,357.
- Where it went, and what was taken off (Hacker News, 13.3 million at first): every question
  about an attribute walked the tag (a bit a name now: 15% of everything); a rule ending in
  `a` was tried on every link up to the root (the names round an element are counted);
  `font-family` looked for 29 names anywhere in its value (one walk over the list); text was
  measured a call a character (a table of the face's first 256 advances, and a word the face
  has whole is put with one walk).
  A table's advances are looked up in the font when a text first has the character
  (`WEB_ADVANCE_LATER`, `web_advance_found` in `web.h`): filled whole, each face's table was
  26 thousand instructions for the sixty characters a page has; the start 1,940 thousand to
  1,873, the same pictures.
- **The start** with the home page (3.7 KB, 122 nodes, 77 texts) is 1,859 thousand, where the
  browser of before was 1,400: the layout 296, the tree 175, the sheets 116, the styles 75,
  four font files opened and mapped 143, 120 pages first touched 184, and in the window
  system the texts' glyphs looked up (`rom_glyph`, 130 instructions a character) 240. Taken
  off: the font tried at the start is the plain face's (not made twice), and the window
  system's widths are asked once (1,866 to 1,859). Not done: the home page laid out at the
  build. (The pixmap's rows below the view are now drawn while the browser is idle:
  `strips_meanwhile` in `nxweb.c`.)
- **Scrolling** (`build/web/cost.sh`'s way: the same `nxkey` line, less the same waits with no
  events): a notch of the wheel 251 thousand instructions in five steps where the browser of
  before drew the view again once for 336; a page key 182 thousand in ten steps where it was
  252 in one. A step is under 20 thousand: 6 ms of the first core.

- **On the shader machine** (13 October 2026, RTX 5090, D3D11 with fxc2, through the card's
  queue, 2.2 to 2.4 million instructions a second in the run; `NXWEB_TIME=1` has a scroll say
  how its steps went, by the host's clock and the GPU's count of copies): a notch of the wheel
  on Hacker News is 5 steps in 89 to 96 ms, the longest gap between two 28 to 36 ms; a page
  key 10 steps in 183 to 185 ms, the longest gap 21 to 23 ms; every step's picture was in the
  window before the next step was asked for. The screen at the end (six notches, two pages
  down to the page's end, one up: row 378) is the host test's drawing of those rows, every
  pixel within 8 levels. Hacker News from the ROM's copy was 6.4 million instructions there
  too. Not measured there: the browser of before, and a page larger than these.

The pictures in the machine against the same rows drawn by `tools/web_test.c` on the host
(13 October 2026, the harness with `--cpu --warp`, the view's 586 x 376 pixels): the layout
page 99.96% of pixels within 8 levels, Hacker News 99.99%, scrolled eight notches 100%,
Lobsters 99.6%, text.npr.org 99.9%, kernel.org 99.9%, Wikipedia's main page 97.5% (its
pictures are scaled by the GPU and by the test differently). What differs by more is slanted
text, which the test only approximates.

**A scrolled page is checked with the world's target, not the harness's.** `rvc_harness`
draws the GPU's lists into 2,048 rows and the Unity world into 720, so a row the world never
draws is drawn there: every capture of 13 October 2026 was right while the world showed a
black band. The check: the harness with its viewport, the host's flags and `GPU_TARGET_W`,
`GPU_TARGET_H` at 1280 x 720 (three places; a build of it apart, the tree's has no switch
yet), `--cpu --warp --desktop --save-state`, the home page and `web-long.html` scrolled by
each means (`nxkey wheel:300,250:-3`, `pagedown`, `end`, the bar's thumb dragged by
`hold:` `to:` `drop:`, a click under it), and in the window's layer of the snapshot the rows
that are black for more than half their width counted: none but the frame's last. Before,
on the home page: 152 rows by each means (at its end the view is the pixmap's rows 496 to
871); `web-long.html` at its end all 376, `web-layout.html` 246. After: none, and each
picture is the 2,048-row harness's pixel for pixel.

## Scripts

Not run. Measured again on 31 October 2026 with the image's own `qjs` (1.2 MB) under `rvc_cpu`, core 0's
instructions less the same line with `true`, a script's run less `qjs -e 0`:

| | thousands of instructions | at 3 million a second |
|---|---|---|
| the engine's start (`qjs -e 0`) | 1,499 | 0.5 s |
| 180 functions of the DOM calls below, parsed and not called (about 24 KB) | 13,676 (570 a byte) | 4.6 s |
| a class toggled on 50 objects, no DOM behind them | 547 | 0.2 s |
| a list of 200 rows built as text | 889 | 0.3 s |
| 100 thousand additions | 36,851 | 12.3 s |

So a page's small script (Hacker News' 5 KB) would be some 4.5 million before it does anything, jQuery's 90 KB 50 million,
17 s, before any page drawn; and each change it makes to the tree means the layout again (1.5 to 4 million, "What a page costs") and a
binding of some forty calls that does not exist, in a program five times the browser's size or a process beside it. It
cannot be made a feature in a session and half of one helps nobody: a local server's pages, which this browser now
reaches, work without. The earlier measurement, and what the first step would be:

Measured before (13 October 2026; QuickJS, quickjs-ng, MuJS, Duktape
and mquickjs built for the machine and counted with `rdcycle` under `rvc_cpu`):

- **What sites ship.** Of the 24 sites of `web/sites.txt`, 12 send no script at all and all 24
  have their text in the HTML. Of 18 mainstream sites tried beside them, eight answered a
  client without scripts with a challenge or a wall (Google's results, Reddit, Amazon, the
  New York Times, Stack Overflow), three are empty shells (YouTube, X, Instagram), the rest
  have their text in the page and use scripts for menus, search suggestions, collapsing and
  advertising.
- **What the language is.** 303 script files of those sites (10 MB): QuickJS parsed every one
  that was not a module with imports; the ES5 engines parsed 10 to 17%. Hacker News' 5 KB
  fails on them at one arrow function. So the engine would be QuickJS: 1.1 MB of program,
  five times this browser, 0.44 MB of memory at its start.
- **What parsing costs.** 701 instructions a byte of source over those files: 4.3 KB of script
  a second at 3 million instructions a second. Hacker News' script 0.8 s, jQuery 22 s, React
  with ReactDOM 38 s before anything is drawn, a site's own bundle three to eight minutes.
  Bytecode made ahead reads 17 times faster (jQuery 1.2 s) but is 3.5 times the source.
- **What running costs.** A class toggled on 50 elements 0.07 s, a list of 200 rows built as
  text 0.5 s, 50 KB of JSON parsed 2.3 s, 100 thousand additions of whole numbers 7 s and of
  fractions 12 s (a double is about 150 instructions of the machine, which has single floats
  only), 100 thousand function calls 15 s.
- **So**: a page may spend about ten thousand bytes of script at its load and a few hundred
  simple steps a click. That is Hacker News' comment folding and Lobsters' "3 hours ago", and
  nothing with jQuery or a framework. In the VRChat world it is less still: every script is
  one more address of a kind the world must have been built with, their names carry the
  version's hash, and there are no cookies and no POST for what the small scripts mostly do
  (vote, log in, load more). (POST and cookies there are this machine's own network's since 31 October 2026.)
- What was taken from it without an engine: `<noscript>` is shown, `<details>` opens and
  closes, a redirect for readers without scripts is followed. The next step, if one is wanted:
  QuickJS in a process of its own with a budget of instructions (`JS_SetInterruptHandler`),
  scripts of the page's own site under 10 KB, and a tree of the page it may change, after
  which the page is laid out again. The tree is there now (`web_nodes`); a binding to it is
  about forty calls (`querySelector`, `classList`, `addEventListener`, `getAttribute`,
  `innerHTML`, `style.display` lead the count in the scripts looked at).

Not done: scripts, POST and cookies through the host, https in the machine, several connections at a time, keep-alive, HTTP authentication, a caret inside a field, `<label>`, pictures as backgrounds, SVG drawn (a page's inline `<svg>`
is passed over, which is Google's own logo), positioned boxes, clipping and scrolling inside
a box, text selection, text larger or smaller (the 13 of 16 is a constant of `css.h`), a find across
two pieces of text.
