// The fxc2 a shader of patches (src/amp.h) needs: before fxc2's patch 64 a hull shader's `return I[id]` was an
// indexable temp the card did not draw (it removed the device; WARP drew it). fxc2 from 65 on names its version
// (__FXC2_VERSION__); one older names none, and Microsoft's compiler (no __FXC2__) draws them right.
#if defined(__FXC2__) && (!defined(__FXC2_VERSION__) || __FXC2_VERSION__ < 65)
#error "fxc2 is older than 65: put FXC2's bin/unity in the editor's Data/Tools (the harness: its d3dcompiler_47.dll beside it)"
#endif
