# A reference for the window's rain (unity/ShaderEmu/Shaders/WindowRain.shader): a simulation of
# drops on an upright pane, run offline only, to look at and to match the shader against. Nothing
# of it is used at run time.
#   python world/rain_reference.py [seconds]
# Writes build/rain_reference/frame_NNN.png (the water's height, lit) and tracks.png (where the
# sliding drops went, coloured by time).
#
# The model is the published one, not made up:
# - Kaneda, Kagawa & Yamashita, "Animation of Water Droplets on a Glass Plate", Computer Animation
#   '93: the glass a grid with a random affinity for water f in [0,1] at every point (its flaws); a
#   drop runs if its mass is over a static critical mass and stops under a dynamic one; it steps to
#   one of the three cells below, to a wet one if there is one (straight on first, else the fuller),
#   else to the one with the largest w1(k) f (w1 = 0.8, 1, 0.8 on an upright pane); on the cell it
#   leaves it leaves m' = w2 f (m - f m_min) + f m_min (at most f m_max); a drop is a cap of a sphere
#   at the contact angle (their eq. 11), and drops that overlap are one (a union).
# - Le Grand, Daerr & Limat, "Shape and motion of drops sliding down an inclined plane", J. Fluid
#   Mech. 541 (2005): a drop slides above a critical Bond number set by the hysteresis (their eq.
#   4.2), reaches its steady speed within millimetres, and that speed is linear in its Bond number,
#   Ca = eta U / gamma = c (Bo - Bo_c), Bo = V^(2/3) rho g / gamma: so its speed is its volume's, and
#   it goes faster as it takes in water.
# Rain lands as small drops at random places; drops that touch run together into the larger; still
# drops dry as their contact radius (diffusion-limited evaporation).
import math
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "build", "rain_reference")

# water on window glass
RHO, G, GAMMA, ETA = 1000.0, 9.81, 0.072, 0.001
THETA_A, THETA_R = math.radians(55), math.radians(35)
THETA = 0.5 * (THETA_A + THETA_R)          # a still drop's contact angle
C_SLOPE = 0.009                             # Le Grand et al., fig. 7: Ca against Bo, 0.0067 to 0.011


def bo_c(ta, tr):
    """Le Grand et al., eq. 4.2 (after Dussan V. 1985)."""
    return ((24 / math.pi) ** (1 / 3) * (math.cos(tr) - math.cos(ta)) * (1 + math.cos(ta)) ** 0.5
            / ((2 + math.cos(ta)) ** (1 / 3) * (1 - math.cos(ta)) ** (1 / 6)))


BO_C = bo_c(THETA_A, THETA_R)
V_STATIC = (BO_C * GAMMA / (RHO * G)) ** 1.5 * 1e9          # mm^3: over it a drop runs
V_DYNAMIC = V_STATIC * 0.08 / 0.15                           # Kaneda et al., table 1: m_c^d / m_c^s
UNIT = V_STATIC / 0.15                                       # Kaneda's mass unit, in mm^3
M_MIN, M_MAX, W2 = 0.001 * UNIT, 0.1 * UNIT, 0.1             # Kaneda et al., table 1
W1 = {-1: 0.8, 0: 1.0, 1: 0.8}
WET_FASTER = 5.0 / 4.0                                       # Kaneda: a_w / a_d

CELL = 1.0                                   # mm
W, H = 240, 480                              # cells: 0.24 by 0.48 m of the pane
RAIN = 1200.0                                # drops landing a square metre a second
EVAP = 0.004                                 # mm^2/s: dV/dt = -EVAP a


def cap_radii(v):
    """A drop of volume v (mm^3) as a cap at THETA: its sphere's radius R and its contact radius a."""
    k = math.pi * (2 - 3 * math.cos(THETA) + math.cos(THETA) ** 3) / 3
    R = (np.maximum(v, 0) / k) ** (1 / 3)
    return R, R * math.sin(THETA)


def speed(v, wet):
    """Le Grand et al.: steady speed (mm/s) of a drop of v mm^3; Bo_c for a running one is its dynamic threshold."""
    bo = (v * 1e-9) ** (2 / 3) * RHO * G / GAMMA
    bo_d = (V_DYNAMIC * 1e-9) ** (2 / 3) * RHO * G / GAMMA
    u = GAMMA / ETA * C_SLOPE * max(bo - bo_d, 0.0) * 1000.0
    return u * (WET_FASTER if wet else 1.0)


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 40.0
    os.makedirs(OUT, exist_ok=True)
    rng = np.random.default_rng(5)
    V = np.zeros((H, W))                     # row 0 the top
    f = rng.random((H, W))                   # Kaneda's affinity: the glass's flaws
    movers = []                              # [row, col, progress, its trail's last cells, id]
    starts = []
    tracks = []                              # (t, row, col, volume)
    dt = 1.0 / 120.0
    steps = int(seconds / dt)
    frames, frame_every = 0, int(2.0 / dt)

    def absorb(r, c, v_into, own=()):
        """Take in every still drop touching a drop of v_into at (r, c), but its own trail: gives the volume gained."""
        _, a = cap_radii(v_into)
        reach = int(math.ceil(a + cap_radii(M_MAX * 4)[1])) + 1
        r0, r1, c0, c1 = max(r - reach, 0), min(r + reach + 1, H), max(c - reach, 0), min(c + reach + 1, W)
        block = V[r0:r1, c0:c1]
        rr, cc = np.nonzero(block)
        gained = 0.0
        for y, x in zip(rr + r0, cc + c0):
            if (y, x) == (r, c) or (y, x) in own or any(m[0] == y and m[1] == x for m in movers):
                continue
            if math.hypot(y - r, x - c) * CELL < a + cap_radii(V[y, x])[1]:
                gained += V[y, x]
                V[y, x] = 0.0
        return gained

    for step in range(steps):
        t = step * dt
        # rain: small drops at random places, run together with what they touch
        n = rng.poisson(RAIN * (W * H * 1e-6) * dt)
        for _ in range(n):
            r, c = rng.integers(0, H), rng.integers(0, W)
            v = math.exp(rng.uniform(math.log(0.05), math.log(4.0)))
            V[r, c] += v
            V[r, c] += absorb(r, c, V[r, c])
            if V[r, c] > V_STATIC and not any(m[0] == r and m[1] == c for m in movers):
                movers.append([r, c, 0.0, [], len(starts)])
                starts.append(t)
        # still drops dry, as their contact radius
        still = V > 0
        _, a = cap_radii(V)
        V[still] = np.maximum(V[still] - EVAP * a[still] * dt, 0.0)
        # movers: Kaneda's step rule, at Le Grand's speed
        alive = []
        for m in movers:
            r, c, p, trail, ident = m
            v = V[r, c]
            if v < V_DYNAMIC:
                continue                      # it stops: a still drop now
            ahead_wet = r + 1 < H and V[r + 1, max(c - 1, 0):c + 2].any()   # Kaneda: faster on a wet surface
            p += speed(v, ahead_wet) * dt / CELL
            gone = False
            while p >= 1.0:
                if r + 1 >= H:
                    V[r, c] = 0.0
                    gone = True
                    break
                cand = [k for k in (-1, 0, 1) if 0 <= c + k < W]
                wet = [k for k in cand if V[r + 1, c + k] > 0]
                if wet:
                    k = 0 if 0 in wet else max(wet, key=lambda k: V[r + 1, c + k])
                else:
                    k = max(cand, key=lambda k: W1[k] * f[r + 1, c + k])
                fr = f[r, c]
                if v < fr * M_MIN:
                    left = v
                elif v <= fr * M_MIN + (M_MAX - M_MIN) / W2:
                    left = W2 * fr * (v - fr * M_MIN) + fr * M_MIN
                else:
                    left = fr * M_MAX
                left = min(left, v)
                V[r, c] = left
                trail.append((r, c))
                del trail[:-12]                   # (what it left within its own reach is its trail, not a drop it meets)
                r, c = r + 1, c + k
                v = V[r, c] + (v - left)
                V[r, c] = v
                V[r, c] += absorb(r, c, v, set(trail))
                v = V[r, c]
                p -= 1.0
                if v < V_DYNAMIC:
                    break
            if not gone and V[r, c] >= V_DYNAMIC:
                alive.append([r, c, p, trail, ident])
                tracks.append((t, r, c, V[r, c], ident))
        movers = alive
        if step % frame_every == 0:
            render(V, movers, os.path.join(OUT, "frame_%03d.png" % frames))
            frames += 1
    render(V, movers, os.path.join(OUT, "last.png"), scale=3)
    track_picture(tracks, seconds)
    statistics(tracks)
    print("RAIN REFERENCE: Bo_c %.3f, static %.2f mm^3, dynamic %.2f mm^3, %d frames, %d track points" % (BO_C, V_STATIC, V_DYNAMIC, frames, len(tracks)))


def render(V, movers, path, scale=2, k_merge=0.06):
    """The water's height: every drop a cap (Kaneda eq. 9-11), joined by a smooth union (log-sum-exp
    over k_merge mm); lit from the upper left, so its shape shows."""
    hgt, wid = H * scale, W * scale
    acc = np.zeros((hgt, wid))   # sum of exp(h / k)
    any_ = np.zeros((hgt, wid), bool)
    rr, cc = np.nonzero(V)
    for r, c in zip(rr, cc):
        R, a = cap_radii(V[r, c])
        R, a = float(R), float(a)
        n = int(math.ceil(a * scale)) + 1
        y0, x0 = int(r * scale), int(c * scale)
        ys = np.arange(max(y0 - n, 0), min(y0 + n + 1, hgt))
        xs = np.arange(max(x0 - n, 0), min(x0 + n + 1, wid))
        if len(ys) == 0 or len(xs) == 0:
            continue
        dy = (ys[:, None] + 0.5) / scale - (r + 0.5)
        dx = (xs[None, :] + 0.5) / scale - (c + 0.5)
        d2 = dx * dx + dy * dy
        inside = d2 < a * a
        h = np.where(inside, np.sqrt(np.maximum(R * R - d2, 0)) - R * math.cos(THETA), 0.0)
        sub = acc[ys[0]:ys[-1] + 1, xs[0]:xs[-1] + 1]
        sub += np.where(inside, np.exp(h / k_merge), 0.0)
        any_[ys[0]:ys[-1] + 1, xs[0]:xs[-1] + 1] |= inside
    height = np.where(any_, k_merge * np.log(np.maximum(acc, 1e-30)), 0.0)
    gy, gx = np.gradient(height, 1.0 / scale)
    light = np.clip(0.55 - 0.35 * gx - 0.35 * gy, 0, 1)
    img = np.where(any_, light, 0.06)
    rgb = np.stack([img * 0.85, img * 0.9, img], -1)
    for r, c, *_ in movers:   # where the sliding drops are
        y, x = int(r * scale), int(c * scale)
        rgb[max(y - 1, 0):y + 2, max(x - 1, 0):x + 2] = (1.0, 0.5, 0.1)
    Image.fromarray((rgb * 255).astype(np.uint8)).save(path)


def statistics(tracks):
    """What the shader is matched to: a sliding drop's sideways wander against the way gone (the
    mean square of x(s + L) - x(s)), its speed against its volume, and how far drops go."""
    by = {}
    for t, r, c, v, ident in tracks:
        by.setdefault(ident, []).append((t, r, c, v))
    lengths = sorted(len(set(p[1] for p in pts)) for pts in by.values())
    print("STATS drops %d, way gone (mm): median %d, 90%% %d" % (len(by), lengths[len(lengths) // 2], lengths[int(len(lengths) * 0.9)]))
    for L in (2, 5, 10, 20, 50, 100):
        d = []
        for pts in by.values():
            xs = {}
            for t, r, c, v in pts:
                xs[r] = c
            for r, c in xs.items():
                if r + L in xs:
                    d.append((xs[r + L] - c) ** 2)
        if d:
            print("STATS wander over %3d mm: rms %.2f mm (%d pairs)" % (L, math.sqrt(sum(d) / len(d)), len(d)))
    first = sorted(pts[0][3] for pts in by.values())
    print("STATS volume when it sets off: 10%% %.1f, 50%% %.1f, 90%% %.1f mm^3" % tuple(first[int(q * (len(first) - 1))] for q in (0.1, 0.5, 0.9)))
    rates = []
    for pts in by.values():
        if len(pts) > 6:
            s0, s1 = pts[0][1], pts[-1][1]
            if s1 > s0:
                rates.append(math.log(pts[-1][3] / pts[0][3]) / (s1 - s0))
    rates.sort()
    if rates:
        print("STATS net change of ln(volume) a mm of the way: 10%% %.4f, 50%% %.4f, 90%% %.4f" % tuple(rates[int(q * (len(rates) - 1))] for q in (0.1, 0.5, 0.9)))
    vs = sorted(p[3] for pts in by.values() for p in pts)
    if vs:
        for q in (0.1, 0.5, 0.9):
            v = vs[int(q * (len(vs) - 1))]
            print("STATS sliding volume %d%%: %.1f mm^3, speed %.0f mm/s, contact radius %.2f mm" % (q * 100, v, speed(v, False), float(cap_radii(v)[1])))


def track_picture(tracks, seconds):
    img = np.full((H, W, 3), 12, np.uint8)
    for t, r, c, v, _ in tracks:
        u = t / seconds
        img[r, c] = (int(255 * u), int(120 + 100 * (1 - u)), int(255 * (1 - u)))
    Image.fromarray(img).resize((W * 2, H * 2), Image.NEAREST).save(os.path.join(OUT, "tracks.png"))


if __name__ == "__main__":
    main()
