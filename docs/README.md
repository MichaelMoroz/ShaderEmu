# The docs

One file per subject. A doc names another by its file name, which is unique across the folders.

## machine

- [boot.md](machine/boot.md) – Booting Linux
- [console.md](machine/console.md) – The console
- [cpu-harness.md](machine/cpu-harness.md) – The machine on the processor
- [display.md](machine/display.md) – Display
- [fpu.md](machine/fpu.md) – Float instructions
- [fxc2.md](machine/fxc2.md) – fxc2: the D3D11 build without FXC
- [gpu.md](machine/gpu.md) – GPU
- [input.md](machine/input.md) – Input
- [lanes.md](machine/lanes.md) – Lanes: pixels of the tick, each running a function on an index of its own
- [multicore.md](machine/multicore.md) – More than one core
- [profiling.md](machine/profiling.md) – Profiling the programs the machine runs
- [sound.md](machine/sound.md) – The sound card
- [speed.md](machine/speed.md) – The machine's speed
- [tick-costs.md](machine/tick-costs.md) – What an instruction costs

## system

- [nommu.md](system/nommu.md) – Linux without an MMU
- [nanox.md](system/nanox.md) – Nano-X
- [fonts.md](system/fonts.md) – Fonts
- [cc.md](system/cc.md) – A C compiler in the machine
- [flags.md](system/flags.md) – Compiler flags of every build, and what paid
- [hlsl.md](system/hlsl.md) – HLSL in the machine: shaders compiled for the lanes
- [scripts.md](system/scripts.md) – Scripts: JavaScript and Python
- [terminal.md](system/terminal.md) – Terminal programs
- [ports.md](system/ports.md) – Ports: upstream's network and system programs
- [media.md](system/media.md) – libmedia: pictures, sound and films on the worker cores
- [install.md](system/install.md) – a game's installer kept as its maker shipped it, its licence accepted and its files unpacked in the machine (cabinst)
- [lan.md](system/lan.md) – The network: TCP/IP between machines
- [fetch.md](system/fetch.md) – Fetch: pages from the host
- [persist.md](system/persist.md) – Persist: a folder the host keeps between sessions
- [clipboard.md](system/clipboard.md) – The text field in front of the screen: the machine's clipboard, text out and in (`clip:`)
- [open.md](system/open.md) – Open: a link or a file, with the program for its kind
- [fuzz.md](system/fuzz.md) – The desktop's fuzzer: plans of opening, using and closing programs that replay

## apps

- [bench.md](apps/bench.md) – The benchmark: `nxperf`
- [export.md](apps/export.md) – Export and import: a file or folder out as a photo VRChat's camera takes (a zip on a PC) or as text, and in by a link to that photo or pasted text (`nxexport`, `open`)
- [ide.md](apps/ide.md) – The IDE
- [imgui.md](apps/imgui.md) – Dear ImGui
- [play.md](apps/play.md) – An audio player: `nxplay`
- [video.md](apps/video.md) – The video player
- [convert.md](apps/convert.md) – The converter: `nxconvert`
- [web.md](apps/web.md) – The browser
- [physics.md](apps/physics.md) – Rigid boxes on every core
- [pinball.md](apps/pinball.md) – Pinball
- [solitaire.md](apps/solitaire.md) – Solitaire (Klondike)
- [raytrace.md](apps/raytrace.md) – Rays on every core

## games

- [chess.md](games/chess.md) – Chess
- [classicube.md](games/classicube.md) – ClassiCube
- [doom.md](games/doom.md) – Doom
- [halflife.md](games/halflife.md) – Half-Life: Uplink
- [jk2.md](games/jk2.md) – Jedi Knight II: Jedi Outcast, the 2002 demo (begun: no picture yet, on rvc_cpu; private images only)
- [micropolis.md](games/micropolis.md) – Micropolis
- [nes.md](games/nes.md) – A NES
- [openttd.md](games/openttd.md) – OpenTTD
- [aoe.md](games/aoe.md) – Age of Empires, the 1997 trial (private images only)
- [quake.md](games/quake.md) – Quake
- [quake3.md](games/quake3.md) – Quake III Arena
- [ralert.md](games/ralert.md) – Red Alert
- [tdawn.md](games/tdawn.md) – Tiberian Dawn

## world

- [world.md](world/world.md) – The room's models
- [holodeck.md](world/holodeck.md) – The holodecks
- [stations.md](world/stations.md) – The classroom's computers
- [danger.md](world/danger.md) – The danger zone
- [share.md](world/share.md) – Seeing each other's machines
- [udon-net.md](world/udon-net.md) – How much Udon carries between clients (measured)
- [gamepad.md](world/gamepad.md) – The game controllers
- [volume.md](world/volume.md) – The volume display
- [linux.md](world/linux.md) – The world on Linux

## plans

- [aoe-plan.md](plans/aoe-plan.md) – Plan: Age of Empires, the 1997 trial
- [blitter-plan.md](plans/blitter-plan.md) – Plan: a blitter in the commit pass
- [cpu-benchmarks-plan.md](plans/cpu-benchmarks-plan.md) – Plan: the benchmarks of the era, on the machine
- [jk2-plan.md](plans/jk2-plan.md) – Plan: Star Wars Jedi Knight II: Jedi Outcast, the 2002 demo
- [simd-lanes-plan.md](plans/simd-lanes-plan.md) – Plan: lanes, a worker's pixels each on an index of their own
- [original-x86-plan.md](plans/original-x86-plan.md) – Windows in a VRChat Shader — Project Handoff
- [riscv-backport-plan.md](plans/riscv-backport-plan.md) – What gpu_vm's week can give the RISC-V machine (28 October 2026)
- [riscv-isa.md](plans/riscv-isa.md) – The RISC-V machine's instruction set: what to add, measured (28 October 2026)
- [riscv-profile.md](plans/riscv-profile.md) – Where the RISC-V machine's time goes: the 51 parts profiled (28 October 2026)
