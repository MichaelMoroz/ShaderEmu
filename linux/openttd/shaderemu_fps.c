/* Frames a second in the window's caption (docs/system/nanox.md, "Frames a second"): the GPU's lists go to the
 * machine directly in the picture on the GPU (shaderemu_gpu.cpp), past the library that writes it for other programs. */
#include <nano-X.h>
#include "fps_caption.h"

void se_fps_caption(const GR_WINDOW_INFO *info, uint32_t now)
{
	fps_caption(info, now);
}
