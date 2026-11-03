// SPDX-License-Identifier: GPL-3.0-or-later
// ShaderEmu: sinf, cosf and sincosf for OpenCNC3D's renderer, in floats (game.sh renames the renderer's calls to
// these; the brain and the libraries keep musl's). The sea's warp asks for sinf and cosf of phase + 256 (x + z),
// up to 32,768 radians, and musl reduces such an argument in doubles, which this machine has only in software:
// 5,255 instructions a call, 2,018 calls a frame, the most of a frame's water (docs/games/opencnc3d.md, "The sea's
// sines"). Here the argument is reduced by pi/2 in five parts of 7 bits each, every product exact for a quotient
// under 2^17 (about 205,887 radians), so the reduced argument is good to about 2^-30 of the quotient; the
// polynomials are Cephes' (single precision, about 1e-7 on [-pi/4, pi/4]). Past that, or for a NaN or an
// infinity, musl's. Against the true sine and cosine of a float argument, simulated in float32 over 40,000 random
// arguments in +-200,000 and +-10 and the sea's 256 k + 0.3: within 3.4e-7.
#include <math.h>

static void reduce(float x, float* s, float* c)
{
    const float fk = x * 0.636619772f;
    const int k = (int)(fk + (fk >= 0.0f ? 0.5f : -0.5f));
    const float f = (float)k;
    // pi/2 in five parts of at most 7 significant bits, each taken greedily from what the last left (2.6e-12)
    float r = x - f * 0x1.9p0f;
    r -= f * 0x1.0cp-7f;
    r -= f * 0x1.ecp-14f;
    r -= f * 0x1.5p-22f;
    r -= f * 0x1.1p-30f;
    const float z = r * r;
    const float sr = r + r * z * (-1.6666654611e-1f + z * (8.3321608736e-3f + z * -1.9515295891e-4f));
    const float cr = 1.0f - 0.5f * z + z * z * (4.166664568298827e-2f + z * (-1.388731625493765e-3f + z * 2.443315711809948e-5f));
    switch (k & 3) {
    case 0: *s = sr; *c = cr; break;
    case 1: *s = cr; *c = -sr; break;
    case 2: *s = -sr; *c = -cr; break;
    default: *s = -cr; *c = sr; break;
    }
}

static int fast(float x)
{
    return x > -200000.0f && x < 200000.0f;     // (false for a NaN)
}

float ocnc_sinf(float x)
{
    float s, c;
    if (!fast(x))
        return sinf(x);
    reduce(x, &s, &c);
    return s;
}

float ocnc_cosf(float x)
{
    float s, c;
    if (!fast(x))
        return cosf(x);
    reduce(x, &s, &c);
    return c;
}

void ocnc_sincosf(float x, float* s, float* c)
{
    if (!fast(x)) {
        *s = sinf(x);
        *c = cosf(x);
        return;
    }
    reduce(x, s, c);
}
