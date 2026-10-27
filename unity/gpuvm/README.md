# gpu_vm in Unity

`Assets/ShaderEmu` here holds the files the world's gpu_vm copy changed (EmuMachine.cs, the gvm shaders,
Display.shader, Editor/ShaderEmuGvm.cs), laid over the world's `unity/ShaderEmu`. The full world (the room) is made as
`docs/world/world.md` says and needs Bakery, LTCGI, Mochie's shaders and the models. To run and measure the machine
alone there is a smaller project, made from this folder and `minimal/`.

## The machine alone

    python gpu_vm/platform/unity_image.py build/gvm_apps/img-cur
    python unity/gpuvm/make_project.py C:/Development/VRChat/ShaderEmu-GPUVM --sdk-from E:/Development/VRChat/VRCFluid
    python unity/gpuvm/make_project.py C:/Development/VRChat/ShaderEmu --sdk-from E:/Development/VRChat/VRCFluid --world
    pwsh C:\Users\micha\Documents\FXC2\scripts\unity-overlay.ps1 -Editor "C:\Program Files\Unity\Hub\Editor\2022.3.22f1\Editor" -Dest C:\Development\VRChat\Unity-2022.3.22f1-fxc2
    copy C:\Users\micha\Documents\FXC2\bin\unity\* C:\Development\VRChat\Unity-2022.3.22f1-fxc2\Data\Tools\

- `--world` makes the whole room instead: `unity/ShaderEmu` as committed (its models and textures: no Blender, no
  downloads but the shader packages of `world/install_packages.py`), the gpu_vm files over it, and the measuring
  folder; `minimal/Editor` is left out, as the world's builder has what it stands in for. Then the image's `Images`
  folder, "ShaderEmu/Build world" and "ShaderEmu/GPU-VM: build the machine".
- `make_project.py` takes VRChat Creator Companion's World template (ProjectSettings, Unity's packages) and copies
  VRChat's Base and Worlds SDK (3.10.4; UdonSharp and ClientSim are in Worlds) from another VCC project's `Packages`
  (VCC or `vpm` can add them instead). It writes the world's Scripts, the few shaders the machine draws with, `src/`
  from `experiments/rvc_opt` as the world's "Sync shader sources" does, this folder's files over them, and
  `minimal/`: `Editor/ShaderEmuGvmScene.cs` (the helpers of the world's builder that ShaderEmuGvm.cs uses, and the
  scene), `Editor/GvmAuto.cs` and `Measure/GvmProbe.cs`. Run again, it writes the repository's files anew.
- The editor is fxc2's overlay (`docs/machine/fxc2.md`): the tick under Microsoft's FXC takes minutes and runs slower.
  The overlay script alone copies the whole DLL as `D3DCompiler_47.dll`, and Unity's shader compiler then never
  starts ("failed to read magic number"): the stub and `fxc2_d3dcompiler.dll` of FXC2's `bin/unity` are what works.
  Delete `Library/ShaderCache*` after a change of compiler.
- In the editor: "ShaderEmu/GPU-VM: make the machine's scene" (`ShaderEmuGvmScene.cs`) makes the program assets,
  imports the boot image from EditorPrefs `ShaderEmu.GvmImage` (once), and makes `Assets/ShaderEmu/GvmMachine.unity`:
  VRCWorld at the origin facing the display and the console, the GPU device and the pass cameras below on layers the
  player's camera does not see, EmuMachine powered on at start, then ShaderEmuGvm.cs's "build the machine".
  The panel's two sliders are there unseen: speed 5 (65,536 instructions a frame) and tick 3 (8,192 a pass,
  EmuMachine's TicksPerRound): eight passes a frame, as the world's.
- `make_project.py --tick STEP` draws another gpu_vm step than the committed `tick.cginc` (v0009) through
  `gpu_vm/platform/unity_tick.py`; then make the scene again. A step with core 0's own write cache (c0nb, `#ifndef
  CORE0`: `build/tick_gen/c0e/v0003.hlsl`) gets `GVM_HAS_CORE0 1`: `GvmTick.shader` leaves core 0 out,
  `GvmTick0.shader` (the step with CORE0=1) draws core 0's 8 x 8 block as one quad on the tick camera, and the commit's
  points are WCN0 + 255 x WCN with core 0's first, as `harness/apps/gvm_backend11.cpp` draws them.

## RISC-V beside it

The same project has the world's RISC-V machine for comparison: the world's shaders as `ShadersRv/` (named
`ShaderEmuRv/...`), the world's EmuMachine as `Scripts/EmuRvMachine.cs` with the probe's hooks and `hostWants` (the
host flags' bits 0-1; 0 is the harness's `--no-desktop`), both written by `make_project.py`. "ShaderEmu/RISC-V: make
the machine's scene" (`minimal/Editor/ShaderEmuRvScene.cs`) makes `Assets/ShaderEmu/RvMachine.unity` as the world's
builder lays the machine out, eight passes of 8,192 a frame, with the no-MMU image of EditorPrefs `ShaderEmu.Repo`'s
`build/images/nommu`.

## Measuring

`GvmAuto.cs` runs the project's `gvm_auto.txt` when the editor starts (`image DIR`, `import`, `repo DIR`, `programs`,
`scene`, `rvscene`, `check`, `probe FILE`, `play`, `play rv`, `quit`, a line each; `check` compiles every pass of the machine's materials and stops at an error), and `play` puts the Game view in front first (AGENTS.md: else the Scene view is drawn too). In play mode
`GvmProbe` (a MonoBehaviour beside EmuMachine, not a part of a world) runs `gvm_probe.txt`: it waits for the console's
text, types (EmuMachine's `hostInput`), changes the sliders, samples Unity's frames and core 0's instructions
(`totalInstructions`) and logs everything to `Logs/gvm_probe.log`, the console's lines with the wall's seconds. `minimal/measure.py probe PROJECT [--rv] [--log NAME] [--file F]` writes
the script (boot, `nx`, three program starts, imdemo, then samples at three budgets; the first line `log NAME` names
the log); `measure.py harness LOG [--rv | --image DIR --tick STEP]` types the same lines to the harness with `--expect`/`--send`, and `measure.py table LOG...` reads either log. A first run:
`gvm_auto.txt` of `image E:/.../build/gvm_apps/img-cur`, `programs`, `scene`, `check`, `play`, `quit`, then
`Unity.exe -projectPath ...` through `tools\gpu_queue.ps1` (`pwsh -Command "Start-Process -Wait ..."`): the card is
the harness's too.

## The measures (24 October 2026)

img-cur, v0009, RTX 5070 Laptop, fxc2, the Game view 3840 x 2160 behind ClientSim's notice; the harness through
`measure.py harness` (`build/unity_gvm`). Busy seconds are `/proc/stat`'s in the window less the idle desktop's.

| | harness, 4,096 | harness, 8,192 | Unity, 8 x 8,192 a frame |
|---|---|---|---|
| console up (guest's uptime) | 3.33 s | 3.29 s | 6.45 s (11.7 s of the wall from play) |
| nxterm's start, busy s | 0.55 | 0.56 | 0.92 |
| nxedit's start | 0.23 | 0.25 | 0.43 |
| nxfiles's start | 0.25 | 0.24 | 0.45 |
| `nx`, 30 s after it, busy s | 15.4 | 15.2 | 13.8 (of 37.8 s) |
| imdemo, frames a second | 30.1 | 30.5 | 10.2 |

Unity's frames, 10 s each (core 0's instructions a second; all cores' in the log's `sec:` lines):

| | frame, ms | core 0, MIPS |
|---|---|---|
| idle desktop, 8 x 8,192 | 11.2 (89 fps) | 0.25 |
| idle desktop, 16 x 8,192 | 15.8 | 0.20 |
| idle desktop, 16 x 4,096 | 14.6 | 0.21 |
| imdemo, 8 x 8,192 | 11.2 (89 fps) | 1.76 |
| imdemo, 16 x 8,192 | 17.5 | 2.39 (all cores 4.7-4.9) |
| imdemo, 16 x 4,096 | 17.7 | 2.36 |

At eight passes a frame the machine gets about 720 passes a second, where the harness makes 3,900 to 7,700: a pass in
Unity is about 1 ms of the frame however long its budget (16 x 4,096 costs what 16 x 8,192 does), so a busy guest runs
at 0.6 to 0.75 of the harness's speed and an idle one wastes its rounds. The 89 fps of both 8-pass rows looks like the
display's refresh, not the machine.

## RISC-V, v0009 and v0003 (24 October 2026)

gpu_vm on img-ui5; RISC-V on `build/images/nommu` with the world's machine; eight passes of 8,192 a Unity frame; the
harness at 8,192 flat out (`build/unity_gvm/h-*-8192.log`, Unity's in `build/unity_gvm/u2`). imdemo runs 300 frames.

| | RISC-V Unity | v0009 Unity | v0003 Unity | RISC-V harness | v0009 harness | v0003 harness |
|---|---|---|---|---|---|---|
| console up, wall of play mode | 7.4 s | 7.7 s | 4.9 s | | | |
| guest's uptime at the first command | 8.6 s | 9.8 s | 6.8 s | 8.2 s | 3.2 s | 3.6 s |
| nxterm's start, busy s | 0.19 | 0.88 | 0.36 | 0.18 | 0.30 | 0.40 |
| nxedit's start | 0.14 | 0.49 | 0.17 | 0.12 | 0.20 | 0.23 |
| nxfiles's start | 0.13 | 0.55 | 0.20 | 0.11 | 0.23 | 0.26 |
| imdemo, frames a second | 22.8 | 10.7 | 21.1 | 25.0 | 33.8 | 26.8 |
| Unity's frame in imdemo | 15.2 ms (66 fps) | 11.2 ms (89) | 31.2 ms (32) | | | |
| core 0 in imdemo, MIPS | 4.31 | 1.77 | 3.24 | | | |
| imdemo's frames a Unity frame | 0.35 | 0.12 | 0.66 | | | |

(Unity's uptime less the 5 s sample at the prompt. The idle desktop's frame is 11.1 to 11.2 ms for all three.)

- Unity shows the harness's order of work a pass: ImGui's busy passes for 300 frames give 8 / (passes a frame) = 0.40,
  0.15 and 0.65 imdemo frames a Unity frame for RISC-V, v0009 and v0003, and Unity measures 0.35, 0.12 and 0.66. At one
  Unity frame rate v0003 does 1.9 times RISC-V's work.
- But a busy v0003 pass costs the card twice what a RISC-V pass does: Unity's frame in imdemo is 31 ms against 15
  (3.9 ms a pass against 1.9). In the harness flat out it is 3.0 ms against 2.0 (passes for 300 frames over the run's
  seconds), so in seconds the two machines are even (21.1 and 22.8 frames a second in Unity, 26.8 and 25.0 in the
  harness), and Unity adds about 0.9 ms to a busy gpu_vm pass that it does not add to RISC-V's (the commit's draw and
  two rounds of the operations' blits and write, each a camera render or a blit). v0009's passes end early at a full
  write cache: cheap (Unity holds 89 fps) but a sixth of the work.
- `nx`'s 30 s are 14 to 17 busy s on gpu_vm against 0.7 on RISC-V, in the harness and in Unity alike.
