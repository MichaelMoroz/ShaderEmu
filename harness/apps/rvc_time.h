// rvc's timer value for a given _Time.y, computed on the host the way the shader does it
// (including its float round trips), for shaders built with NO_DOUBLES.
#pragma once

#include <cmath>
#include <cstdint>

inline void rvcMtime(double t, uint32_t& lo, uint32_t& hi) {
    float timeX = (float)(t / 20);
    double mtime = (double)timeX * 1000000.0 * 0.1;
    float whole = std::floor((float)(mtime / 4294967296.0));
    lo = (uint32_t)std::floor((float)(mtime - 4294967296.0 * whole));
    hi = (uint32_t)(mtime / 4294967296.0);
}
