// Fog that thins with height over the city (City.shader, Sky.shader): its density is
// _FogDensity a metre at _FogBase (the street) and falls by e every _FogFalloff metres up. A
// fragment's fog is that density summed along the eye's ray to it, in closed form: no depth
// texture, one exp more than a plain fog. The sky is the ray's end at infinity.
#ifndef SHADEREMU_HEIGHT_FOG
#define SHADEREMU_HEIGHT_FOG

float4 _FogColour, _FogFlash;
float _FogDensity, _FogBase, _FogFalloff;

// the fog's depth along `dir` (a unit vector) from the eye for `len` metres
float HeightFogDepth(float3 dir, float len)
{
    float at = _FogDensity * exp(-(_WorldSpaceCameraPos.y - _FogBase) / _FogFalloff);
    float k = max(dir.y * len / _FogFalloff, -30.0);   // how many falloffs the ray climbs (down: < 0)
    return at * len * (abs(k) > 1e-3 ? (1.0 - exp(-k)) / k : 1.0 - 0.5 * k);
}

// ... and to the sky: upwards it ends, level or downwards it never does
float HeightFogSkyDepth(float3 dir)
{
    float at = _FogDensity * exp(-(_WorldSpaceCameraPos.y - _FogBase) / _FogFalloff);
    return dir.y > 1e-4 ? at * _FogFalloff / dir.y : 1e4;
}

float3 HeightFogged(float3 c, float depth, float flash)
{
    return lerp(c, _FogColour.rgb + _FogFlash.rgb * 10.0 * flash, 1.0 - exp(-depth));
}

#endif
