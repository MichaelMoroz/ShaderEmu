# RISC-V translated to Udon at build time (an experiment)

Udon cannot make or load a program at runtime: no extern assembles, loads or swaps one, and
there is no reflection or delegate. So instead of a JIT, `rv2udon.py` translates a bare-metal
RV32IMA image into Udon assembly once, in the editor, the way UdonSharp's output is made.
An interpreter in Udon runs at about 0.6 MIPS, below one of the shader machine's cores; the
translation is what might beat it.

    python experiments/udon_aot/rv2udon.py programs/bin/raytrace.bin programs/build/raytrace.elf OUT
    python experiments/udon_aot/rv2udon.py programs/bin/classes.bin - OUT      (no ELF: all of it as code)

`OUT/prog.uasm` is the program, `prog.jt` the jump table, `prog.externs` the externs it calls.
`editor_runner.cs` is a method body for `unity_execute_code`: it runs a C# RV32IMA interpreter
as the reference, then the translation in the editor's Udon VM, with the same console input,
and compares the console and every word of RAM.

## How it translates

- x1 to x31 are heap variables, x0 one never written; every immediate is a constant of its own
  (hex literals: Int32 arithmetic in Udon wraps, and the literal parser takes `0x80000000`).
  An add is `PUSH, PUSH, PUSH, EXTERN`; `li`, `lui`, `auipc`, `mv` and `jal`'s link are a
  `COPY`; a write to x0 (a `nop`) is nothing.
- RAM is one `int[]` from 0x80000000. A word load is three externs: `rs1 + (imm + 2^31)`, `>> 2`,
  the array's get. Bytes and halves shift and mask the word; a byte store reads, masks, writes.
- Branches and `jal` are direct `JUMP`s; `jalr` looks the target up in the jump table (one
  entry for every word of the text) and takes `JUMP_INDIRECT`.
- Where a backward branch lands and in every `jalr`, a budget is counted down; at 0 the slice
  ends and the next starts there. A console write ends the slice too, and a second status
  poll with no input and no write between it is a wait.
- Devices are found by constant propagation over the program: a load or store whose base is
  a known constant outside RAM is the UART (the transmit, receive and status registers) or the
  timer. Any other base is taken to be RAM, unchecked.
- `mulh*` is `Math.BigMul` and a shift; division is in 64 bits, where `MIN / -1` cannot throw.
- The heap is sized to the declarations through UdonSharp's `CompilerUdonInterface.Assemble`:
  the default heap is 512 variables, the most 1,048,576. Int64 constants and the RAM image are
  set in the heap after assembly, as an editor script would before the asset is saved.

## Measured (19 October 2026, editor, Mono, Ryzen 7 5800X3D)

All of them give the reference's console and RAM to the word (`optest.c` is built with `programs/build.bat`'s flags).

| program | guest instructions | Udon | guest MIPS |
|---|---|---|---|
| raytrace 320 x 200, 3 bounces, shadows | 73,572,594 | 12.1 s | 6.08 |
| raytrace 64 x 40 | 2,957,114 | 0.49 s | 6.04 |
| op test (every translated operation over edge values) | 39,392 | | same RAM and sum |
| classes, all seven loops | 46,500,442 | 9.0 s | 5.17 |

raytrace: 1,393 instructions become 12,449 Udon instructions (96 KB) and 457 heap variables;
its mix is 64% arithmetic, 17% multiplies (16.16 fixed point), 9% loads, 4% stores.

`classes`, per guest instruction: arithmetic 128 ns, `lw` 267, `sw` 263, a two-instruction loop
206 (its budget check costs what its body does), call and return 267, loads striding through
4 MB 156, jumps between `nop`s 93.

## Linux: the kernel and its programs translated

`linux2udon.py` translates the no-MMU kernel's text (0x82000000 to `_einittext`, 743,675 words)
and the text of programs in the ROM into Udon bytecode, and adds an interpreter written in Udon for
the rest: the system instructions, code it did not translate, and programs not named.
`editor/UdonLinux.cs` loads it into the editor's VM and runs it slice by slice with
`editor/RvRef.cs`'s console and clock; `RvRef.cs` is also the reference (the no-MMU processor in C#).

    python experiments/udon_aot/linux2udon.py IMG/linux_payload.bin OUT [IMG/rootfs.bin /bin/busybox ...]
    UdonAot.UdonLinux.Run(OUT, IMG, "/ # ", 40000, 120.0, "/ # ", "uname -a
")   (editor code)

- Registers are heap variables in translated code and an `int[]` in the interpreter; they are
  copied over when one hands to the other.
- Code is translated a block at a time: values known within a block are folded to constants
  (106,494 instructions in the kernel), writes overwritten before a read are dropped (29,311),
  and an address is checked to be RAM once a block for its base register. The kernel alone is
  50 MB and 181,391 heap slots, made in 10 s.
- A load or store below RAM goes to a routine for the ROM, the clock and the devices; a store
  there is dropped. `jalr` looks its target up in a table of the text; outside it, the interpreter.
- CSRs, `ecall`, `sret`, `wfi`, traps, interrupts and the SBI are the interpreter's. The copy CSR
  is `Array.Copy` or `Array.Clear`.
- The F extension as the machine has it (`docs/machine/fpu.md`) is the interpreter's too: registers
  as bits, each operation a `BitConverter.Int32BitsToSingle`, the `Single` operator and
  `SingleToInt32Bits`; conversions to an integer rounded by `Mathf` as the instruction's mode says
  and clamped. Translated code hands a float instruction to it.
- A program (a FLAT binary, whose text has no relocations) is translated at its file offsets and
  runs at a base held in a variable, wherever the kernel put it. At `sret` to user mode the
  interpreter finds which program and base by four words of its text, checked again each time.
  Programs that were not translated are interpreted.

Measured on 19 October 2026 (editor, Mono, Ryzen 7 5800X3D): with `uname -a`, `ls /`, an
arithmetic `echo`, `cat /proc/cpuinfo`, `head /etc/passwd` and `md5sum /bin/busybox`, the
console is the reference's to the byte, the boot timestamps aside: 8.2 s with busybox translated,
62.6 s with it interpreted (0.4 MIPS). First prompt at 3.0 s. With the kernel, busybox, nano-X
and the desktop's nine programs the build is 116 MB, 402,985 heap slots, 27.6 s.

## The C++ translator (`l2u/`)

`l2u/l2u.exe` takes `linux2udon.py`'s arguments and writes the same kind of output, a tenth to an
eighth of the bytes, in a second or two (`l2u/build.cmd` compiles it, 7 s):

    set L2U_HOT=build\udon_profile
    experiments\udon_aot\l2u\l2u.exe IMG\linux_payload.bin build\udon_linux IMG\rootfs.bin /bin/busybox ...

- Two tiers. Hot words are translated inline as before, in superblocks. Every other word is
  threaded: the segment's jump table (`jt`) points it at a routine shared by every word of the same
  operation and registers (about 42,000 of them), whose immediate is in the segment's `tim` table.
  Any pc can be entered, so the interpreter is seldom reached (7,500 handovers in the shell check,
  51,500 before). Threaded code runs about 2.5 times slower than inline.
- Hot is what the profile `L2U_HOT` names (words run 100 times, `L2U_HOTN`), and innermost loops
  of up to 64 words in programs (`L2U_INNER`; the kernel's `L2U_INNERK`, 0). A profile is made by
  a build with `L2U_PROF=1`, a run, and the `prof5` array read back, by program name: a program
  rebuilt into other words has none. `build/udon_profile` is the shell check's training script,
  the desktop, nxterm, nxsettings, nxnet, glxgears, a fetch and nxview, and each game's start and
  play run headless, summed.
- Floats, the CSRs, `fcsr` and the clock reads are translated in both tiers (fpu()'s semantics, f
  registers as `Int32` bits: a `Single` heap variable quietens a signalling NaN). 2,208 generated
  tests (`gen_ftest.py`) give the interpreter's results; `fptest 2000` the reference's console.
- A program's code that was not translated is threaded when it first runs, into a window of 1 M
  words with the same routines (`sec_dyn.inc`; `fence.i` empties it): `fptest 2000` 1.2 s, 6.5
  interpreted. Loads from the ROM (mapped files) stay in translated code.
- Library routines known by a hash of their words run natively (`sec_native.inc`): musl's and the
  kernel's `memcpy`, `memmove` and `memset` as `Buffer.BlockCopy` and `Array.Clear`, libgcc's
  64-bit division as `Int64`'s, musl's `strcmp` and `memcmp` a word a step; other cases go on in
  the translated body. Switches for comparisons: `L2U_DYN`, `L2U_NATIVE`, `L2U_UDREL`, `L2U_THR`.
- A halfword is moved by `Buffer.BlockCopy` of 2 bytes through a small `int[]` (`lhu` an add, the
  copy and a read; `sh` a write and the copy). Not right: a halfword across a word boundary and a misaligned
  word load or store.
- `build/udon_profile` is not in the repository: it is remade by the steps above.

Measured (19 October 2026, editor): kernel alone 4.8 bytes a text word (75.6 from the Python one);
kernel and 13 programs 22.4 MB and 65,232 heap slots (135 MB), translated in 1.7 s (24 to 31 s);
kernel and Red Alert 31.2 MB and 84,237 slots (252 MB, 854,733), booting to the prompt. The shell
check 7.4 s (8.1), the console the reference's. In the world against the Python build: prompt 4.0 s
(5.0), the desktop idle 16 s after `nx` (20.5), glxgears in a window counting 881 to 965 frames a
second (606 to 652); `dd` from `/dev/urandom` to `/dev/fb0` 3.0 s (1.6), the kernel's random
generator being in no profile. Without the desktop in the profile its start took 28 s.

With the games (19 October 2026, alternating runs in the translator's own runner, against the
build and profile above): Doom's timedemo from its mark 2.30 s (5.9; 2.65 with the old translator
and the games' profile), the shell check 5.8 s (6.7), boot 2.0 s (2.65), md5sum of 4 MB 9.0 s
(9.9), fptest 1.2 s (6.5). Hot inline code runs at 1.6 to 1.9 externs a guest instruction (an
add is one, 76 ns), so the translator's own share of that is 1.04 to 1.13 times; the rest is the
interpreter left behind and the profile. The world's build, the kernel, the 13 programs and the
nine games, is 123.4 MB, 14.0 bytes a word, 377,713 heap slots, translated in 8.7 s; the menu
imports it in 16 to 36 s.

The games in the world (editor, the earlier driver, GPU mode, `nano-X -p & sleep 2; GAME &`, no worker cores, which
each game does without): Doom plays its demo at 21 frames a second, Quake e1m1 at 23, ClassiCube
at 28, the NES at 90 to 97% of its speed, Quake III q3dm17 at 11 after 90 s of loading; Red
Alert's menu, Tiberian Dawn's demo text and Micropolis's map are drawn and wait for the player.
Half-Life's hldemo1 runs at 1 frame a second after 190 s and its picture is black but for the HUD.

## In the world (`ShaderEmuWorldUdon.unity`)

A copy of the world has the Udon processor beside the shader one (menu "ShaderEmu/Add the Udon
machine to the open scene", `unity/ShaderEmu/Editor/ShaderEmuUdonCpu.cs`; the driver is
`Scripts/EmuUdonCpu.cs`). The world's scripts are the original world's, unchanged: `EmuMachine`
keeps the display, the GPU camera, sound, the control pass, LAN, fetches, the holodeck, the
classroom's stations and the stream between players. Only the processor changes:

- A button on the main console's title row ("CPU: shader" / "CPU: Udon VM", `ToggleCpu`) picks
  the processor and powers the machine on again. The world starts with the shader's.
- In Udon mode `EmuUdonCpu` holds the shader CPU at reset (the tick's `_Init` 1 every frame: it
  runs `cpu_init` each pass, executes and stores nothing) and runs the translated program on `mem`.
- The translation marks the 4 KB pages it stores to (`L2U_DIRTY=1`: `pdirty`, `plist`, `pn`; the
  native `memcpy`, `memset` and `memmove` too; a checker found 0 pages missed; 10% of Doom's timedemo in the runner, 2% of the shell check).
  Each frame the driver `BlockCopy`s every marked page into a row of one RGBA32 stage (64, 256 or
  1024 rows, row 0 the page numbers), uploads it with one `LoadRawTextureData` and `Apply`, and
  `UdonAotScatter.shader` draws the pages into the machine's state in one blit: a geometry shader
  of 32 instances, 40 pages each.
- The words the control pass writes (`gpu.h`'s `gpu_control`, the console's and the network's)
  stay the state's: a texel only the device writes is discarded, one the two share is written by
  a pass whose `ColorMask` is the guest's words. `UdonAotWords.shader` turns those texels into
  words, read back each frame (3 KB) into `mem`.
- A GPU list is sent once when the guest sets the submit bit; when the readback shows its frame
  drawn, the copy it asked for is read back into `mem` first (the copies counter has moved), then
  the control words. A fetch's answer is read back the same way before its reply word.

Measured in the editor (19 October 2026): the prompt 5.4 s after power on, and the desktop draws
whole on its own. The self-test (glxgears and demo-hello on the window system) drew 720 lists and
27 copies. Over 18.7 s of slices 104,684 pages went up in 576 ms. Doom's timedemo in a window on
the desktop: 18.0 frames a second, 75 Unity frames a second, the slices 2.6 ms a frame (10.2 M
instructions a second of their time, as in the translator's runner) and the upload 0.14 ms (36
pages). It was 10 frames a second at 53 before two changes: a rest (the pause hint, as
`shaderemu_gpu_rest` ends a pass) while a list is submitted or out ends the frame's slices, where
the slices had spun on it to the frame's budget (14.3); and a copy on its way is read back beside
the device words, so it is here when they say it is made, not a readback later (18.0). A Doom
frame is now about four Unity frames, nearly all of it the list's way through the GPU and back. The switch to the shader CPU boots its
desktop, and back to Udon its own; a classroom place taken shows the Udon machine's console, the
monitor beside it its desktop. Not measured again with this driver: sound, the kept folder,
fetches, LAN, the holodeck and the stream to another player (they are `EmuMachine`'s, as in the
original world). The Danger zone's zaps change the state, not `mem`, so they do nothing in Udon mode.

MicroPython (`micropython -c`, its start subtracted) against the figures given for another
MicroPython on Udon with 10 ms a frame, 3.0 s and 27 s: `sum(range(40000))` 0.71 s here (5.38
M instructions, 7.5 MIPS) and `sum(i for i in range(40000))` 3.36 s (20.8 M, 6.2 MIPS); on
the shader machine (`rvc_harness`, RTX 5090, D3D11) 0.87 s and 3.95 s.

## What it would take

- Worker cores are not there: Udon runs on the main thread only, so a core more would be the same
  thread's time shared, no faster; programs ask for none and run on the first.
- `rv2udon.py` has no traps, CSRs or interrupts; an unknown pointer to a device throws there.
- Udon's time is the main thread's, so a frame's few milliseconds bound it, where the shader
  machine's cores are not.
