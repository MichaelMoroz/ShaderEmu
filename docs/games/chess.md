# Chess

`nxchess` (`linux/chess`, Start menu: Games, Chess) is chess in a window, played with the pointer or the keys, with an
engine of its own that searches on the worker cores. It is 263 KB of the image (the flat program, with the book and the
pieces in it) and a 572-byte icon. At run time it takes 2 MB for the shared table and about 36 KB a worker.

## The game

- The board, legal moves as dots, a move by two clicks or a drag, promotion by asking, undo, new game, the move list in
  short notation, the last move marked, castling, in passing, mate, stalemate, the fifty moves and repetition.
- Keys: the arrows and Space or Enter move a square on the board, `u` is undo, `n` a new game.
- Seven strengths on the Strength menu: 2, 3 and 5 plies, then 2, 5, 15 and 60 seconds a move. Strength and side are the
  registry's `nxchess.level` and `nxchess.play`. The menu says Play, for playing either side, or both.
- A small book of 21 lines, chosen at random (`NXCHESS_BOOK=0` is none, `NXCHESS_SEED` fixes the choice).
- One window: `ui_single_instance()`. The workers are lent (`mcw_lend`): a program that wants them gets them, and the
  game goes on with the first core alone.

The pieces are `linux/chess/pieces.h`, made by `tools/make_chess_pieces.py` from DejaVu Sans's chess glyphs; white's open
glyphs lose their fine lines at 44 pixels, so white pieces get an edge of their own. The engine is written for this
machine and has no outside source.

## The engine

`engine.h` is a 0x88 board with what a rank needs in one texel, lists of pieces, two 32-bit hashes (no 64-bit registers),
and moves in 16 bits. `search.h` is iterative deepening with a principal variation search, a table, a null move, late move
reductions, killers, history, reverse futility, late move pruning and a quiescence search. The evaluation is material,
tables by place, and pawn terms cached by the pawns' hash. A node on the first core costs some 1,600 instructions.

## The search on the workers

Two cores must not store to one texel, and a core sees another's store a pass later, so the usual table all searchers
write cannot exist here. `share.h` does this instead:

- Each **searcher** has a table of its own (16 KB) and a ring of 64 entries in which it tells what it found. Only it stores
  to them. Its status is a texel of its own, stamped with the search it belongs to, so the first core never stores to it.
- A **shared table** of 2 MB is read by all and written only by **keepers**, workers that do nothing else: they read every
  ring each pass and store what lies in their shard (8 keepers at 192 workers or more, 4 at 96, 2 at 48, 1 at 4).
- The **first core** only looks: which searcher finished the deepest, what it said, and when to stop. It stores `ch_stop` and
  `ch_deepest`, each alone in a texel.
- Searchers split at nodes of depth 3 or more: after the first move each takes the moves it owns by their index first, then
  the others, rotated by who it is. One in eight looks a depth further, and a searcher skips a depth another has finished.

`RVC_RACES` says 0 undone, 0 near, and 0 stops on the plain run; the "as it was" reads are searchers reading the shared table
and rings, which is how they are meant to read.

**A result with workers is not a fixed sum.** Who is first decides what is found, so the nodes differ from run to run: with
workers the check is the right move (`nxchess solve`). With 0 workers the search is fixed, and its sum is the build
computer's (`linux/chess/test/chess_host.c`).

### Tried and dropped

Rounds that give a result independent of the number of cores: the first core keeps the top of the tree and hands out jobs
with a budget, then a barrier. On a host model of the cores it reached at most about 3 times the speed, and at depths 9 and
10 time to depth was often worse than one core (0.1 to 2.9 times): a round lasts as long as its longest job, and the first
core expands the top at 2 MIPS.

## What it gains

The card, one run through the queue, `nxchess time 3000` over five positions, averaged:

| workers | nodes a second | gain | depth in 3 s |
|---|---:|---:|---:|
| 0 | 2,306 | 1 | 4.8 |
| 15 | 31,673 | 13.7 | 6.0 |
| 63 | 117,906 | 51 | 6.0 |
| 255 | 169,909 | 74 | 4.4 |

`rvc_cpu` with the card's clock (`--pass-model`, `RVC_WCACHE=1`):

| workers | nodes a second | depth in 5 s | depth in 20 s |
|---|---:|---:|---:|
| 0 | 2,092 | 5.4 | 7.4 |
| 7 | 13,384 | 6.4 | |
| 15 | 30,426 | 7.0 | 9.4 |
| 31 | 63,206 | 7.4 | 10.0 |
| 63 | 113,072 | 7.2 | 10.6 |
| 255 | 282,330 (on the card 169,909) | 7.0 | 9.4 |

The 0-worker and 63-worker model figures agree with the card to 10%; at 255 the model says 66% more than the card.

So **63 workers is the default** (`MOST_WORKERS`): 255 has more nodes and less depth. What bounds it:

- Nodes go up nearly with the cores until the pass grows: 255 workers make every pass longer, and the first core's look at
  the search and everything else on the machine with it.
- Depth does not follow nodes. Each searcher does the first-move chain itself before any split, a heavy subtree is searched by
  one core at a time, and what another found is two or three passes away. The host model puts time to depth at about 3 times
  with 15, 4.5 with 63 and 6 to 8 with 255 at depths 8 and 9, against 14, 51 and 74 times the nodes.
- The write cache does not bound it: 192 of core 0's and 539 of the workers' passes ended full in 235 thousand.
- Lanes do not fit: a node's search goes far past a pass and keeps a stack of moves; steps would save and restore all of it.

## Checking it

- `nxchess perft`: six positions at known numbers. `nxchess book`: every line of the book legal.
- `NXCHESS_WORKERS=0 nxchess search 6`: sum `6faa2f74`, the same as `chess_host 6` on the build computer.
- `nxchess solve MS`: six tactics must come out right. With 63 workers 6 s is enough, with 15 about 12 s, and one core
  needs more than 20 s for the third, whose mate lies at depth 6 past a queen given.
- `python tools/test_all.py chess`, and `--races`: includes a game by `nxkey` with undo, new game and a killed
  program (`mctest` must pass after it), and a strength kept by the registry.
- `linux/chess/test/chess_model.c` is the model of many cores on the host, a searcher a coroutine, to try a change to the
  sharing in seconds.

## Not done

- Reversi, checkers and small-board Go: chess took the budget. They could share the window's board code.
- The engine does not see zugzwang past its null move (Morphy's mate in two with a rook given was not solved).
- A second `mcw_open` after `mcw_close` in one process gave 127 workers and hung in `mcw_for_begin`. nxchess keeps its
  workers open instead; the cause in `mcw` was not looked for.
- No endgame tables; no pondering in the player's time; no clock.
