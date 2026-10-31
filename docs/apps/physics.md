# Rigid boxes on every core

`nxphys` (`linux/physics`; Start menu: Other, "Physics (every core)") is a room of rigid
boxes: a wall of bricks with a heavy box thrown at it, towers, a heap let fall. Every worker
core (`multicore.md`) moves its share of the boxes, and nothing in it needs a lock: the
machine has none to give.

    wsl -- bash linux/nommu/build.sh physics/build.sh     (7 s; after Nano-X's build)
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

Until 13 October 2026 `w` was the colours and `a` all the workers.

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
  `PHYS_POSED` in the state's key; a neighbour reads them there, where it worked them out
  again for every contact (a tenth of a substep). A state the program writes itself has the
  bit clear and is worked out as before.
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
   eight: finding it again was most of a look.
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
Since 14 October 2026 that holds for a substep with a look and for the shares of the grid and
the pictures when they make one; the substep after a look waits only for the shares of the
bodies its own have contacts with (unless one of its bodies sleeps), and a share of the grid
with nothing to make waits for none ("Waiting for fewer", below).

## What a body does about its contacts, and why

Each line here is there because the thing without it was measured to fail.

- **All its points at once, then as much of the sum as serves them best.** Each point says
  what push would settle it alone; the pushes are added, tried, and scaled by a least-squares
  fit to what the points wanted. The order of the points is then nothing. Gone through one
  after another, the first point always got the most and a resting stack walked sideways.
- **Friction the same way.** Four corners that each stop a spin alone stop it three times
  when added: a box set down on the floor span itself into the air.
- **The push a point had is kept and given again first** (`a[3]` of the list's own copy; the
  floor's in `body_info`). Without it a stack sinks: a substep only moves a load one layer.
- **A point that has come apart has no push any more.** Carried into the air it threw boxes.
- **What a substep brought a body into is its velocity; what it was in already only moves
  it** (`speed` and `cur`). Otherwise getting out of a wall is a kick.
- **A slow neighbour is taken to stay where it is** (`pose_guess`). Guessed on by its last
  move, two resting bodies each answer the other's answer, and a wall of bricks shook itself
  apart; a tower, where every push is straight down, never showed it. A fast neighbour goes
  on as it went, or a thrown box would give the brick it hits no speed.

Tried and not kept:

- **A body's mass shared among its contact points.** Each corner then mostly turns its share
  of the body, and four corners undo a quarter of what they should.
- **The lower of two bodies counting as the heavier.** A stack stands, and a brick that
  hangs over lifts the bricks on it: no load goes down.
- **A body's inverse inertia turned into the room once a pose**, so that a push's effect
  needs no turning into the body's frame and back: 1.6% fewer instructions, another sum, and
  the still wall's fastest brick at 3 mm a second where it was 2.
- **One job a substep a worker.** Giving 255 jobs out and waiting for them is some six passes
  of core 0; the workers have one job each for as long as the boxes move.

- **A body that has been still for a third of a second sleeps** (`quiet`): under 5 cm a
  second and 0.1 radians a second for 20 substeps, its velocity is made nothing and it is
  left where it is, its state copied on, until a body faster than 20 cm a second (or turning
  faster than a radian a second, or held) comes within its reach. Its neighbours take it as
  they take any body. Taken by a neighbour to weigh all there is, a brick the heavy box
  pressed between sleeping ones went 42 cm into the floor, twice as deep as without sleep.

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
  then (`ui_window_origin`): by the event's own place the box was taken beside the hand.
- **No waiting for the window system or for a substep.** Events are asked for when the
  machine's button or key words change and for a tenth of a second after (the event is on
  its way), else seven times a second; asked for ten times a second, a press was that late.
  A view that moved is drawn at once from the picture shown last (`show_picture(1)`): the
  shares are asked nothing, so turning does not wait for the boxes, and works while they stand.
- **Nothing is said on the console as it runs** unless `PHYS_SAY=1`: a line there is a tenth
  of a second of core 0, and one a second was a stutter a second.

## The controls (13 October 2026)

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
- **Until the world knew the program** (`GAME_PHYS`, 8, in `gameid.h`) a headset's controls
  were the pointer's: word 5 was nothing, the left stick the arrow keys, which is why a
  sitter's arrows walk and turn; the left grip was Esc, which a sitter with hands does not
  leave by; the triggers were the pointer's buttons. The world has it now: `EmuGamepad.cs`'s
  `GamePhysics` (8) is the HANDS scheme, the sticks sent as sticks and no key but Enter, and a
  hand whose beam is on the console takes nothing (`EmuHolodeck.cs`).
- **A window's keys that are held** (`w` `a` `s` `d`, Ctrl, a sitter's arrows) go by the
  key's release, which the window now asks for; a window that loses the keyboard lets all go.

Not tried in a headset, nor from a seat in the world: any of it. On `rvc_cpu` with the
host's words given (`--host`, `--host-mouse`): "Checking it".

With nothing held the boxes are as they were: every sum above the same, `PHYS_COUNT=1` 0
pages, the wall's dearest share 1,561,829 instructions with a look where it was 1,563,575
(the pull is out of line and by a copy of the pose: called by the pose's address it cost
2% of a substep, the pose leaving its registers). On the shader machine, 120 substeps,
the program before and this one in turn: the wall 796 and 707 ms against 795 and 712, the
towers 421 and 372 against 409 and 373, the heap 986, 994, 987 against 979, 1,011, 991. A
held frame of the wall after 120 substeps (the interpreter with the GPU's passes): no pixel
of the picture differs.

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

Instructions a box (`PHYS_COUNT=1`, on `rvc_cpu`):

| | a substep | a substep with a look |
|---|---|---|
| a tower's cube, 8 points | 5 thousand | 12 thousand |
| a wall's brick, 16 points | 9 thousand | 17 to 18 thousand |

Where a wall's instructions go (a line profile of `physics.c` over an `rvc_cpu --pc-log`):
finding and making the contacts a quarter, building the points from them a quarter,
`settle_points` a fifth, the picture's quads 6%. A worker's passes are full ones (95% of
8,192 instructions on the shader machine): the stores of a substep fit its 3 KB a pass.

A substep takes as many passes as its longest share and one more, so with a box a worker the
slowest box sets the pace: some four passes a substep.

On the shader machine (D3D11, fxc2, RTX 5090, 255 workers), with nothing else on the card and
with another program taking 37 to 53% of it:

| | substeps a second | of real time | with the card shared |
|---|---|---|---|
| the wall, 206 boxes | 60 to 62 | all of it (it goes no faster) | 40 to 45, 0.7 |
| the heap, 400 boxes | 47 to 56 | 0.8 to 0.9 | 27 to 35, 0.5 |
| a heap of 1,000 as it lands | 11 to 18 | 0.2 to 0.3 | |

Forty-eight substeps of a wall of two rows (42 boxes): 16.6 s on core 0 alone, 2.2 s with 7
workers, 0.63 s with 255, the same sum each time. It runs no faster than time goes: core 0
lets the shares go a substep on for every sixtieth of a second.

### A fifth fewer (12 October 2026)

A wall's substep on the first core alone went from 2,903 thousand instructions to 2,320, and
with seven workers the workers' from 2,895 to 2,303; a brick's substep with a look is 13.6
thousand where it was 17.5, without one 7.8 where it was 8.7. To the first substep: 4.4
million instructions became 1.4 with 7 workers (2.3 with 255, of which 0.9 is `mcw_open`).

- **The look** first collects the bodies within reach (too far along x alone is too far; a
  neighbour's margin and radius together are in its pose, `reach`), then goes through those;
  the old list is left as it is until the new one is known. `struct body_pose` is eight
  texels, two of them nothing, so that a neighbour's is found by a shift.
- **A point that is clearly apart is left without working it out**: its depth is first
  guessed from the bodies' own frames (the gap between the middles along the normal, the
  point's `b[3]`, and the other point along the normal in its body's frame), which is the
  real depth to within rounding; `SURE` and a hundred-thousandth of the bodies' places and
  sizes is some ten times that. Three points of five are apart (the bricks beside and the
  one above). It assumes a state's `q` is a unit quaternion, as the rest does.
- `box_box` tests a row at a time and leaves early for boxes apart; `clip` cuts both planes
  of an axis in one call; `phys_quads` shares corner sums between the faces across an axis.
- **Nothing is cleared at the start**: what `mcw_alloc()` gives is zeros, a page is there
  for the workers by one store (`there()`), the bodies' arrays only as far as the scene's
  bodies (`bodies_there()`): 6.5 MB of `memset` gone.
- Built `-O3 -fsched-pressure`: 5% fewer than `-O2`. A function whose frame passes 2 KB pays
  two or three instructions for every local it touches (`look` with its list of manifolds
  as a local array): the list is on `alloca`.

- **The first core has a share of the bodies only with 7 workers or fewer** (`OWN_UP_TO`;
  `PHYS_OWN=0/1` says otherwise). Share 0 is then core 0's, run in its own loop by the same
  `share_advance()` as a worker's job, and worker k has share k; with more workers the
  shares are the workers', worker k with share k - 1. Every share waits for core 0's
  counter when it has one, and core 0 has the kernel's clock and the window to see to:
  passes for 119 substeps of the wall, with the share against without, were 8,790 for
  10,596 with 3 workers, 4,437 for 4,780 with 7, 2,346 for 2,278 with 15 and 356 for 342
  with 255. On the shader machine with 255 workers core 0 therefore runs half of its
  passes' instructions (it only waits; 57% of its passes end in its loop's `pause`); with a
  share it ran 76% of them and the wall took 4% more passes.
- **A pass with nothing to do is a rest** (`mcw_rest()` in `pass()`; `multicore.md`, "A core
  that rests"): the kernel has the core, for another program or for its idle loop's work,
  where the loop paused in user mode. Of 3,229 passes of the wall on 207 workers the first
  core rests in 2,862 and runs 99.8% of the other 367 (it ran 27.4% of all of them when
  each wait was its own pause); the substeps a second are the same 61 to 63, and 120
  substeps took 1,329 ms where they took 1,344. `PHYS_COUNT=1` still counts them.
- **The frame ends with `seglSwapEarly()`**: the list reads one of the two quad buffers, the
  colour table (written once) and the set's own matrices, and `ask_picture()` settles
  (`seglSettle()`) before the shares write a quad buffer again. Core 0's loop no longer
  looks at every share each turn (`first_own`, `shown_so_far`), and `PHYS_COUNT=1` also
  says how many passes the first core ended with nothing to do.

Tried and not kept: `settle_points` out of line (3% more); the points built in two loops,
one for each pair of matrices (no fewer); the cuts' distances carried into the choice of
points (more, for the keeping). Still there then: the workers spin for the other shares'
counters (5% of their instructions with seven, most of them with 255; since 13 October a share
that waits ends its pass, and since 14 October it waits for fewer: below).

More sums a change must leave, `PHYS_WORKERS=0`: `PHYS_SCENE=4 PHYS_BODIES=13 PHYS_STEPS=300`
`3d3402c2`, `PHYS_SCENE=3 PHYS_BODIES=150 PHYS_STEPS=90` `ed123738`, `PHYS_SCENE=2
PHYS_STEPS=60` `51fae5a5`, the still wall `52ecfca9`; and `PHYS_QSUM=1` (with `PHYS_STEPS`)
prints a sum of the last picture's quads, `f53c3f0c` for the wall. (Before bodies slept, 14
October 2026: `e373ce02`, `a1b597fa`, `d15836d6`, `3a16b24c`, `bc7f3663`.)

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
  (`b28b77b4` on `rvc_cpu`, `e444ba29` before bodies slept; the shader machine's differs, its divisions being the card's,
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
  it at the floor) prints one sum with `PHYS_WORKERS=0`, 7 and 255 (`0af17cf6` on `rvc_cpu`; `4ca48d6c` before bodies slept),
  and for each hand a line: the held point at most 146 mm (hand 1, as it swings) and 79 mm
  from where the hand wanted it, from 20 substeps after it was taken; box 194 let go at
  12,428 mm/s where the hand went 12,000; and the deepest corner in the floor or a wall at
  any substep, 98 mm (the thrown brick meeting the room's wall; the pressed one alone 34).
  Hands that take nothing (`PHYS_HANDS="0:0,0,5000,11000,0 150:0,0,5000,11000,0"`) leave the
  thrown wall's sum `b28b77b4`, and its deepest corner at any substep is 216 mm (206 before bodies slept): no hand
  put a box deeper than a throw does. On the shader machine the hands' run is one sum too
  (`28d9b9c1` with 255 workers, twice, and with 15), the same three lines, 0 pages.
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

A change to the solver is tried on a wall and on a tower both: every bug in it so far showed
in one and not the other.

## Workers that stopped at pages (12 October 2026)

With 255 workers every worker came, in the first substep, to pages of the program's own code
that no core had run yet, and each stopped there until the first core had read the page: 921
stops, and a third stack page each on top. `take_workers` now reads the program's text before
the jobs go out and the library has three stack pages there for a worker (`STACK_TOUCHED`).
`PHYS_COUNT=1` prints the pages brought for workers: 1,126 before, none now. The first
substep went from 92 ms to 41, a substep from 7.5 ms to 6.2, 120 substeps from 1,061 ms to
843; the sums are the same.

A worker whose share is waiting for the others could not end its pass then (the pause hint is
a sleep until its job word changes; it ends it with `mc_end_pass()` since the next section), so the instructions of all cores are the cores times the passes
whatever the work: 6.35 million a substep before, 5.26 now, of which 2.2 are work
(`PHYS_WORKERS=0`: 2.83 to 2.18). Fewer shares than workers with the same fullest share were
tried and are slower (9.1 ms a substep against 6.1 on the wall).

### A share that waits ends its pass (13 October 2026)

240 substeps on the shader machine, the build before and this one in turn: the wall 2,398 and
2,411 ms to 2,191 and 2,198, the towers 1,364 and 1,355 to 1,112, the heap 3,270 and 3,376 to
2,958 and 2,972; the wall's first 120 substeps 1,240 to 1,281 ms to 1,109. The sums are the
same.

- **`PHYS_COUNT=1` said 606 pages, not 0.** Every worker stopped at its mailbox's page at its
  first step (the library's part: `multicore.md`), and at its boxes' quads in each of the two
  pictures' buffers: GPU memory is there from the start but not the program's to write until
  it has stored there, and a stopped worker waits for the first core's next look, up to 50
  ms. `bodies_there()` now stores to the quads' pages too. It says 0 again.
- **A share with nothing to do ends its pass** (`mc_end_pass()`), where it turned in place
  to the pass's end. The passes are as many (755 for 757), and the ones in which no worker
  runs a whole pass are 2.0 to 2.3 ms where the others are 3.2 to 3.4: a third of the
  workers' instructions were the work (39% run of 255 times 8,192 a pass, where 77% were
  run). `PHYS_SPIN=1` turns in place: the wall 2,293 and 2,298 ms, the towers 1,244 and
  1,251, the heap 3,111 and 3,128, and three boxes on 207 shares 1,442 ms for 600 substeps
  against 1,075.

What holds the rest is the dearest share: a substep is as many passes as it takes, and
with a look that is 26 to 43 thousand instructions for the wall once it is hit (the mean is
13), 41 to 50 for the heap's two boxes a share (the mean 24 to 27); without one 14 to 17.
So the wall's pair of substeps is four passes and two, the heap's six and two.

Tried and not kept: a store the write cache has no room for as the pass's end (thirteen
texels of one bucket): it ends the pass, and the commit then has every waiting worker's
stores to go through: three boxes on 207 shares took 1,985 ms for 600 substeps against
1,436. And float constants from integer registers (`raytrace.md`): the same within 0.3%.

With this, 240 substeps of the wall by workers asked for (`PHYS_WORKERS`): 2,453 ms with 191,
2,192 and 2,198 with 207, 2,194 with 223, 2,198 with 239, 2,211 with 255. It still asked for
207 (255 since the next section).

### Shares with no bodies, and what a substep's time is (13 October 2026, later)

120 substeps on the shader machine (RTX 5090, the tick of that day), the program of the
morning before, the build above and this one in turn: the wall 1,072 to 1,143 ms, 894, and
849 to 880; the heap 1,497 to 1,538, 1,191, and 1,126 to 1,137; the towers 615, 468, and 445.
The wall's 240 substeps: 2,020 ms, and 1,716. The sums are the same, on `rvc_cpu` and there.

The wall's 120 substeps on 19 October 2026 on an RTX 5070 laptop (`docs/machine/speed.md`): 1,494 ms
on the shader machine, 1,499 on `rvc_lean`, 426 on the compute machine, 543 on the CPU harness.

- **It asks for all 255 workers, and those the bodies do not need have the grid and the
  pictures** (`service_of`, `PHYS_SERVICE=N`, 0 for none): three shares in sixteen, never so
  many that the fullest share gets another body, none under 64 shares. A share with bodies
  then scans nobody's keys and writes no quads. The wall by workers asked for, 240 substeps:
  2,328 ms with 191, 1,740 with 207, 1,704 to 1,718 with 223 to 255; the heap's 120: 1,181
  with 207 and 223, 1,133 with 255.
- The grid's scan is three instructions a body (881 a share for the wall where it was
  1,531), and the look at every other share's substep is four shares a turn.
- **What the time is**: a pass is as long as the worker that runs longest in it (2.5 to 2.9
  ms when one runs all of it, 0.3 to 0.5 when all only look), so a substep is its dearest
  share's instructions, rounded up to passes. `PHYS_COUNT=1` (with `PHYS_STEPS`) prints that
  sum and how many substeps took how many passes: the wall 1.57 million with a look and 0.78
  without, mostly four passes and two. A worker's float arithmetic costs 1.6 times its other
  instructions, loads and stores included; a quotient or a root no more than a product.

`PHYS_ORDER=ahead` makes the look for a pair of substeps after the second substep of the pair
before (on that substep's boxes, when the share has already said they are made), not just
before the pair: the share with the longest look then holds no other up, and the wall's 120
substeps take 771 to 805 ms, the towers' 413, the heap's 1,096. It is not the order it runs in: a
contact is found a substep later, and where the heavy box lands on the wall a brick was 48 cm
in the floor for some substeps where it is 19 cm (`PHYS_PRINT=1` now has every box's turn and
size; the deepest pair of boxes over substeps 44 to 56: 22 cm, and 33). At rest the two are
the same within 3 mm, and the towers' fall as deep either way (13 to 26 cm). Its sums on
`rvc_cpu`: the wall `094d8d9a`, its quads `044ab11c`, the same with 0, 7 and 255 workers
(now `bb5193e1` and `20a3ca2c`: "A picture every substep"). For it a state is
kept in three buffers and the grid in two (every run has three since).

Tried and not kept: every sixteenth share saying how far its sixteen are, for the last share
to read sixteen words and not 255 (the wall the same, the heap and the towers 2 to 3%
slower); `box_box` from one place and in line (1.4% more instructions); a pair of bodies
moving alike kept as two slow ones are (the wall's dearest look 5% less, the heap's none, and
boxes deeper in the floor); the look on another share, by a plan of what each cost before
(by the costs measured, within 1% of now: one look that is longer than planned is the
substep). Compiler flags: none within 0.5% of `-O3 -fsched-pressure` was better.

It asked for 207 workers, not all 255, until then (`WORKERS_ASKED`; `a` at its window and `PHYS_WORKERS`
give any number): with 207 a substep is as long as with 255 in every scene (the wall 6.2 ms
against 6.1, the towers 3.7 against 3.8, the heap 7.4 against 7.6; 191 is slower), and 48
cores are not turning in place. All cores a substep, the morning's build against this one:
the wall 6.35 million to 4.22, the towers 4.12 to 2.46, the heap 8.72 to 5.09; the start
59.3 million to 38.9, 50.7 to 37.9, 50.3 to 39.9.

### Waiting for fewer (14 October 2026)

Of the wall's instructions on all cores (`python tools\game_par.py nxphys`, 255 workers, 120
substeps from the first), a tenth was waiting: every share read all 255 other shares' counters
at every substep, 2.25 instructions each, and a share with nothing to do ran some 150
instructions before it ended its pass. The work itself is the same instructions whoever does it
(277.8 million with `PHYS_WORKERS=0`). Now:

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

All cores a substep, mean of three, the build before and this one (on `rvc_cpu`):

| | all cores a substep | core 0 | passes for 120 substeps |
|---|---|---|---|
| the wall (`game_par.py`) | 2,443k to 2,321k (-5.0%) | 15.1k, the same | 463 to 455 |
| the towers | 830k to 718k (-13.4%) | | 319 to 310 |
| the heap | 3,607k to 3,447k (-4.4%) | | 588 to 579 |

Waiting is now some 6% of the wall's instructions (the full look at the substep with a look
3%, shares with nothing to do 2%, the jobs' loop 1%). The sums are the same: all those of
"Checking it" and above with 0, 7 and 255 workers, `PHYS_ORDER=ahead`'s, the hands', the
still wall's and `PHYS_QSUM=1`'s; `PHYS_COUNT=1` says 0 pages, and `RVC_RACES` 0 undone (its
reads as they were are the counters). Not measured on the shader machine.

- `PHYS_COUNT=1`'s sums of grids and substeps are counters of `mcwx.h` (`mcw_add`), and the
  program's code is read with `mcw_touch_code()`.
- Tried and not kept: every sixteenth share saying its sixteen have made a substep, read
  where it is there and the sixteen otherwise (no fewer: the last shares finish in the
  pass before everyone looks); the shares' jobs made by `mcw_call()` (each worker's call
  record is 3 KB that the first core makes and touches: 83 more passes before the first
  substep); a body's pushes summed as its points are made rather than in `settle_points`
  (0.5% more: the floats leave their registers).
- Asked for 223 workers the wall is 2,274k a substep and 448 passes, the towers 685k and 301,
  the heap 3,395k and 586: it still asks for 255 (on the shader machine on 13 October the heap
  was 4% slower with 223, the wall the same).

### Bodies that sleep (14 October 2026, later)

A body that has been still for 20 substeps sleeps ("What a body does about its contacts"):
its substep is a copy of its state, its pose and its `body_near` into the next buffer, and a
look round it for a fast body (`woken`: the cells of the pair's grid, the reach of each).
Its share then waits for every share at the substep after a look too, since that look reads
bodies that are not its pairs. Every sum changed (the state's last word is `quiet`, and the
boxes go otherwise); each is one sum with 0, 7, 15, 63, 207 and 255 workers and with
`PHYS_SHARES=255` ("Checking it" has them). `PHYS_COUNT=1` says 0 pages, and `RVC_RACES` 0
undone on the wall, the towers and the heap.

All cores a substep on `rvc_cpu`, 255 workers, from the first substep (the program before
both changes of the day, then this one):

| | all cores a substep | passes |
|---|---|---|
| the wall, 120 substeps (`game_par.py`) | 2,443k to 1,427k (-41.6%); core 0 15.1k to 15.6k | 463 to 453 |
| the towers, 120 | 830k to 640k (-22.8%) | 319 to 311 |
| the heap, 120 (it is falling) | 3,607k to 3,460k (-4.1%) | 588 to 578 |
| the heap, 300 | 3,514k to 3,094k (-12.0%) | 1,230 to 1,191 |

The heap never quite rests: after 300 substeps 217 of its 400 boxes sleep, and the rest creep
at a few centimetres a second, which is the solver's (it does not converge). What is thrown
still hits: 180 substeps of the wall leave 7 bricks over a metre out of its plane (10 before),
none in the floor, and its deepest corner at any substep is 216 mm (206); the still wall has
no brick moving. Held frames of the three scenes before and after, by `rvc_harness --cpu
--warp`, look alike but for where the loose boxes lie.

Tried and not kept:

- **A sleeping body taken by its neighbours to weigh all there is** (they undo all of a
  contact with it): the bricks the heavy box pressed between sleeping ones went 42 cm into
  the floor, the wall's instructions 150.0 million where they are 171.2.
- **Twenty substeps made ten**: the wall 146.4 million, a tower's top a millimetre lower.
- **A pair's contact worked out by one body of it**: the other would have it a substep
  later. The pairs that are worked out again are those with a fast body (two slow ones keep
  their contact seven looks of eight), and for those a substep late is the heavy box 23 cm
  further into what it hits. Not made.

### A picture every substep (15 October 2026)

In the world the boxes went at 62 substeps a second and 21 pictures a second, whatever the
workers: a substep is the boxes' sixtieth of a second and core 0 lets the shares go no faster
than time (`PHYS_HZ`), and a picture was asked for at most every 40 ms. Run unpaced
(`PHYS_STEPS`), the wall's 120 substeps take about 2.4 passes a substep (`PHYS_COUNT=1`: with a
look mostly 3 and 4, without 2), and each is as long as its dearest share's one body: the
substeps are work, not waits for core 0, which only asks for pictures and sets the limit.
`game_par.py nxphys`'s 4.04 passes a frame has the start of 255 jobs and the program's exit in
it (3.08 to the `physstat: done` line), and on the card passes a second over substeps a second
counts the short passes of that exit with the long ones of the substeps.

Now, with shares of the pictures (64 shares or more), a picture is asked for whenever the last
has been shown and the shares are at a newer substep, at most one a sixtieth of a second, and is
made beside the substeps: a picture's share waits until every body has made the substep named
(`ready`) and holds only at the substep before the look that would write that state's buffer
again, so no body waits for it. Core 0 names a substep three past the phases it sees (a share
may have made that many since), or the limit, which no share passes. Fewer than 64 shares make
their own bodies' pictures as before, 25 a second.

| | before | after |
|---|---|---|
| the wall, in real time, `rvc_cpu` (card model) | 62 substeps, 21 to 23 pictures a second | 62, 58 to 60 |
| the towers | 62, 20 to 21 | 62, 59 to 60 |
| the heap | 62, 21 to 23 | 62, 57 to 60 |
| the wall on the card (RTX 5090, D3D11 + fxc2) | 62, 20 to 21 | 62, 60 to 62 |
| the heap on the card | 61 to 63, 20 to 21 | 61 to 63, 59 to 62 |
| the wall's 120 substeps unpaced, `rvc_cpu` | 789 ms, 4.05 passes a frame | 794 ms, 4.05, 25 pictures |
| the heap's 120 unpaced, `rvc_cpu` | 1,040 ms, 5.06 | 1,052 ms, 5.10, 26 pictures |
| the wall's 120 unpaced, card | 711 ms, 168.8 a second | 738 ms, 162.6 (one run each; 25 pictures where there were some 18) |

All cores' instructions a second in real time go up with the pictures (the wall's 76 million
to 88), core 0's do not (1.57 to 1.52 million). Every sum of "Checking it" and above is the
same with 0, 7, 15, 63 and 255 workers, `PHYS_QSUM=1`'s quads `f53c3f0c`, the hands' lines,
`PHYS_COUNT=1` 0 pages, `RVC_RACES` 0 undone (its reads as they were are the counters and core
0's words, none of a picture's share reading a state). `PHYS_ORDER=ahead`'s sums are now
`bb5193e1`, its quads `20a3ca2c`, with 0 and 255 workers. The done line ends with the pictures
shown.

Not done: more pictures than substeps (90 a second in a headset would need a substep shorter
than the boxes' sixtieth: other sums); waiting only for the cells round a body at a look (the
substeps are within a seventh of each body's own chain, and the grid is made from every body).

### A throw without stopping the shares (15 October 2026, later)

A throw, a box fired by a click or a hand, and the scene again (`r`, the scene's keys and
buttons, Enter) stopped every share's job, waited for 255 of them, and gave them out again: 65
to 85 ms from the key to the next picture, 88 to 122 for `r`, the pictures standing still
meanwhile. Now it is a launch (`launch_ask`, `launch`): core 0 lowers the limit to a substep
none of the shares can have passed (three past the phases it sees, or the limit), and when every
share waits there with its job kept and no picture is being made, writes the boxes into that
substep's buffer and lets them go on; the picture of that substep is asked for at once. A box
is launched at a substep without a look, and needs nothing but its own `body_near`
(`phys_near_of`): the grid the next look reads is made from it. The scene again is launched at
a look's, with every key, `body_near` and the grid made anew (`written_at`), from a copy of the
boxes as the scene put them (`starts`); another scene is put there and shared out again. A launch
does not draw the bar. Changing the workers, `PHYS_ORDER=ahead` and a stopped room still stop
and start the jobs; `PHYS_LAUNCH_OLD=1` does everything the old way.

From the key to the next picture, `rvc_cpu` (card model), the old way and the new in turn on
one build, a throw at 4 s and 6 s and `r` at 8 s:

| | throws | `r` |
|---|---|---|
| the wall, old | 77, 65 ms | 91 ms |
| the wall, new | 22, 22 ms | 32 ms |
| the heap, old | 79, 73 ms | 113 ms |
| the heap, new | 18, 28 ms | 40 ms |

`PHYS_LAUNCH=N:K` (with `PHYS_STEPS`) launches act()'s K at substep N (1 the scene again, at
the look's substep at or after N; 3 the heavy box thrown), so that a launch is the same
whoever computes it: `PHYS_SCENE=1 PHYS_STILL=1 PHYS_LAUNCH=20:3 PHYS_STEPS=120` gives
`dd5fe852` with 0, 7 and 255 workers, and the same the old way (`PHYS_LAUNCH_OLD=1`, which
makes the 100 substeps after it); `PHYS_LAUNCH=21:3` gives `22e9ab16`; `PHYS_SCENE=1
PHYS_LAUNCH=40:1 PHYS_STEPS=120` `8cccf9ba`; `PHYS_SCENE=3 PHYS_BODIES=150 PHYS_LAUNCH=30:1
PHYS_STEPS=90` `496c8792`. Every sum without a launch is as before, `PHYS_COUNT=1` 0 pages,
`RVC_RACES` 0 undone. Driven through its window (the Towers button, Enter, space, a click
fired, `p` twice, `r`, `1`, `q`): each launch says `launched at substep N`, the boxes are 109,
400 and 206 as they should be, the click says `a box fired from 10643 3199 3152`, `pidof`
finds no nxphys after `q`, and `mctest 15 2000` says PASS. Not measured on the card.
