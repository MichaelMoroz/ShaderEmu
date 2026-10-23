/* The game bench's one-core kernels compiled by gpu_vm's clang (GAMEBENCH.md: the compiler's gap against gamebench.h's
 * hand-written g_logic, g_verts and g_ray1): gvmbench's cases g_logic_cc, g_verts_cc and g_ray1_cc (cases.h's real
 * programs, ns a bundle). The bench pokes gb_kind (1 logic, 2 verts, 3 rays) and gb_frames; gb_units counts the work
 * as the hand-written cases do (entities, calls of 4,096 vertices, chunks of 32 pixels). Built by build_gvm.sh. */
#include "gamebench_kern.h"

volatile unsigned gb_kind = 1, gb_frames = 2, gb_units, gb_sum;
static inst inst0;
static uint32_t fb0[256 * 192], vout0[4096][4];

int main(int argc, char **argv)
{
	if (argc > 2) { gb_kind = (unsigned)(argv[1][0] - '0'); gb_frames = 0; for (const char *c = argv[2]; *c; c++) gb_frames = gb_frames * 10 + (unsigned)(*c - '0'); }   /* (gvm_cpu: KIND FRAMES) */
	insts = &inst0; fb = fb0; vout = (uint32_t (*)[4096][4])vout0;
	unsigned kind = gb_kind, E = 512, sum = 0;
	if (kind == 3) { RW = 64; RH = 32; }
	LP.E = kind == 1 ? (int)E : 0;
	data(1, kind == 1 ? (int)E : 1);
	for (unsigned f = 0; f < gb_frames; f++) {
		if (kind == 1) { inst0.frame = f; logic_frame((uint32_t)(uintptr_t)&inst0, E); gb_units += E; }
		else if (kind == 2) { sum += verts_job((uint32_t)(uintptr_t)vout0, 4096); gb_units++; }
		else { sum += ray_job(0, f); gb_units += (unsigned)(RW * RH / 32); }
	}
	gb_sum = sum;
	return 0;
}
