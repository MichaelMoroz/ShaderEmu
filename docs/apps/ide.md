# The IDE

`nxide` is a place to write C in the machine, build it with the machine's own compiler
(`docs/system/cc.md`) and run it, with a debugger; and JavaScript and Python, run by QuickJS
and MicroPython (`docs/system/scripts.md`), Python with a debugger too; and HLSL shaders, compiled
in the machine for the lanes (`docs/system/hlsl.md`). Start menu, Programming;
a `.c`, `.h`, `.js`, `.py` or `.hlsl` file opened anywhere is its (`/usr/share/nxopen.15-ide`,
`docs/system/open.md`), and so is the explorer's Edit of one. A file's language is its ending.

    / # nxide                   # the project of last time, else the hello example
    / # nxide FILE.c
    / # nxide FOLDER            # the folder as the project

    wsl -- bash /mnt/c/.../linux/ide/build.sh      # 6 s, after Nano-X's; then the image

## What it is made of (`linux/ide`)

- `nxide.c`: the window. A menu bar (File, Edit, Selection, View, Run, Help: `ui_menu.h`'s
  menus), a row of keys, the text with its line numbers, a panel below with four leaves
  (Output, Problems, Variables, Call stack) and a status line that also asks (Find, Go to
  line). Save as is the Save window (`ui_save_file`; a test's script still types the path in
  the status line), and a text with changes is asked about before New, Open, an example,
  Exit or the caption's box lets it go: Save, Don't save, Cancel (`ui_ask.h`, `docs/system/nanox.md`).
  New and Open wait for a build or a run to end; Build writes the file only when it changed.
- `text.h`: the text and its history. Every change is put in or taken out through two
  functions, which keep what they did: Ctrl+Z takes a command back and Ctrl+Y makes it
  again, 1,024 changes deep; letters typed one after another are one change. A tab is
  spaces to the next fourth column, in the file too.
- `syntax.h`: the colours. A lexer, a line at a time, for C, JavaScript and Python: keywords,
  types (in the scripts, the built-in names), numbers, strings, comments, the preprocessor's
  lines and Python's decorators, and a name before a bracket. The one thing a line needs from
  those above it is the state it begins in (inside a comment, a JavaScript template, a Python
  string of three quotes), which is kept for every line and worked out again from the line that changed.
- `pydbg.py`: the debugger for Python, run by MicroPython (below).
- `debug.h`: the debugger (below).

## Colours, and why no language server

A language server is a compiler that never stops running, and a compiler here is the first
core's whole time: compiling the smallest program is 0.85 s, the `home` example 5.5 s (most of
it the OpenGL and Nano-X headers). So the two halves are apart:

- **Colours are the lexer's**, a few hundred instructions for a line, done as a line is
  drawn. A key redraws its line; the lines below it are drawn again only when one was added
  or taken out, or a comment was opened or closed.
- **What is wrong is the compiler's.** A second and a half after the last key, the text as
  it is goes to `tcc -c` beside the program (never while something is built or run), and
  what it says is marked: a red bar before a line with an error, an orange one for a
  warning, and the lines in Problems (a click goes there). View, "Check while typing" turns
  it off; a build always does it.

Since 17 October 2026 the files open in tabs (up to 12), each with its own text, history,
marks and problems. A tree is at the left only when a folder is open as the project (File,
"Open a folder as the project...", "Its folder as the project", "Close the project"; Ctrl+B
hides and shows it; its header's box closes the project); the project is kept in the registry
as `ide.project` and opened at the next start without a file. A name too long for its tab, a
row of the tree or the caption is cut with "..." (`ui_text_fit` in `ui.h` stopped early and
let a tab's name run over its close box). Clicking an error
line in Output, or a function in Call stack, goes to that line, and the bracket at the cursor
is boxed with its pair. Ctrl+H replaces, as one change. Ctrl+W closes a tab, Ctrl+D selects
the word, Ctrl+Shift+S saves all. Script words: `close`, `tab`, `tree`, `saveall`, `back`,
`tabs`, `replace:OLD=NEW`, `project:FOLDER`, `noproject`.

For a script the check while typing is the interpreter's compiler: `qjsc -c` for JavaScript and
MicroPython's `compile()` for Python, each on a copy in `/tmp`, so a mistake of syntax is marked
as in C; a mistake only running finds is marked when it ends a run.

## Build and run

Build (F7) saves the file and runs `cc -g FILE -o /tmp/ide-NAME`: always with `-g`, since
TinyCC does not optimise either way. Its output is in Output and its errors in Problems,
with the first one's line shown. Run (F6) builds and then starts the program, whose output
comes to Output as it is written (a program with a window opens it as any does). Stop
(Shift+F5) ends whatever runs.

For a script Build only checks it (`qjsc -c`, or `compile()`), and Run runs `qjs --std FILE`
or `micropython FILE`. A script that ends with an error has its line marked and shown, from
the interpreter's trace back (QuickJS's `at ... (FILE:LINE:COLUMN)`, MicroPython's
`File "FILE", line N`), the innermost line of the file itself.

A shader (`.hlsl`, `.hlsli`, `.fx`) is checked while typing and built by `hlslc`, the HLSL
compiler as a program (`programs/hlsl/hlslc.c`), which says a mistake as tcc does
(`FILE:LINE: error: what`), so it is marked as C's are. Run starts `hlslview FILE`: the
`fractal` example built at image time (`linux/tcc/build.sh`), which compiles the file when
it starts and sets whichever of its uniforms the shader has (`eye`, `forward`, `right`,
`up`, `size`, `turn`, `time`), so a shader of `float4 main(float4 pos : SV_Position) :
SV_Target` and those uniforms runs as it is. A shader has no debugger. Its colours are
C's lexer with HLSL's words (`syntax.h`). The program's link has `-lhlsl` as `cc`'s has.

## The debugger

Without an MMU (the default machine) `cc` makes a flat program and deletes the ELF that has
the stabs: the IDE links as `cc` does and runs `elf2flt` itself, keeping `/tmp/ide-NAME.elf`.
It starts the program with the kernel's vfork, and moves every address by where the flat
program was loaded (the pc at the exec stop against `_start`; `dbg_relocate` in `debug.h`).

It is feasible, and it is line by line. Two things make it so:

- TinyCC's `-g` writes **stabs**: every line's address, the functions, and each variable
  with its type and where it is (a place from the frame's pointer, or an address).
  `dbg_load` reads them from the program's file; types are followed through pointers,
  arrays and structures.
- The kernel has **ptrace**. The machine has no single step, so a stop is an instruction that
  traps (a word of zeros: below, why not `ebreak`) written over a line's first instruction
  and taken out again when the program is held. To
  go on from a line, the instruction there is looked at (`dbg_one`): it can go to the next
  one or, a branch or a jump, to one other place, and a stop at each is one
  instruction's step.

What it does: F9 or a click before a line's number marks the line; F5 builds and runs to
the first marked line (with none, to the first line of `main`), and goes on from a stop;
F10 steps over a line, F11 into a call, F12 out of the function. At a stop the line is
yellow, Variables has the function's and then the program's (numbers, floats, characters,
pointers with the text they point at, the first eight of an array, a structure's fields),
and Call stack the functions it is in (TinyCC keeps the return address just under the
frame's pointer and the caller's frame under that). A signal stops it too, at its line.

- A step's stops are in the one function being stepped and where it returns to (the
  return address is just under the frame's pointer), never anywhere else. Over: that
  function's lines. Out: the return address alone. Into: an instruction at a time until
  the line is another; at a call of one of the program's own functions, on to that
  function's first line, and a call of anything else is stepped over.
- **The worker cores run the same memory.** A stop is an instruction changed, for every
  core: the first version put one on every line of the program for a step, and a step
  taken in `main` while the workers shaded stopped them in `shade` (the debugger then
  showed a line of `shade` with the first core's registers: nonsense). The debugger holds
  the first core's thread alone. A mark on a line the workers run cannot be a stop for them, and it
  ends the program cleanly: the stop is an instruction that is none (a word of zeros), which
  is SIGILL for the first core's thread and a trap for a worker, whose library then ends
  the program with a line saying which core and where; the IDE says a worker came to a
  marked line. (It was `ebreak` at first, which on a worker core means "park": the worker
  went to sleep in the middle of its job and the program waited for it for ever.)
- A function's first line (its name) and its closing brace are not places to stop: at the
  first the frame is not made, and the brace's address is not an instruction's. A mark
  there is the next line's.
- Not there: watching a variable, changing one, stopping a program that is running (Stop
  ends it), and anything about code the compiler's libraries are made of (they have no
  stabs: a step into `printf` is a step over it).

### Python

Debug on a `.py` file runs `micropython /usr/share/ide-pydbg.py FILE COMMANDS REPORT`, which runs
the file under `sys.settrace` and talks to the IDE through two FIFOs in `/tmp`: the marks and
go, into, over, out or stop one way, and at a stop its line, the module's names and values and
the file's frames the other. The keys are C's: F9, F5, F10, F11, F12. Over and out count the
file's frames (a call into another module is stepped over). Variables has the module's names
only: MicroPython keeps no names of a function's own variables. `sys.settrace` is built in
(`linux/micropython/build.sh`), with a change so that a call with no trace set makes no frame
object: as MicroPython has it every call paid for one, 40 times slower; now 2.4% (186.6
million instructions against 182.1 on a loop of calls).

JavaScript has no debugger: QuickJS has no hook at a line or a call to stop on, and the IDE
runs it only.

## The examples

File, Examples lists `/usr/share/example-*.c` (`linux/tcc/examples`), then the scripts'
`example-*.js` and `.py` (`linux/ide/examples`), then the shaders' `.hlsl`. One is opened as the
user's own copy, `/root/NAME.c` (`.js`, `.py`, `.hlsl`), which is written at the first Save or
Build. The script word `example:NAME.ENDING` picks one of two of a name.

| | |
|---|---|
| `hello` | prints, with a function to step into |
| `cube` | the smallest OpenGL program |
| `home` | a place to walk round |
| `shader` | a picture every pixel of which is one function's, on the worker cores |
| `fractal` | C: a ray marched Sierpinski pyramid, its shader compiled in the machine (`docs/system/hlsl.md`) |
| `fractal` (HLSL) | that shader: change it and Run |
| `pi` | JavaScript: pi to 200 digits with BigInt (upstream's `/examples/pi.js`, Bellard's) |
| `primes` | Python: a sieve, with a function to step into |

`shader` shares a 160 x 120 picture out in 240 pieces of half a row, about one a worker
(`mcw.h`, which `cc` links now: `docs/machine/multicore.md`), and draws it as one texture: 20
pictures a second with 255 workers on the shader machine. The first core shades the last
48 pixels itself while the workers' answers are on their way (two passes of the machine,
which it sat out before); much more than that and it is the one the picture waits for. Its waves are its own few floats
and not `sinf`: the C library works a sine out in doubles, each a call of hundreds of
instructions here, and with it a picture took seconds.

## What a key costs (12 October 2026)

Twenty letters typed went from 2.98 million instructions to 1.56, a page down and up from
6.42 to 2.82, a click in the text from 3.90 to 1.49 (ten of each), the start with a file
from 3.73 million to 2.13, and an idle IDE runs nothing where it polled.

- `tx_load` builds a file's lines directly (it went through `tx_insert`, a `realloc` a
  character: a million instructions for `home.c`); `tx_insert` reallocs once a line.
- A changed line is drawn once (it was up to three times a key), the tool row only when a
  key of it looks different; a full redraw fills the ground in two pieces; a line is one
  text request a colour, with spaces where the other colours' characters are (a space
  draws nothing), and is lexed once. Clicks and Shift+arrows redraw only the lines the
  selection's change touches. Widths come from `ui_width`, not from the server.
- Jobs are started without a shell; their output wakes the loop through `GrRegisterInput`
  and their end through SIGCHLD (a handler that does nothing: `select` returns).

The debug script's last stop prints line 20 in `main`, before these changes and after.

**13 October 2026.** Instructions on `rvc_cpu`, over the same keys sent to `demo-hello`:
20 letters 779 thousand to 390, ten pages down in `home.c` 1,778 to 1,340, the start with
hello 4,543 to 1,400, 20 letters with the check that follows them 2,583 to 2,130.

- **A line is kept as it is drawn** (`struct shown` in `nxide.c`): its pieces, a colour each,
  and whether it ends inside a comment. It is read by the lexer once, when it changed; a
  line below a changed one is not read at all while the changed one ends as it did. The
  lexer looks characters up in a table and words by their first letter (`syntax.h`).
- **A key draws from the first column that looks different** (`shown_differ`,
  `draw_line_tail`): the kept line against the new one, the row's ground from there and the
  pieces' ends. The cursor's bar is taken away where it was (`bar_line`). A row with a
  selection or the stop on it is drawn whole as before.
- **A whole view is drawn a colour after a colour** (`draw_editor_from`): a colour changed is
  a request too, and was three a line. The editor's text has a context of its own.
- **A view that moved a few lines is a copy** (`view_moved`: `GrCopyArea` of the rows that
  stay, the new lines drawn). `NXIDE_SCROLL=draw` draws it whole as before.
- The status line draws the text that changed, the row of keys the keys that look
  different, a check's end the lines whose bars changed and the panel if its problems did.
- **A text the compiler said nothing of is not shown to it again**: two sums of it are kept
  (`text_sums`), and `linux/ide/build.sh` writes the examples' into `/usr/share/ide-clean`
  (`clean.py` compiles each as the check does, with the compiler built for the host: run
  the IDE's build after `linux/tcc/build.sh`). Not for a text that includes a file by
  `"..."`. Without the list the start is 4,003 thousand.
- The check is started with the kernel's vfork, its file is written with one call, and the
  clock is asked once a key.
- The scroll bar's thumb stays in its bar when the view begins after the last whole page (Go
  to line near the end): it was painted over the panel and the status line. The one place
  the picture is not as before.

What is left of a check is the compiler's: `tcc -c` of an empty file is 1.34 million
instructions (its own tables and predefined macros, 150 pages first touched), of hello 2.6.
Its frees before it exits are 100 thousand of that (`linux/tcc`, not done).

To check the drawing: `NANOX_SOFTWARE=1`, a script of keys by chance with no command that
draws everything last, and the window's sum against the build before (20 scripts the same).
A script's `mark:N` moves the cursor without its column: keep it at a script's start.

## Checking it

`NXIDE_PRINT=1` prints what happens (`nxide: ...`, the program's output after `nxide|`,
and at a stop every variable and the call stack), and `NXIDE_SCRIPT="..."` does commands
one after another, each when nothing is being built or run: `build run debug go over into
out stop undo redo save quit comment all indent`, `mark:LINE`, `line:LINE`, `type:TEXT`
(`_` a space, `|` Enter), `text`, `problems`, `wait`. Under `rvc_cpu`, with `nano-X -p &`:

    NXIDE_PRINT=1 NXIDE_SCRIPT="mark:9 debug over over out into go quit quit" nxide

must stop at line 9 in `square` with `n = 1`, then line 11 with `result = 1`, then line 20
in `main`; and `NXIDE_SCRIPT="line:16 type:int_broken_=_; build problems undo text run quit
quit" nxide /root/hello.c` must name an error at line 16, give the text back as it was, and
print the program's six lines. Two seconds for both.

More of the script, for what those two do not reach: `example:NAME`, `open:PATH`, `new`,
`find:TEXT`, `key:NAME` (`up`, `s-down` with Shift, `c-a` with Ctrl, `enter`, `f9`...),
`menu:N` (opens and closes a menu), `cut copy paste word unindent panel numbers`, `where`,
and `storm:N`: N keys and commands by a fixed sequence of chances, through the functions a
hand's go through, with the cursor and the selection checked after each; then every change
taken back must leave the text as it was and every one made again as the storm left it. A
change to `text.h` or to the keys is tested with

    NXIDE_SCRIPT="example:home storm:600 example:shader storm:500 example:cube storm:500 quit quit" nxide

(20 s under `rvc_cpu`), which also opens one file after another: the first build crashed at
the second file opened, its text put in at the first one's cursor. With `NXIDE_PRINT` every
command prints where it left the cursor and the history, and at its start the IDE prints
where its keys and menus are across the window, for a test that clicks (`nxkey X,Y`: the
window's client begins 3, 23 inside its frame). A program that opens a window covers the
IDE's keys where it lands: a test that clicks Stop runs a program that has none.

The IDE is also tested started as the desktop starts it, its children's ends ignored
(`sh -c 'trap "" CHLD; NXIDE_PRINT=1 NXIDE_SCRIPT="build quit quit" exec nxide'`): the bar
ignores SIGCHLD, a started program inherits that, and the first IDE never saw its build end.

`python tools/test_all.py ide` runs the IDE's cases: `nxide-c` (the two above), `nxide-js` (pi
run), `nxide-py` (`example:primes mark:17 debug into over over out go`: stops at 17, 7, 8, 9 in
`sieve`, 18 with `primes` in Variables), `nxide-problems` (an error running and one of syntax in
each script), `nxide-tree` (no tree without a project) and `nxide-storm` (a storm in each language). The window itself is a snapshot's layer:
the stopped line is `ffff96`, a mark `c81414`, an error's bar `dc0000`.
