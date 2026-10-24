# gpu_vm in Unity

`Assets/ShaderEmu` here holds the files the world's gpu_vm copy changed (EmuMachine.cs, the gvm shaders,
Display.shader, Editor/ShaderEmuGvm.cs), laid over the world's `unity/ShaderEmu`. The full world (the room) is made as
`docs/world/world.md` says and needs Bakery, LTCGI, Mochie's shaders and the models. To run and measure the machine
alone there is a smaller project, made from this folder and `minimal/`.

## The machine alone

    python gpu_vm/platform/unity_image.py build/gvm_apps/img-cur
    python unity/gpuvm/make_project.py C:/Development/VRChat/ShaderEmu-GPUVM --sdk-from E:/Development/VRChat/VRCFluid
    pwsh C:\Users\micha\Documents\FXC2\scripts\unity-overlay.ps1 -Editor "C:\Program Files\Unity\Hub\Editor\2022.3.22f1\Editor" -Dest C:\Development\VRChat\Unity-2022.3.22f1-fxc2
    copy C:\Users\micha\Documents\FXC2\bin\unity\* C:\Development\VRChat\Unity-2022.3.22f1-fxc2\Data\Tools\

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

## Measuring

`GvmAuto.cs` runs the project's `gvm_auto.txt` when the editor starts (`image DIR`, `programs`, `scene`, `check`, `play`,
`quit`, a line each; `check` compiles every pass of the machine's materials and stops at an error), and `play` puts the Game view in front first (AGENTS.md: else the Scene view is drawn too). In play mode
`GvmProbe` (a MonoBehaviour beside EmuMachine, not a part of a world) runs `gvm_probe.txt`: it waits for the console's
text, types (EmuMachine's `hostInput`), changes the sliders, samples Unity's frames and core 0's instructions
(`totalInstructions`) and logs everything to `Logs/gvm_probe.log`, the console's lines with the wall's seconds. `minimal/measure.py probe PROJECT` writes
the script (boot, `nx`, three program starts, imdemo, then samples at three budgets); `measure.py harness LOG` types the
same lines to the harness with `--expect`/`--send`, and `measure.py table LOG...` reads either log. A first run:
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
