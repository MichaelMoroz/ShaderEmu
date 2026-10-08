/*
 * A model's triangles for the GPU, in place of gl_mesh.c: three vertices a triangle in the
 * model's own order, and beside them each one's texture coordinates as a tagged compact
 * vertex holds them (docs/gpu.md). GLQuake looked for long strips and fans here, a search per model.
 */
#include "quakedef.h"

void GL_MakeAliasModelDisplayLists (model_t *m, aliashdr_t *hdr)
{
	int		i, j, k, order = hdr->numtris * 3;
	unsigned	*coords;
	trivertx_t	*verts;

	hdr->poseverts = order;
	coords = Hunk_Alloc (order * 4);
	hdr->commands = (byte *)coords - (byte *)hdr;
	for (i = 0; i < hdr->numtris; i++)
		for (j = 0; j < 3; j++) {
			int s, t;

			k = triangles[i].vertindex[j];
			s = stverts[k].s, t = stverts[k].t;
			if (!triangles[i].facesfront && stverts[k].onseam)
				s += hdr->skinwidth / 2;	/* on the back side */
			/* the middle of the texel, in 1,024ths of the skin */
			*coords++ = ((unsigned)((2 * s + 1) * 512 / hdr->skinwidth) & 0xfff) |
				((unsigned)((2 * t + 1) * 512 / hdr->skinheight) & 0xfff) << 12;
		}

	verts = Hunk_Alloc (hdr->numposes * order * sizeof (trivertx_t));
	hdr->posedata = (byte *)verts - (byte *)hdr;
	for (i = 0; i < hdr->numposes; i++)
		for (j = 0; j < hdr->numtris; j++)
			for (k = 0; k < 3; k++)
				*verts++ = poseverts[i][triangles[j].vertindex[k]];
}
