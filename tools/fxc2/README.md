# fxc2

`d3dcompiler_47.dll` here is [fxc2](https://github.com/MichaelMoroz/FXC2)'s build of that DLL
(its `bin/d3dcompiler_47.dll`, vkd3d 2.1-93-gcfcb4833 with fxc2's patches): an HLSL compiler
that writes D3D11 bytecode as Microsoft's does, in seconds where FXC takes minutes. The build
copies it next to the harness, which then compiles with it (`docs/fxc2.md`).

To update it, copy the file from a newer FXC2 checkout and run the checks in `docs/fxc2.md`.

vkd3d is LGPL-2.1-or-later and this DLL links it statically; its source and the patches are in
the FXC2 repository.
