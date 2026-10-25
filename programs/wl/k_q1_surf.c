/*
 * q1_surf: Quake's surface cache, the lit copies of wall textures its software renderer makes whenever a surface
 * comes into view or its light changes (WinQuake r_surf.c, copied): R_BuildLightMap (ambient, the light styles'
 * lightmaps scaled, R_AddDynamicLights' falloff for a rocket's light, bound and invert) and R_DrawSurface with
 * R_DrawSurfaceBlock8_mip0, each 16 x 16 block's texels through the colormap by light stepped across and down.
 * A unit: 12 surfaces of 64 x 64 to 256 x 128 texels at mip 0 (a room's walls, floor and ceiling), a third of
 * them lit by a moving light, from a 64 x 64 texture.
 */
#include <stdlib.h>
#include <string.h>
#include "wl.h"

#define SURFS 12
#define MAXLIGHTMAPS 4
#define VID_CBITS 6
#define TEXW 64
#define MAX_SURF (256 * 128)
#define MAX_BLOCKLIGHTS (17 * 9)

struct surface { int width, height, smax, tmax, styles, lit, texturemins[2]; unsigned char samples[MAXLIGHTMAPS * MAX_BLOCKLIGHTS]; };
static struct surface surfaces[SURFS];
static unsigned char texture[TEXW * TEXW], colormap[64 * 256];
struct cache { unsigned blocklights[MAX_BLOCKLIGHTS]; unsigned char surfdat[MAX_SURF]; };

static void R_BuildLightMap(struct cache *c, const struct surface *surf, const unsigned *lightadj, int ambientlight, const float *dlight)
{
    int smax = surf->smax, tmax = surf->tmax, size = smax * tmax, i, t;
    const unsigned char *lightmap = surf->samples;
    for (i = 0; i < size; i++) c->blocklights[i] = (unsigned)ambientlight << 8;
    for (int maps = 0; maps < surf->styles; maps++) {
        unsigned scale = lightadj[maps];   // 8.8 fraction
        for (i = 0; i < size; i++) c->blocklights[i] += lightmap[i] * scale;
        lightmap += size;
    }
    if (surf->lit) {   /* R_AddDynamicLights for one light: its place on the surface's plane, its radius */
        float rad = dlight[2], minlight = 0, local[2] = {dlight[0] - surf->texturemins[0], dlight[1] - surf->texturemins[1]};
        minlight = rad - minlight;
        for (t = 0; t < tmax; t++) {
            int td = (int)local[1] - t * 16;
            if (td < 0) td = -td;
            for (int s = 0; s < smax; s++) {
                int sd = (int)local[0] - s * 16, dist;
                if (sd < 0) sd = -sd;
                if (sd > td) dist = sd + (td >> 1);
                else dist = td + (sd >> 1);
                if (dist < minlight) c->blocklights[t * smax + s] += (unsigned)((rad - dist) * 256);
            }
        }
    }
    for (i = 0; i < size; i++) {   // bound, invert, and shift
        t = (255 * 256 - (int)c->blocklights[i]) >> (8 - VID_CBITS);
        if (t < (1 << 6)) t = (1 << 6);
        c->blocklights[i] = (unsigned)t;
    }
}

static void R_DrawSurfaceBlock8_mip0(const unsigned char *psource, unsigned char *prowdest, const unsigned *r_lightptr, int r_lightwidth,
                                     int r_numvblocks, int surfrowbytes, const unsigned char *r_sourcemax, int r_stepback)
{
    int v, i, b, lightstep, lighttemp, light, lightleft, lightright, lightleftstep, lightrightstep;
    unsigned char pix;
    for (v = 0; v < r_numvblocks; v++) {
        lightleft = (int)r_lightptr[0];
        lightright = (int)r_lightptr[1];
        r_lightptr += r_lightwidth;
        lightleftstep = ((int)r_lightptr[0] - lightleft) >> 4;
        lightrightstep = ((int)r_lightptr[1] - lightright) >> 4;
        for (i = 0; i < 16; i++) {
            lighttemp = lightleft - lightright;
            lightstep = lighttemp >> 4;
            light = lightright;
            for (b = 15; b >= 0; b--) {
                pix = psource[b];
                prowdest[b] = colormap[(light & 0xFF00) + pix];
                light += lightstep;
            }
            psource += TEXW;
            lightright += lightrightstep;
            lightleft += lightleftstep;
            prowdest += surfrowbytes;
        }
        if (psource >= r_sourcemax) psource -= r_stepback;
    }
}

static void R_DrawSurface(struct cache *c, const struct surface *surf)
{
    int smax = TEXW, tmax = TEXW, r_stepback = tmax * TEXW, soffset = surf->texturemins[0], basetoffset = surf->texturemins[1];
    const unsigned char *r_sourcemax = texture + tmax * smax, *basetptr;
    unsigned char *pcolumndest = c->surfdat;
    soffset = (soffset + (smax << 16)) % smax;
    basetptr = &texture[(((basetoffset + (tmax << 16)) % tmax) * TEXW)];
    for (int u = 0; u < surf->width >> 4; u++) {
        R_DrawSurfaceBlock8_mip0(basetptr + soffset, pcolumndest, c->blocklights + u, surf->smax, surf->height >> 4, surf->width, r_sourcemax, r_stepback);
        soffset = soffset + 16;
        if (soffset >= smax) soffset = 0;
        pcolumndest += 16;
    }
}

static void setup(void)
{
    uint32_t seed = 0x5a7f;
    for (int i = 0; i < TEXW * TEXW; i++) texture[i] = (unsigned char)((((i >> 2) ^ (i >> 8)) & 3) * 16 + (wl_rand(&seed) & 15));
    for (int l = 0; l < 64; l++)
        for (int c = 0; c < 256; c++) colormap[l * 256 + c] = (unsigned char)((c & 0xf0) | ((c & 15) * (63 - l) >> 6));
    for (int s = 0; s < SURFS; s++) {
        struct surface *f = &surfaces[s];
        f->width = 64 << (s % 3 == 2 ? 2 : s % 2), f->height = 64 << (s % 4 == 3);
        if (f->width > 256) f->width = 256;
        f->smax = (f->width >> 4) + 1, f->tmax = (f->height >> 4) + 1;
        f->styles = 1 + s % 3, f->lit = s % 3 == 0;
        f->texturemins[0] = (int)(wl_rand(&seed) % 32) * 16 - 256, f->texturemins[1] = (int)(wl_rand(&seed) % 32) * 16 - 256;   /* on 16, as a BSP surface's are */
        for (int i = 0; i < f->styles * f->smax * f->tmax; i++) f->samples[i] = (unsigned char)(60 + wl_rand(&seed) % 160);
    }
}

static void *inst(void) { return wl_alloc(sizeof(struct cache)); }

static uint32_t run(void *ctx)
{
    struct cache *c = ctx;
    uint32_t sum = 0;
    static const unsigned lightadj[MAXLIGHTMAPS] = {256, 180, 90, 300};
    for (int s = 0; s < SURFS; s++) {
        const struct surface *f = &surfaces[s];
        float dlight[3] = {f->texturemins[0] + f->width * 0.4f, f->texturemins[1] + f->height * 0.6f, 200};
        R_BuildLightMap(c, f, lightadj, 10, dlight);
        R_DrawSurface(c, f);
        for (int i = 0; i < f->width * f->height; i += 61) sum = sum * 31 + c->surfdat[i];
    }
    return sum;
}

const wl_kernel wl_q1_surf = {"q1_surf", "Quake: the surface cache, R_BuildLightMap and R_DrawSurfaceBlock8", setup, inst, run};
