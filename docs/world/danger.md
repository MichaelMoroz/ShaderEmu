# The danger zone

A panel on the den's right wall, by the rack, lets a visitor harm their own machine's memory
and see what it does. A machine is its visitor's own (`share.md`): nobody else's is touched.

- **Radiation** flips bits at random places, 1 to 10,000 a second on a logarithmic scale (1, 2, 5
  in each decade). One flip goes a round, so past what the rounds take a flip is several bits
  of one word, up to 32: 10,000 a second is some 14 bits a flip at 8 rounds and 90 frames.
- **The laser** scrambles a square of memory where a beam points on the memory screen (while
  that shows RAM) and its trigger is held: 5 to 129 texels a side. A square on the screen
  shows where it is, red while it burns.
- **Six keys say what is exposed**, the memory screen's own kinds (`owner()` in
  `MemView.shader`): the kernel, Linux's programs, the windows' buffers, the display and the
  GPU's words, a 3D program's GPU memory, the sound card's. The kernel and Linux start safe;
  Linux's programs are the RAM after the kernel, to 0x92000000 (`docs/system/nommu.md`, "Memory").

Reset on the control panel starts the machine over, whole.

## How

`experiments/rvc_opt/src/zap.h`, included by the commit and not by the tick (an instruction
costs what it did). For one commit the host sets:

| Uniform | |
|---|---|
| `_ZapKind` | 0 nothing, 1 one word of one texel, 2 a square of texels |
| `_ZapX`, `_ZapY` | the texel: its column (0 to 8,191), and its row among RAM's rows (128 KB each: `docs/machine/gpu.md`, "Rows of 8192") |
| `_ZapR` | kind 2: how many texels the square reaches each way |
| `_ZapWord` | kind 1: which of the texel's four words |
| `_ZapMaskLo`, `_ZapMaskHi` | the bits that flip (kind 2: of a pattern made from the place and `_ZapSeed`) |
| `_ZapZones` | a bit a kind of memory: only those are touched |

- `zap_bands()` is or-ed into the bands the commit draws, so the texels are drawn at all, and
  the list the commit leaves in state texel (41,0) has them: the next pass draws the same
  bands into the other buffer, which is how the two stay equal.
- `zap()` is applied where a band is drawn whole. What a core stored to the same texel in
  that pass is a point drawn after it, and wins.
- It is one commit's. Left set, the next commit would flip the bits back: `EmuMachine` sets
  the uniforms before a round's commit and clears `_ZapKind` after it (`Zap`, `ZapNext`;
  sixteen may wait, one goes a round). A machine switched on or reset starts with none
  waiting: before, the sixteen queued for the old machine were flipped in the new one.
- The random place is chosen by `EmuDanger`, not hashed over all RAM in the shader: a zap is
  one band or two of the 72 (`ram_bands`: each of its bits draws two more), never all.

In the world: `EmuDanger` (the panel's keys, the rates, the count), a branch of
`EmuPointer.Aim` for the memory screen (a trigger collider over its quad, there while the
laser is on), `_Cursor` in `MemView.shader`. "ShaderEmu/Add the danger zone to the open scene"
puts it into a baked scene: a canvas, no bake. Run it twice the first time (a new Udon
behaviour: `AGENTS.md`).

## Checking it

    bin\rvc_harness.exe ... --fixed-dt 0.004 --frames 2600 --bench 0 --save-state A.snap
    ... --save-state B.snap --zap "1,100,224,0,2,00ff00ff,0,32" --zap-frame 2000

`--zap KIND,X,Y,REACH,WORD,MASK,SEED,ZONES` (mask and seed in hex) is the same hand from the
harness. Two such boots differ in exactly the texels asked for: one word with the mask's
bits for kind 1, 49 texels for kind 2 with reach 3, none at all with the zone's bit clear,
and no state texel. Row 224 (0x81c00000) is in the sound card's megabytes, which an idle guest leaves alone.

In play mode: power on, then `ToggleRadiation` on the "Danger" behaviour; its `flips` rises
and the machine's `zapHead` follows `zapTail`. With the kernel and Linux exposed (`Zone0`,
`Zone1`) at 1,000 a second, a program died of an illegal instruction after some 10,000 bits,
with the kernel's register dump on the console.

The laser, in play mode: `ToggleLaser`, `TeleportTo` two metres before a point of "Memory
screen", turn "PlayerCamera" up at it (the beam is the head's on a desktop), then
`RunInputEvent("_inputUse", new UdonInputEventArgs(true, HandType.RIGHT))` on the beams'
behaviour. `target[1]` is -2 and `_Cursor` on the memory view's material is where it points;
while the trigger is held `shots` rises by one a round (472 in a few seconds) and the cursor's
fourth number is 1; on memory whose key is dark nothing is queued and the count's label says
"Windows is safe: its key is dark".

The control words are in the Display and GPU kind's first megabyte, exposed from the start:
the laser there leaves words with their top bit set, and Udon's cast of such a word to a
number throws, which halted `EmuMachine` for the session in the world on 12 October 2026
(`ToInt32` of a `UInt32` in the readback's event). Every guest word that becomes a number
goes through `EmuMachine.Small` now. To check: in play mode queue zaps over them (`zaps`
nine numbers each: 2, a column under 64, row 512, reach 8, 0, 0xffff, 0xffff, a seed,
0x3c; then `zapTail`), and the machine must go on counting with texel 64 of `row` scrambled.

## Not done

- The laser has not been tried in a headset, nor with a real mouse button on a desktop: the
  trigger was the event above.
- A zap of a texel a core stores to in the same pass is lost.
- Before 15 October 2026 a flip was one bit, and 1,000 a second was not reached (357 in the
  editor at 4 rounds and 89 frames). The scale past that, with a flip's bits packed into one
  word, has not been measured. The count on the panel is of what was really queued.
