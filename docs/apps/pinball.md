# Pinball

`nxpinball` (`linux/apps/nxpinball.c`) is a pinball table for the desktop, after Windows'
3D Pinball but flat and smaller: the Start menu's Games has it. It is built with the other
programs of `linux/apps/build.sh`.

## Playing

Space (or Enter) starts a game of three balls. The ball waits on the plunger in the lane on the
right: hold Space or Down to draw the plunger back (0.8 s for all of it) and let go to launch;
a short pull drops the ball into the top right of the table, a long one sends it round the
orbit at the top. Z, Left or the left Ctrl key work the left flipper, `/`, Right or the right
Ctrl key the right one. P or Escape pauses (so does taking the keyboard away from the window),
F2 starts a new game, F10 opens the menus.

| What | Points |
|---|---|
| a pop bumper | 100, and 10 to the bonus |
| a slingshot's kick | 10 |
| a top lane lit | 500, and 250 to the bonus; a flipper moves the lit lanes round with it |
| all three lanes | 5,000, the bonus multiplier one up (to 5 x), the lanes out again |
| a drop target | 750, and 500 to the bonus |
| all three targets | 5,000, and 2,000 to the bonus; they come up 1.5 s later |
| the orbit (right to left over the top, not the launch) | 2,500, and 1,000 to the bonus |
| a ball drained | the bonus times the multiplier; both start again |

The best score is the registry's `nxpinball.high` (`docs/system/persist.md`).

## The physics

The table is 300 x 480 units, y downwards, drawn at the scale the screen's height leaves
room for (1.05 pixels a unit on 800 x 600). Its time goes in fixed steps of 1/120 s, four a
frame of 30 a second; a step is cut into as many moves as keep the ball's from being longer
than 2.5 units (its radius is 7), counting what the flippers' tips move too, so it cannot pass
through a wall or a flipper at any speed (the speed is held under 1,500 units a second).
Gravity along the table is 520 units/s².

Walls, guides, posts, the slingshots and the drop targets are line segments with a thickness;
the bumpers are circles; a flipper is a capsule narrowing from 7 units at its pivot to 4 at
its tip, turned at 26 radians a second up and 16 down. A contact pushes the ball out along
the normal and takes away the speed into the surface less a share of it (the restitution:
0.5 for walls, 0.3 for flippers); a flipper's surface moves at omega times the distance
from the pivot, and the bounce is worked in that moving frame, which is what throws the
ball. A bumper adds 300 units a second outwards, a slingshot's kicking side 260. A gate at
the top of the plunger's lane is a segment that only stops the ball from one side.

## Drawing

The table's picture and every look of every part of it that changes (a flipper in 14 angles,
a bumper lit and unlit, the lane lights, the targets up and down, the multiplier's lights, the
plunger in 8 places) are drawn once, at the start and when the desktop's look changes, into
one pixmap the GPU keeps (`docs/system/nanox.md`, "A pixmap the GPU keeps"). A frame is then a
copy from it for each part whose look changed, a copy of the table under where the ball was
(with the parts there over it again), and the ball as three filled circles. The panel's
numbers are drawn when they change.

## Checking it

`PINBALL_DEMO=N nxpinball` plays N frames itself as fast as it can (the flippers when the
ball comes near them, the plunger held a while for each ball), the scores not read or written,
and prints `pinball: N frames, G games, ball B, high H, score S state SUM`. The test set has
it (`tools/test_all.py nxpinball`). `PINBALL_HOLD=near` or `over` then plays on until the ball
is near the flippers or the game is over and leaves the window as it is, for a picture.

## What it costs

`PINBALL_DEMO=1800` from `pinball: go` to the end is 37.1 million instructions on core 0 with
the window system's, 20,600 a frame (`rvc_cpu`, 31 October 2026). It plays 269 frames a second
(calibrated, 31 October 2026); the 30 the game draws take about a ninth of the first core.
