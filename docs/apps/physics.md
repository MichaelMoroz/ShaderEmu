# Rigid boxes on every core

`nxphys` (`linux/physics`; Start menu: Other, "Physics (every core)") is a room of rigid
boxes: a wall of bricks with a heavy box thrown at it, towers, a heap let fall. Every worker
core (`multicore.md`) moves its share of the boxes, and nothing in it needs a lock: the
machine has none to give.

    wsl -- bash linux/nommu/build.sh physics/build.sh     (after Nano-X's build)
    wsl -- bash linux/nommu/root.sh
    pwsh tools\make_image.ps1
    / # nano-X -p & nxphys

    arrows  turn the view          space  throw the heavy box      1 2 3  a wall, towers, a heap
    w a s d  walk the view about   Enter  the next scene           r  the scene again
    c  each box in its core's colour     p  stop and go            q, Escape  leave
    9  all the workers             0  core 0 alone           W  the workers asked for, or none

The bar under the picture has a button for some of those, and a line of figures. In the
picture the pointer takes a box by where it is pressed, carries it and throws it; from a
holodeck's seat both hands do. All of it is in "The controls" below. The window may be any
size: the picture has all of it but the bar, and the buttons share the bar's width.

## What the workers allow

A core sees another's stores one pass later, and 16 bytes have one writer a pass. So a
solver that applies a pair's push to both bodies, or goes through the contacts one after
another, or sorts bodies into shared bins, cannot be shared out. What can is anything
written as a gather: a core reads what the last phase left and writes only what is its own.

And every stage that must follow another costs a pass, whatever it computes, because that is
how long a core waits to see another's stores. So a step is made of as few stages as will do:
one.

## Who owns what

- **Body i is share `i mod n`'s**, and grid cell c is share `c mod n`'s, n being the number
  of shares (one a worker; with shares of no bodies, n is those with bodies, and the grid is
  the others': "Shares with no bodies", below). Every record is whole 16 bytes (`physics.h`).
- **A body's state** (place, turn, velocity, spin: four texels) is in three buffers
  (`PHYS_BUFFERS`): a substep reads one and writes the next, and a look may still read the third.
- **What its neighbours need of it** (`struct body_pose`, eight texels in each buffer): its axes,
  and where `pose_guess` takes it to be next. Its owner leaves them with the state and sets
  `PHYS_POSED` in the state's key; a neighbour reads them there. A state the program writes itself
  has the bit clear and is worked out by whoever reads it.
- **A pair's contact** (`struct manifold`) is in both bodies' lists, each written by its own
  body. The points and the normal are the same bits in both, because the pair is always
  worked out with the lower-numbered body first.
- **The grid** is cells of 2 m with room for 39 bodies each.
- **What a look asks of every body round one** (`struct body_near`, two texels in each
  buffer): its place, its reach and whether it moves fast enough to wake a body, left by its
  owner with the state (`phys_nears()` for a state the program wrote itself).
- **Whether it sleeps** is the state's `quiet`: the substeps it has been still, up to
  `PHYS_SLEEP_AFTER` ("Bodies that sleep", below).

## A substep

One phase, a sixtieth of a second of the boxes' time. For each of its bodies a share:

1. **Looks for contacts**, every second substep (`look`): the 27 cells round the body, a
   sphere test, then box against box: the axis of least overlap (a face of either, or the
   cross of an edge of each), and for a face the other box's facing side cut to the face's
   outline, up to four points. Two slow bodies keep the contact they have, seven looks of
   eight.
2. **Moves the body as it would go**, and takes every other body to be where `pose_guess`
   says.
3. **Takes its part of every contact** (`settle_points`): its mass's part of what the pair
   has to undo. The other body takes the rest for itself, from the same numbers.
4. **Takes its velocity from where it ended.**

The last substep before a look also leaves the grid for it (`phys_grid`): each share scans
every body's cell number and keeps the bodies of its own cells. That is a counting sort made
as a gather. A radix sort is three stages (histogram, prefix, scatter).

A share begins substep p when every share has published p-1: it reads the others' counters
and ends its pass until they are there. One pass of waiting, and no core is the conductor.
That holds for a substep with a look and for the shares of the grid and
the pictures when they make one; the substep after a look waits only for the shares of the
bodies its own have contacts with (unless one of its bodies sleeps), and a share of the grid
with nothing to make waits for none ("Waiting for fewer", below).

## What a body does about its contacts, and why

Each line here is there because the thing without it fails.

- **All its points at once, then as much of the sum as serves them best.** Each point says
  what push would settle it alone; the pushes are added, tried, and scaled by a least-squares
  fit to what the points wanted. The order of the points is then nothing. Gone through one
  after another, the first point gets the most and a resting stack walks sideways.
- **Friction the same way.** Four corners that each stop a spin alone stop it three times
  when added: a box set down on the floor would spin itself into the air.
- **The push a point had is kept and given again first** (`a[3]` of the list's own copy; the
  floor's in `body_info`). Without it a stack sinks: a substep only moves a load one layer.
- **A point that has come apart has no push any more.** Carried into the air it throws boxes.
- **What a substep brought a body into is its velocity; what it was in already only moves
  it** (`speed` and `cur`). Otherwise getting out of a wall is a kick.
- **A slow neighbour is taken to stay where it is** (`pose_guess`). Guessed on by its last
  move, two resting bodies each answer the other's answer, and a wall of bricks shakes itself
  apart; a tower, where every push is straight down, does not show it. A fast neighbour goes
  on as it went, or a thrown box would give the brick it hits no speed.

Ways that do not work, so they are not tried again: a body's mass shared among its contact points
(four corners undo a quarter of what they should); the lower of two bodies counting as the heavier (a
brick that hangs over lifts the bricks on it); one job a substep a worker (giving 255 jobs out and
waiting for them is several passes of core 0: the workers have one job each for as long as the boxes
move).

- **A body that has been still for a third of a second sleeps** (`quiet`): under 5 cm a
  second and 0.1 radians a second for 20 substeps, its velocity is made nothing and it is
  left where it is, its state copied on, until a body faster than 20 cm a second (or turning
  faster than a radian a second, or held) comes within its reach. Its neighbours take it as
  they take any body, not as weighing all there is: a brick pressed between sleeping ones would go
  deep into the floor.

What it is not: a solver that converges. A load takes a substep a layer to arrive, so a tall
stack sinks a few millimetres before it holds, and a body resting on another does not press
on it as it should. Piles, walls and things knocked over look right; a see-saw would not.

## The bar, and a box in the hand

The window is a plain Nano-X window with the picture a window inside it, as the video
player's is: the bar is `ui.h`'s buttons (`linux/apps`), drawn by the window system, and the
GPU's picture never meets it. A button does what its key does (`act`).

- **Which box.** A press in the picture is a ray from the eye through that pixel
  (`ray`, from the view's own three directions), tried against every box in the box's own
  frame (`phys_pick`): three pairs of sides, on core 0, once a press.
- **Carrying it.** Core 0 writes which box, the point on it in the box's frame and where
  that point is wanted (a `struct phys_hold` of `struct phys`: two texels of core 0's own,
  which the workers only read; there are three, the pointer's and a sitter's two hands'). The
  box's own share moves that point a quarter of the way there each substep, 30 cm at most,
  before the contacts are settled: so a held box pushes what it is carried into, swings from
  where it was taken, and cannot be carried through the floor. Nothing but that share writes
  the box. With no box held a body's substep asks one word (`held`) and no more.
- **Throwing it.** A box's velocity is what it did in its last substep, the pull included,
  so a box let go flies on as the hand went: nothing is done at the letting go but the hold's
  `body` set to -1.
- **Where the pointer is** comes from the machine's words while the button is down
  (`docs/machine/input.md`), not from events: the press says where the window is on the display.
  The wanted place is the pointer's ray at the distance ahead of the eye the box was taken at.
  The press itself is where the pointer is when the event is read, in a window looked up
  then (`ui_window_origin`), or the box would be taken beside the hand.
- **No waiting for the window system or for a substep.** Events are asked for when the
  machine's button or key words change and for a tenth of a second after (the event is on
  its way), else seven times a second.
  A view that moved is drawn at once from the picture shown last (`show_picture(1)`): the
  shares are asked nothing, so turning does not wait for the boxes, and works while they stand.
- **Nothing is said on the console as it runs** unless `PHYS_SAY=1`: a line there stutters the program.

## The controls

| | In the window | In a holodeck's seat, no headset | In a headset |
|---|---|---|---|
| take a box, carry it, throw it | the left button on it, held; let go, it flies as the pointer went | Ctrl held (the sitter's left mouse button): the box the view's middle is on | a hand's trigger or grip: the box at the hand (within 15 cm of the room), else the first one the hand points at; each hand its own |
| a held box nearer, further | the wheel, `+` `-` | `+` `-` | the wheel (a box taken by pointing) |
| turn the view | the right button dragged, the left one dragged on no box, the arrows | the mouse | the right stick; the arrow keys left and right |
| walk | `w` `a` `s` `d` (what the view looks at walks) | `w` `a` `s` `d` | the left stick, the way the head looks; the arrow keys up and down |
| fire the heavy box | a left click on no box: from the eye through that pixel; space: from the room's end | space: the way the view's middle looks | the right stick pushed up: from the right hand, the way it points |
| the scene again, the next one | `r`, `1` `2` `3`, Enter | the same | the right stick pulled down; the jump button (Enter) |
| stop and go | `p` | `p` | |

- **A sitter is known by the host's words** (`seglHostState()`, `docs/world/holodeck.md`: bit 4 of
  word 0, bit 5 for tracked hands), read every turn of core 0's loop (`seat`). The view is
  then theirs: level, from where they are (`body_at`), 0.93 m of the room over the floor,
  which is the seat's eye point over the room's, so the two floors are one. A metre of the
  room is `metre` of the scene's (word 0's scale, as Quake reads it): at the room's first
  scale 4.8, so the sitter is a giant before a wall they can reach across. They are put a
  step and a half before what the view looked at.
- **A hand** is its place from words 4 and 6 (4 mm of the room a unit, from the eye, in the
  body's frame) and its trigger and grip. The right hand points as its beam does (word 7).
  Of the left one only the place is told (word 5 is the sticks), so it points along the line
  from the eye through it. A box taken by pointing stays as far along the hand's way as it
  was. Both hands at once have a hold each (`holds[1]`, `holds[2]`).
- **The sticks are word 5**, as the games read them: the left one walks (1.2 m of the room a
  second), the right one turns (140 degrees a second pushed home) and, pushed up or down
  with nothing sideways, fires and begins again.
- **Without a headset** the mouse's counter (word 4) turns the view, 0.066 degrees a count,
  and the head's tilt is the aim's rise; the keys are the window's, and Ctrl takes.
  A sitter leaves by standing up (End): Escape and Q, beside W and A, quit nothing while seated.
- **The world knows the program** (`GAME_PHYS`, 8, in `gameid.h`): `EmuGamepad.cs`'s `GamePhysics`
  (8) is the HANDS scheme, the sticks sent as sticks and no key but Enter, and a hand whose beam is on
  the console takes nothing (`EmuHolodeck.cs`). A sitter with hands does not leave by the left grip.
- **A window's keys that are held** (`w` `a` `s` `d`, Ctrl, a sitter's arrows) go by the
  key's release, which the window asks for; a window that loses the keyboard lets all go.

Not tried in a headset, nor from a seat in the world: any of it. On `rvc_cpu` with the
host's words given (`--host`, `--host-mouse`): "Checking it".

A pull is out of line and by a copy of the pose: called by the pose's address it costs a substep more
than it should, the pose leaving its registers.

## The picture

When core 0 asks (once a substep of the boxes' time at most; 25 times a second with fewer than
64 shares) each share writes its boxes (with 64 shares or more, the shares of the pictures
write them all: "A picture every substep", below) as quads straight into GPU memory, 24 compact float
vertices a box, of the substep named: the picture is of one state ("A picture every substep",
below). A face's colour is a texel of a table of 16 colours by
64 shades, and the worker picks the shade from the face's normal. Two buffers alternate, one
drawn while the other is made. Core 0 draws them with one command that stays (`seglKeptQuads`,
`docs/machine/gpu.md`) and never touches a vertex. It is 3D with `seglWorldUp(2)`, so a holodeck
shows it.

## What it costs

Instructions a box (`PHYS_COUNT=1` with `PHYS_WORKERS=0`, on `rvc_cpu`, the wall, 31 October 2026): 7,300
a substep with a look for contacts, 4,300 without.

120 substeps from the start on 255 workers, unpaced (`PHYS_STEPS=120`). The times are calibrated, 31
October 2026; the model's workers term rests on this one program, so they are rough. The instructions are
`rvc_cpu`'s of the whole run with the window system's start.

| | 120 substeps | all cores, million instructions |
|---|---|---|
| the wall, 206 boxes | 627 ms | 178 |
| the towers, 109 boxes | 360 ms | 82 |
| the heap, 400 boxes | 985 ms | 424 |

It runs no faster than time goes: core 0 lets the shares go a substep on for every sixtieth of a second
(`PHYS_HZ`). `python tools\game_par.py nxphys` measures a run under it.

A substep takes as many passes as its longest share, rounded up, and one more, so with a box a worker
the slowest box sets the pace. `PHYS_COUNT=1` prints the dearest share's sum and how many substeps took
how many passes: the wall's are mostly three or four passes with a look and two without.

- **The look** first collects the bodies within reach (too far along x alone is too far; a
  neighbour's margin and radius together are in its pose, `reach`), then goes through those;
  the old list is left as it is until the new one is known. `struct body_pose` is eight
  texels, two of them nothing, so that a neighbour's is found by a shift.
- **A point that is clearly apart is left without working it out**: its depth is first
  guessed from the bodies' own frames (the gap between the middles along the normal, the
  point's `b[3]`, and the other point along the normal in its body's frame), which is the
  real depth to within rounding. It assumes a state's `q` is a unit quaternion, as the rest does.
- `box_box` tests a row at a time and leaves early for boxes apart; `clip` cuts both planes
  of an axis in one call; `phys_quads` shares corner sums between the faces across an axis.
- **Nothing is cleared at the start**: what `mcw_alloc()` gives is zeros, a page is there
  for the workers by one store (`there()`), the bodies' arrays only as far as the scene's
  bodies (`bodies_there()`).
- Built `-O3 -fsched-pressure`. A function whose frame passes 2 KB pays two or three instructions
  for every local it touches (`look` with its list of manifolds as a local array): the list is on
  `alloca`.
- **The first core has a share of the bodies only with 7 workers or fewer** (`OWN_UP_TO`;
  `PHYS_OWN=0/1` says otherwise). Share 0 is then core 0's, run in its own loop by the same
  `share_advance()` as a worker's job, and worker k has share k; with more workers the
  shares are the workers', worker k with share k - 1. Every share waits for core 0's counter when it
  has one, and core 0 has the kernel's clock and the window to see to.
- **A pass with nothing to do is a rest** (`mcw_rest()` in `pass()`; `multicore.md`, "A core
  that rests"): the kernel has the core, for another program or for its idle loop's work.
  `PHYS_COUNT=1` counts them.
- **The frame ends with `seglSwapEarly()`**: the list reads one of the two quad buffers, the
  colour table (written once) and the set's own matrices, and `ask_picture()` settles
  (`seglSettle()`) before the shares write a quad buffer again. Core 0's loop does not look at
  every share each turn (`first_own`, `shown_so_far`).

Sums a change must leave, `PHYS_WORKERS=0` (`rvc_cpu`, as on 31 October 2026): `PHYS_SCENE=4 PHYS_BODIES=13
PHYS_STEPS=300` `3d3402c2`, `PHYS_SCENE=3 PHYS_BODIES=150 PHYS_STEPS=90` `ed123738`, `PHYS_SCENE=2
PHYS_STEPS=60` `51fae5a5`, the still wall `52ecfca9`; and `PHYS_QSUM=1` (with `PHYS_STEPS`) prints a sum of
the last picture's quads, `f53c3f0c` for the wall.

## Switches, for tests

`PHYS_STEPS=N` ends after N substeps with a `physstat: done` line (the boxes' highest, the
deepest corner in the floor or a wall, the fastest box, a sum of every box's state);
`PHYS_HOLD=1` stays there and says `nxphys: holding`. `PHYS_SCENE=N` (4: one tower),
`PHYS_BODIES=N` (the heap's and the tower's), `PHYS_ROWS=N` (the wall's), `PHYS_STILL=1`
(nothing thrown), `PHYS_WORKERS=N`, `PHYS_SHARES=N` (more shares than workers: core 0 has
the rest), `PHYS_OWNERS=1` (the `c` key), `PHYS_SIZE=WxH` (the picture's size at the start), `PHYS_SAY=1` (a
`physstat:` line a second, and a line for a box held, let go or fired and for a view that moved), `PHYS_PICK=X,Y` (the box at that pixel held from the start, its
point wanted three metres up), `PHYS_HANDS="S:H,X,Y,Z,G ..."` (with `PHYS_STEPS`: hand H, 0 or
1, is at X, Y, Z of the room in millimetres after S substeps, its grip closed or open from
then, and on its way between two such; the shares stop at every substep of it until core 0
has told them the hands, so a run is the same whoever computes it), `PHYS_SEAT=1` (a line a
second of a sitter's place, turn and hands), `PHYS_PRINT=1` (every box's place and velocity),
`PHYS_COUNT=1` (instructions a box; with `PHYS_WORKERS=0`), `PHYS_SPIN=1` (a share that waits
turns in place), `PHYS_SERVICE=N` (so many shares with no bodies; 0: none), `PHYS_ORDER=ahead`
(the look a substep earlier: other sums), `PHYS_LAUNCH=N:K` and `PHYS_LAUNCH_OLD=1` ("A throw
without stopping the shares").

## Checking it

- **Whoever computes it, the same.** `PHYS_SCENE=1 PHYS_STEPS=120 nxphys` prints one sum
  with `PHYS_WORKERS=0`, with `PHYS_WORKERS=7` and with `PHYS_WORKERS=0 PHYS_SHARES=255`
  (`b28b77b4` on `rvc_cpu`; the shader machine's differs, its divisions being the card's,
  and is one sum there too).
- **What stands, stands.** `PHYS_SCENE=1 PHYS_STILL=1 PHYS_STEPS=120 PHYS_PRINT=1 nxphys`:
  no brick more than a centimetre from where the first substep left it, none over a
  millimetre a second. `PHYS_SCENE=4 PHYS_BODIES=13 PHYS_STEPS=300 nxphys`: highest 6899 mm.
- **What is thrown, hits.** `PHYS_SCENE=1 PHYS_STEPS=180 PHYS_PRINT=1 nxphys`: several bricks
  over a metre out of the wall's plane (z), none in the floor (`deepest` a few millimetres).
- **The workers afterwards.** `nxphys & sleep 5; killall -9 nxphys; sleep 1; mctest 15 2000`
  says PASS; `nxphys & sleep 6; nxkey q; sleep 2; pidof nxphys` finds none.
- **The bar and the hand.** `PHYS_SCENE=1 PHYS_STILL=1 PHYS_PICK=350,222 PHYS_STEPS=120 nxphys`
  says `box 117 held` and at its end that the held point is some millimetres from where it is
  wanted; `PHYS_PICK=20,20` holds none. `PHYS_SAY=1 nxphys & sleep 6; nxkey 181,462` (the Towers button,
  the window where the window system first puts it) and the next `physstat:` line has 109
  boxes. The whole way from the pointer: `rvc_cpu --pointer-sweep 0 --pointer-circle
  400,300,12 --pointer-button 1,6,20` with `PHYS_SAY=1 PHYS_STILL=1 nxphys` says a box is held,
  then let go. On the shader machine, the screen of a snapshot (`tools\snapshot_screen.py`):
  `nxkey - - - -` leaves the boxes 0.018 of the picture where they were 0.046, and
  `nxkey drag:684,501:560,420` (the frame's corner) leaves a picture of 516 x 319 with the bar
  under it and seven buttons 72 apart.

- **Hands, whoever computes them.** `PHYS_SCENE=1 PHYS_STILL=1 PHYS_STEPS=150
  PHYS_HANDS="10:1,-240,4760,0,1 50:1,-240,6500,3000,1 70:1,-240,6500,7000,0
  10:0,-4244,4760,0,1 60:0,-4244,-500,3000,1 100:0,-4244,-500,3000,0" nxphys` (hand 1 lifts a
  brick off the wall's top and throws it at 12 m a second; hand 0 takes another and presses
  it at the floor) prints one sum with `PHYS_WORKERS=0`, 7 and 255 (`0af17cf6` on `rvc_cpu`),
  and for each hand a line: the held point at most 146 mm (hand 1, as it swings) and 79 mm
  from where the hand wanted it, from 20 substeps after it was taken; box 194 let go at
  12,428 mm/s where the hand went 12,000; and the deepest corner in the floor or a wall at
  any substep, 98 mm (the thrown brick meeting the room's wall).
  Hands that take nothing (`PHYS_HANDS="0:0,0,5000,11000,0 150:0,0,5000,11000,0"`) leave the
  thrown wall's sum `b28b77b4`. On the shader machine the hands' run is one sum too, with the same
  three lines and 0 pages.
- **The pointer's throw, its click, the view.** With `PHYS_SAY=1 PHYS_STILL=1 nxphys` under
  `rvc_cpu --pointer-sweep 0`: `--pointer-circle 400,300,12 --pointer-button 1,6,20` says
  `box 99 held` and `let go at 1711 mm/s`; `--pointer-circle 400,330,0 --pointer-button
  1,6,6.2` (the floor, for a fifth of a second) says `a box fired from 10643 3199 3152`;
  `--pointer-circle 400,300,40 --pointer-button 2,6,8` says where the view is from after
  the right button's drag. `nxkey d+x` moves what the view looks at, `nxkey enter` is the
  towers (109 boxes in the next `physstat:` line).
- **The seat.** `PHYS_SAY=1 PHYS_SEAT=1 PHYS_STILL=1 nxphys` under `rvc_cpu --host
  59000031,0,0,0,06400000,0,40000000,0` (seated, hands tracked, the first scale; the right
  hand at the eye with its trigger held, pointing ahead): `box 174 held by hand 2, 7537 mm
  away`, a metre 4766, the left hand 1.9 units ahead of the eye, and `nxkey escape` does
  not end it. `--host 59000031,0,0,0,06400000,407f00,0,0` (the left stick forward, the right
  one half to the right): the place goes 5.7 units a second the way it faces and the turn
  35 degrees a second. `--host 59000012,0,0,0,0,0,0,0 --host-mouse 100` (no headset): the
  turn goes 6.6 degrees a second, `nxkey ctrl+x` says `held by hand 0` and `let go`, `nxkey
  space` `a box fired`.

A change to the solver is tried on a wall and on a tower both: a bug in it shows in one and not the
other.

## Workers that stop at pages

A worker stops at every page of the program's own code that no core has run yet, and at a third stack
page. `take_workers` reads the program's text before the jobs go out and the library has three stack
pages there for a worker (`STACK_TOUCHED`). `PHYS_COUNT=1` prints the pages brought for workers: it must
say 0. Fewer shares than workers with the same fullest share are slower.

### A share that waits ends its pass

- GPU memory is there from the start but not the program's to write until it has stored there, and a
  stopped worker waits for the first core's next look. `bodies_there()` stores to the quads' pages too.
- **A share with nothing to do ends its pass** (`mc_end_pass()`). `PHYS_SPIN=1` turns in place, for a
  comparison.

What holds the rest is the dearest share: a substep is as many passes as it takes. A store the write
cache has no room for is no way to end a pass: the commit then has every waiting worker's stores to go
through.

### Shares with no bodies, and what a substep's time is

- **It asks for all 255 workers, and those the bodies do not need have the grid and the
  pictures** (`service_of`, `PHYS_SERVICE=N`, 0 for none): three shares in sixteen, never so
  many that the fullest share gets another body, none under 64 shares. A share with bodies
  then scans nobody's keys and writes no quads. After a change to how bodies are shared out, measure
  191 to 255 workers again (`PHYS_WORKERS`).
- The grid's scan is three instructions a body, and the look at every other share's substep is four
  shares a turn.
- **What the time is**: a pass is as long as the worker that runs longest in it, so a substep is its
  dearest share's instructions, rounded up to passes (`PHYS_COUNT=1`, above). A worker's float arithmetic costs 1.6 times its other
  instructions, loads and stores included; a quotient or a root no more than a product.

`PHYS_ORDER=ahead` makes the look for a pair of substeps after the second substep of the pair
before (on that substep's boxes, when the share has already said they are made), not just
before the pair: the share with the longest look then holds no other up. It is not the order it runs
in: a contact is found a substep later, and boxes go deeper where the heavy box lands. Its sums on
`rvc_cpu`: the wall `bb5193e1`, its quads `20a3ca2c`, the same with 0 and 255 workers. A state is kept in
three buffers and the grid in two for it.

### Waiting for fewer

- **The substep after a look waits only for the bodies it needs** (`partners_ready`): it
  reads no body but those its own have contacts with, and every share began the look before
  it once all had made the substep before that, so no buffer it writes is still being read.
  The substep with a look, and a share's grid or picture, still wait for every share.
- **A share of the grid and the pictures with neither to make waits for nobody**: it reads
  nothing.
- **A share that waits asks a short function** (`share_advance`); the substep is out of
  line (`share_work`).
- **A look reads one texel of each body in the cells round its own** (`struct body_near`:
  the place and the reach, left with the state), where it read the state's key and the
  pose's reach.

`RVC_RACES` must say 0 undone (its reads as they were are the counters).

- `PHYS_COUNT=1`'s sums of grids and substeps are counters of `mcwx.h` (`mcw_add`), and the
  program's code is read with `mcw_touch_code()`.
- A share's job made by `mcw_call()` would be a 3 KB call record the first core makes and touches for
  each worker: not used.

### Bodies that sleep

A body that has been still for 20 substeps sleeps ("What a body does about its contacts"):
its substep is a copy of its state, its pose and its `body_near` into the next buffer, and a
look round it for a fast body (`woken`: the cells of the pair's grid, the reach of each).
Its share then waits for every share at the substep after a look too, since that look reads
bodies that are not its pairs. Each sum is one sum with 0, 7, 15, 63, 207 and 255 workers and with
`PHYS_SHARES=255` ("Checking it"). `PHYS_COUNT=1` says 0 pages, and `RVC_RACES` 0 undone on the wall, the
towers and the heap.

The heap never quite rests: part of it creeps at a few centimetres a second, which is the solver's (it
does not converge). A pair's contact is not worked out by one body of it, as the other would have it a
substep later and a fast body would go further into what it hits.

### A picture every substep

A substep is the boxes' sixtieth of a second and core 0 lets the shares go no faster than time
(`PHYS_HZ`). The substeps are work, not waits for core 0, which only asks for pictures and sets the limit.

With shares of the pictures (64 shares or more), a picture is asked for whenever the last
has been shown and the shares are at a newer substep, at most one a sixtieth of a second, and is
made beside the substeps: a picture's share waits until every body has made the substep named
(`ready`) and holds only at the substep before the look that would write that state's buffer
again, so no body waits for it. Core 0 names a substep three past the phases it sees (a share
may have made that many since), or the limit, which no share passes. Fewer than 64 shares make
their own bodies' pictures, 25 a second.

Each sum is the same with 0, 7, 15, 63 and 255 workers, `PHYS_COUNT=1` says 0 pages, `RVC_RACES` 0 undone
(its reads as they were are the counters and core 0's words, none of a picture's share reading a state).
The done line ends with the pictures shown.

Not done: more pictures than substeps (90 a second in a headset would need a substep shorter
than the boxes' sixtieth: other sums); waiting only for the cells round a body at a look (the
substeps are within a seventh of each body's own chain, and the grid is made from every body).

### A throw without stopping the shares

A throw, a box fired by a click or a hand, and the scene again (`r`, the scene's keys and buttons, Enter)
are a launch (`launch_ask`, `launch`), which does not stop the shares' jobs: core 0 lowers the limit to a substep
none of the shares can have passed (three past the phases it sees, or the limit), and when every
share waits there with its job kept and no picture is being made, writes the boxes into that
substep's buffer and lets them go on; the picture of that substep is asked for at once. A box
is launched at a substep without a look, and needs nothing but its own `body_near`
(`phys_near_of`): the grid the next look reads is made from it. The scene again is launched at
a look's, with every key, `body_near` and the grid made anew (`written_at`), from a copy of the
boxes as the scene put them (`starts`); another scene is put there and shared out again. A launch
does not draw the bar. Changing the workers, `PHYS_ORDER=ahead` and a stopped room still stop
and start the jobs; `PHYS_LAUNCH_OLD=1` stops them for everything.

`PHYS_LAUNCH=N:K` (with `PHYS_STEPS`) launches act()'s K at substep N (1 the scene again, at
the look's substep at or after N; 3 the heavy box thrown), so that a launch is the same
whoever computes it: `PHYS_SCENE=1 PHYS_STILL=1 PHYS_LAUNCH=20:3 PHYS_STEPS=120` gives
`dd5fe852` with 0, 7 and 255 workers, and the same the old way (`PHYS_LAUNCH_OLD=1`, which
makes the 100 substeps after it); `PHYS_LAUNCH=21:3` gives `22e9ab16`; `PHYS_SCENE=1
PHYS_LAUNCH=40:1 PHYS_STEPS=120` `8cccf9ba`; `PHYS_SCENE=3 PHYS_BODIES=150 PHYS_LAUNCH=30:1
PHYS_STEPS=90` `496c8792`, with `PHYS_COUNT=1` 0 pages and `RVC_RACES` 0 undone. Driven through its window
(the Towers button, Enter, space, a click fired, `p` twice, `r`, `1`, `q`): each launch says `launched at
substep N`, the boxes are 109, 400 and 206 as they should be, the click says `a box fired from 10643 3199
3152`, `pidof` finds no nxphys after `q`, and `mctest 15 2000` says PASS.
