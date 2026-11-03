# Solitaire

`nxsolitaire` (`linux/apps/nxsolitaire.c`) is Klondike, the patience of seven columns, for the
desktop: the Start menu's Games has it, after Pinball. It is built with the other programs of
`linux/apps/build.sh` (3 November 2026). The cards' faces, their back and the table are drawn by
the program itself, once, into a sheet it copies from; nothing of another product's cards, layout
or pictures is in it. Its icon is two cards fanned (`tools/draw_icons.py`).

## Playing

The four foundations at the top right are built up by suit from the ace to the king; in the seven
columns a card goes on one a rank higher of the other colour, and only a king goes on an empty
column. The stock at the top left turns over one card, or three (Game, Draw one / Draw three:
the registry's `nxsolitaire.draw`), and turns the waste back over when it is empty, as often as
wanted. A column's new top card is turned face up by itself.

| What | How |
|---|---|
| move cards | a click picks a card and those over it (a yellow edge round them), a click on where they go puts them there; or a drag |
| send a card up | a double click on it, the right button, or the bar's Up (the picked card, else the lowest card that can go) |
| undo | Undo, Ctrl+Z, U or Backspace: a move at a time back to the deal |
| finish | Finish or F puts up every card that can go, one every 70 ms; it starts by itself after a move that leaves nothing hidden and nothing in the stock or the waste |
| a new game | New or F2 (a random deal); Game, Deal again plays the same cards over |

### For a hand

The game is meant for a beam or a hand in the world as much as for a mouse (porting-games,
"Controls for VR"): every move can be made by clicks alone, a press that moves less than 8
pixels is a click (a hand's steady pointer holds a press for 7), a press is given its meaning at
the release, a drag that ends where its cards do not fit puts them back, and the buttons are
60 x 24 pixels. Nothing is done at a screen's edge. A double click is two clicks on the same
card within 500 ms. None of this has been tried in a headset.

## What is kept

| Where | What |
|---|---|
| the registry, `nxsolitaire.draw` | 1 or 3 |
| `/persistence/nxsolitaire/game` | the game in play, written when the window closes (Exit, Alt+F4, the caption's X, Q): its deal's number, the draw, the time, every pile and the moves Undo can take back (2,048 at most). The next start takes it up again; a won game or one with no move leaves none. Some 300 bytes and 12 bytes a move |
| `/persistence/nxsolitaire/stats` | games played, won, wins in a row and the most, the fastest win and the fewest moves: Game, Statistics shows them and can reset them. A deal counts as played at its first move; a deal left unwon ends the run of wins when the next is begun |

The host keeps `/persistence` only when asked (`docs/system/persist.md`).

## How it is drawn

The window is 500 x 468 pixels, a card 60 x 84. Every face (52), the back and the three empty
places are drawn into one pixmap of 780 x 420 at the start and when the look changes; a card on
the table is one copy from it. The table is made whole in a picture of the window's size and the
part that changed is copied to the window: all of it for a move, the strip at the bottom for the
clock (once a second while a game is played, nothing while it waits), and while cards are
carried the rectangle that holds where they were and where they are. The pointer's moves are asked
for only while a button is held (`ui_moves`). A column whose shown cards would pass the window's
bottom brings them closer, 8 pixels at the least.

A deal is the pack shuffled by its number (a linear congruential generator, Fisher and Yates),
dealt row by row into the columns, the rest the stock: `python tools/solitaire_deal.py N` prints
the same deal.

## Checking it

`NXSOLITAIRE_PRINT=1` prints each move with every pile (`##` a card face down) and where each
pile's top card is in the window; `=2` the moves only. `nxsolitaire N` deals deal N, and
`NXSOLITAIRE_DRAW` takes the draw without the registry. `NXSOLITAIRE_DEAL=nearly` deals every card
up to the jacks home and the queens on the kings, a game Finish ends in 8 moves.

- `tools/test_all.py nxsolitaire`: deal 15 played by `nxkey`'s pointer: 6c dragged onto 7d, 5c
  picked and put on 6h (5h turned up), Ah sent up by a double click (6d turned up), the stock
  clicked and that undone by the Undo button, 8h dragged onto 5h where it does not fit and put
  back; the window closed (Alt+F4), the game taken up again with the same piles and undone once
  by the U key. Each step's piles are checked, not pixels.
- `tools/test_all.py nxsolitaire-finish`: the nearly won deal ended by the Finish button, then
  again by a queen's double click that starts Finish by itself: 8 moves to 52 cards home both
  times, and the statistics 1 played, 1 won, then 2 and 2 with a run of 2.

A capture of deal 15 after three draws of three and a drag put back, and of the desktop's
Start menu with the game in it, were looked at under `rvc_harness --cpu --warp`.
