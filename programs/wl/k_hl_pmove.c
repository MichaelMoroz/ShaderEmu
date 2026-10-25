/*
 * hl_pmove: Half-Life's player movement, which its server runs for every player's every command and its client
 * again for prediction (hlsdk pm_shared.c, copied): PM_Friction, PM_Accelerate, PM_WalkMove with its step up and
 * down, PM_FlyMove's four bumps and clip planes, PM_ClipVelocity, gravity and PM_CategorizePosition's ground
 * trace. PM_PlayerTrace here sweeps the player's box against a room's 48 boxes (pm_trace.c's box hulls, each
 * box grown by the player's half size as PM_HullForBox does, the slab test of its planes). A unit: 16 players
 * moved 10 commands each at 100 frames a second, running, turning and strafing into the boxes.
 */
#include <math.h>
#include <string.h>
#include "wl.h"

#define BOXES 48
#define PLAYERS 16
#define COMMANDS 10
#define MAX_CLIP_PLANES 5
#define STOP_EPSILON 0.1f
#define DIST_EPSILON (1.0f / 32)
#define DotProduct(a, b) ((a)[0] * (b)[0] + (a)[1] * (b)[1] + (a)[2] * (b)[2])
#define VectorCopy(a, b) ((b)[0] = (a)[0], (b)[1] = (a)[1], (b)[2] = (a)[2])
#define VectorClear(a) ((a)[0] = (a)[1] = (a)[2] = 0)

typedef float vec3_t[3];
typedef struct { int allsolid, startsolid; float fraction; vec3_t endpos; struct { vec3_t normal; float dist; } plane; } pmtrace_t;
typedef struct {
    vec3_t origin, velocity, forward, right, up, basevelocity;
    int onground, dead, waterjumptime, waterlevel, movetype;
    float frametime, friction, maxspeed;
    struct { float forwardmove, sidemove; } cmd;
} playermove_t;
struct movevars { float gravity, stopspeed, maxspeed, accelerate, friction, edgefriction, stepsize, bounce; };
static const struct movevars mv = {800, 100, 320, 10, 4, 2, 18, 1};
static const vec3_t player_mins = {-16, -16, -36}, player_maxs = {16, 16, 36};
static vec3_t box_min[BOXES], box_max[BOXES];
static vec3_t start_origin[PLAYERS];

/* the player's box swept from start to end against every box (each grown by the player's size): the nearest hit */
static pmtrace_t PM_PlayerTrace(const vec3_t start, const vec3_t end)
{
    pmtrace_t tr;
    memset(&tr, 0, sizeof tr);
    tr.fraction = 1;
    for (int b = 0; b < BOXES; b++) {
        float enter = -1, leave = 1;
        int axis = -1, side = 0, inside = 1;
        for (int i = 0; i < 3; i++) {   /* the box hull's six planes, clipped as SV_ClipToLinks' hull check */
            float lo = box_min[b][i] - player_maxs[i], hi = box_max[b][i] - player_mins[i];
            float d1 = lo - start[i], d2 = lo - end[i];   /* the min plane, normal -axis */
            if (start[i] < lo) {
                inside = 0;
                if (end[i] < lo) goto miss;
                float f = (d1 - DIST_EPSILON) / (d1 - d2);
                if (f > enter) enter = f, axis = i, side = -1;
            } else if (end[i] < lo) {
                float f = d1 / (d1 - d2);
                if (f < leave) leave = f;
            }
            d1 = start[i] - hi, d2 = end[i] - hi;   /* the max plane, normal +axis */
            if (start[i] > hi) {
                inside = 0;
                if (end[i] > hi) goto miss;
                float f = (d1 - DIST_EPSILON) / (d1 - d2);
                if (f > enter) enter = f, axis = i, side = 1;
            } else if (end[i] > hi) {
                float f = d1 / (d1 - d2);
                if (f < leave) leave = f;
            }
        }
        if (inside) {
            tr.startsolid = 1;
            if (leave >= 1) tr.allsolid = 1;
            continue;
        }
        if (enter < leave && enter < tr.fraction) {
            if (enter < 0) enter = 0;
            tr.fraction = enter;
            VectorClear(tr.plane.normal);
            tr.plane.normal[axis] = (float)side;
        }
    miss:;
    }
    for (int i = 0; i < 3; i++) tr.endpos[i] = start[i] + tr.fraction * (end[i] - start[i]);
    return tr;
}

static float VectorNormalize(vec3_t v)
{
    float length = sqrtf(DotProduct(v, v));
    if (length) {
        float ilength = 1 / length;
        v[0] *= ilength, v[1] *= ilength, v[2] *= ilength;
    }
    return length;
}

static int PM_ClipVelocity(vec3_t in, vec3_t normal, vec3_t out, float overbounce)
{
    float backoff, change, angle = normal[2];
    int i, blocked = 0;
    if (angle > 0) blocked |= 0x01;
    if (!angle) blocked |= 0x02;
    backoff = DotProduct(in, normal) * overbounce;
    for (i = 0; i < 3; i++) {
        change = normal[i] * backoff;
        out[i] = in[i] - change;
        if (out[i] > -STOP_EPSILON && out[i] < STOP_EPSILON) out[i] = 0;
    }
    return blocked;
}

static void PM_Accelerate(playermove_t *pmove, vec3_t wishdir, float wishspeed, float accel)
{
    float addspeed, accelspeed, currentspeed;
    if (pmove->dead || pmove->waterjumptime) return;
    currentspeed = DotProduct(pmove->velocity, wishdir);
    addspeed = wishspeed - currentspeed;
    if (addspeed <= 0) return;
    accelspeed = accel * pmove->frametime * wishspeed * pmove->friction;
    if (accelspeed > addspeed) accelspeed = addspeed;
    for (int i = 0; i < 3; i++) pmove->velocity[i] += accelspeed * wishdir[i];
}

static void PM_Friction(playermove_t *pmove)
{
    float *vel = pmove->velocity, speed, newspeed, control, friction, drop = 0;
    if (pmove->waterjumptime) return;
    speed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    if (speed < 0.1f) return;
    if (pmove->onground != -1) {
        vec3_t start, stop;
        start[0] = stop[0] = pmove->origin[0] + vel[0] / speed * 16;
        start[1] = stop[1] = pmove->origin[1] + vel[1] / speed * 16;
        start[2] = pmove->origin[2] + player_mins[2];
        stop[2] = start[2] - 34;
        pmtrace_t trace = PM_PlayerTrace(start, stop);
        friction = trace.fraction == 1.0f ? mv.friction * mv.edgefriction : mv.friction;
        friction *= pmove->friction;
        control = (speed < mv.stopspeed) ? mv.stopspeed : speed;
        drop += control * friction * pmove->frametime;
    }
    newspeed = speed - drop;
    if (newspeed < 0) newspeed = 0;
    newspeed /= speed;
    vel[0] *= newspeed, vel[1] *= newspeed, vel[2] *= newspeed;
}

static int PM_FlyMove(playermove_t *pmove)
{
    int bumpcount, numbumps = 4, numplanes = 0, i, j, blocked = 0;
    vec3_t dir, planes[MAX_CLIP_PLANES], primal_velocity, original_velocity, new_velocity, end;
    float d, time_left = pmove->frametime, allFraction = 0;
    pmtrace_t trace;
    VectorCopy(pmove->velocity, original_velocity);
    VectorCopy(pmove->velocity, primal_velocity);
    for (bumpcount = 0; bumpcount < numbumps; bumpcount++) {
        if (!pmove->velocity[0] && !pmove->velocity[1] && !pmove->velocity[2]) break;
        for (i = 0; i < 3; i++) end[i] = pmove->origin[i] + time_left * pmove->velocity[i];
        trace = PM_PlayerTrace(pmove->origin, end);
        allFraction += trace.fraction;
        if (trace.allsolid) {
            VectorClear(pmove->velocity);
            return 4;
        }
        if (trace.fraction > 0) {
            VectorCopy(trace.endpos, pmove->origin);
            VectorCopy(pmove->velocity, original_velocity);
            numplanes = 0;
        }
        if (trace.fraction == 1) break;
        if (trace.plane.normal[2] > 0.7f) blocked |= 1;
        if (!trace.plane.normal[2]) blocked |= 2;
        time_left -= time_left * trace.fraction;
        if (numplanes >= MAX_CLIP_PLANES) {
            VectorClear(pmove->velocity);
            break;
        }
        VectorCopy(trace.plane.normal, planes[numplanes]);
        numplanes++;
        if (numplanes == 1 && pmove->movetype == 3 && ((pmove->onground == -1) || (pmove->friction != 1))) {
            for (i = 0; i < numplanes; i++) {
                if (planes[i][2] > 0.7f) {
                    PM_ClipVelocity(original_velocity, planes[i], new_velocity, 1);
                    VectorCopy(new_velocity, original_velocity);
                } else PM_ClipVelocity(original_velocity, planes[i], new_velocity, 1.0f + mv.bounce * (1 - pmove->friction));
            }
            VectorCopy(new_velocity, pmove->velocity);
            VectorCopy(new_velocity, original_velocity);
        } else {
            for (i = 0; i < numplanes; i++) {
                PM_ClipVelocity(original_velocity, planes[i], pmove->velocity, 1);
                for (j = 0; j < numplanes; j++)
                    if (j != i && DotProduct(pmove->velocity, planes[j]) < 0) break;
                if (j == numplanes) break;
            }
            if (i == numplanes) {
                if (numplanes != 2) {
                    VectorClear(pmove->velocity);
                    break;
                }
                dir[0] = planes[0][1] * planes[1][2] - planes[0][2] * planes[1][1];
                dir[1] = planes[0][2] * planes[1][0] - planes[0][0] * planes[1][2];
                dir[2] = planes[0][0] * planes[1][1] - planes[0][1] * planes[1][0];
                d = DotProduct(dir, pmove->velocity);
                pmove->velocity[0] = dir[0] * d, pmove->velocity[1] = dir[1] * d, pmove->velocity[2] = dir[2] * d;
            }
            if (DotProduct(pmove->velocity, primal_velocity) <= 0) {
                VectorClear(pmove->velocity);
                break;
            }
        }
    }
    if (allFraction == 0) VectorClear(pmove->velocity);
    return blocked;
}

static void PM_WalkMove(playermove_t *pmove)
{
    int oldonground, i;
    vec3_t wishvel, wishdir, dest, original, originalvel, down, downvel;
    float spd, fmove = pmove->cmd.forwardmove, smove = pmove->cmd.sidemove, wishspeed, downdist, updist;
    pmtrace_t trace;
    pmove->forward[2] = 0, pmove->right[2] = 0;
    VectorNormalize(pmove->forward);
    VectorNormalize(pmove->right);
    for (i = 0; i < 2; i++) wishvel[i] = pmove->forward[i] * fmove + pmove->right[i] * smove;
    wishvel[2] = 0;
    VectorCopy(wishvel, wishdir);
    wishspeed = VectorNormalize(wishdir);
    if (wishspeed > pmove->maxspeed) {
        float k = pmove->maxspeed / wishspeed;
        wishvel[0] *= k, wishvel[1] *= k, wishvel[2] *= k;
        wishspeed = pmove->maxspeed;
    }
    pmove->velocity[2] = 0;
    PM_Accelerate(pmove, wishdir, wishspeed, mv.accelerate);
    pmove->velocity[2] = 0;
    for (i = 0; i < 3; i++) pmove->velocity[i] += pmove->basevelocity[i];
    spd = sqrtf(DotProduct(pmove->velocity, pmove->velocity));
    if (spd < 1.0f) {
        VectorClear(pmove->velocity);
        return;
    }
    oldonground = pmove->onground;
    dest[0] = pmove->origin[0] + pmove->velocity[0] * pmove->frametime;
    dest[1] = pmove->origin[1] + pmove->velocity[1] * pmove->frametime;
    dest[2] = pmove->origin[2];
    trace = PM_PlayerTrace(pmove->origin, dest);
    if (trace.fraction == 1) {
        VectorCopy(trace.endpos, pmove->origin);
        return;
    }
    if (oldonground == -1 && pmove->waterlevel == 0) return;
    if (pmove->waterjumptime) return;
    VectorCopy(pmove->origin, original);
    VectorCopy(pmove->velocity, originalvel);
    PM_FlyMove(pmove);
    VectorCopy(pmove->origin, down);
    VectorCopy(pmove->velocity, downvel);
    VectorCopy(original, pmove->origin);
    VectorCopy(originalvel, pmove->velocity);
    VectorCopy(pmove->origin, dest);
    dest[2] += mv.stepsize;
    trace = PM_PlayerTrace(pmove->origin, dest);
    if (!trace.startsolid && !trace.allsolid) VectorCopy(trace.endpos, pmove->origin);
    PM_FlyMove(pmove);
    VectorCopy(pmove->origin, dest);
    dest[2] -= mv.stepsize;
    trace = PM_PlayerTrace(pmove->origin, dest);
    if (trace.plane.normal[2] < 0.7f) goto usedown;
    if (!trace.startsolid && !trace.allsolid) VectorCopy(trace.endpos, pmove->origin);
    VectorCopy(pmove->origin, pmove->up);
    downdist = (down[0] - original[0]) * (down[0] - original[0]) + (down[1] - original[1]) * (down[1] - original[1]);
    updist = (pmove->up[0] - original[0]) * (pmove->up[0] - original[0]) + (pmove->up[1] - original[1]) * (pmove->up[1] - original[1]);
    if (downdist > updist) {
    usedown:
        VectorCopy(down, pmove->origin);
        VectorCopy(downvel, pmove->velocity);
    } else pmove->velocity[2] = downvel[2];
}

/* PM_CategorizePosition: on the ground if a trace 2 units down hits a floor */
static void PM_CategorizePosition(playermove_t *pmove)
{
    vec3_t point = {pmove->origin[0], pmove->origin[1], pmove->origin[2] - 2};
    if (pmove->velocity[2] > 180) {
        pmove->onground = -1;
        return;
    }
    pmtrace_t tr = PM_PlayerTrace(pmove->origin, point);
    if (tr.plane.normal[2] < 0.7f) pmove->onground = -1;
    else pmove->onground = 0;
    if (pmove->onground != -1 && !tr.startsolid && !tr.allsolid) VectorCopy(tr.endpos, pmove->origin);
}

static void setup(void)
{
    uint32_t seed = 0x9e70;
    /* the floor, four walls, then crates and steps of 8 to 24 units */
    box_min[0][0] = -1024, box_min[0][1] = -1024, box_min[0][2] = -64, box_max[0][0] = 1024, box_max[0][1] = 1024, box_max[0][2] = 0;
    for (int w = 0; w < 4; w++) {
        int b = 1 + w, ax = w & 1, sg = w < 2 ? -1 : 1;
        box_min[b][0] = box_min[b][1] = -1024, box_max[b][0] = box_max[b][1] = 1024, box_min[b][2] = 0, box_max[b][2] = 256;
        if (sg < 0) box_max[b][ax] = -960;
        else box_min[b][ax] = 960;
    }
    for (int b = 5; b < BOXES; b++) {
        float x = (wl_randf(&seed) * 2 - 1) * 900, y = (wl_randf(&seed) * 2 - 1) * 900, sx = 16 + wl_randf(&seed) * 96, sy = 16 + wl_randf(&seed) * 96;
        float h = (b & 3) == 0 ? 8 + (b % 3) * 8 : 32 + wl_randf(&seed) * 64;
        box_min[b][0] = x - sx, box_max[b][0] = x + sx, box_min[b][1] = y - sy, box_max[b][1] = y + sy, box_min[b][2] = 0, box_max[b][2] = h;
    }
    for (int p = 0; p < PLAYERS; p++) {   /* a start clear of every box */
        for (;;) {
            vec3_t o = {(wl_randf(&seed) * 2 - 1) * 900, (wl_randf(&seed) * 2 - 1) * 900, 37};
            pmtrace_t t = PM_PlayerTrace(o, o);
            if (!t.startsolid) {
                VectorCopy(o, start_origin[p]);
                break;
            }
        }
    }
}

static uint32_t run(void *ctx)
{
    (void)ctx;
    uint32_t sum = 0;
    for (int p = 0; p < PLAYERS; p++) {
        playermove_t pm, *pmove = &pm;
        memset(pmove, 0, sizeof pm);
        VectorCopy(start_origin[p], pmove->origin);
        pmove->velocity[0] = 200 * (float)((p & 1) * 2 - 1), pmove->velocity[1] = 80;
        pmove->frametime = 0.01f, pmove->friction = 1, pmove->maxspeed = mv.maxspeed, pmove->movetype = 3;
        for (int c = 0; c < COMMANDS; c++) {
            float yaw = p * 0.4f + c * 0.15f;
            pmove->forward[0] = cosf(yaw), pmove->forward[1] = sinf(yaw), pmove->forward[2] = 0;
            pmove->right[0] = sinf(yaw), pmove->right[1] = -cosf(yaw), pmove->right[2] = 0;
            pmove->cmd.forwardmove = 400, pmove->cmd.sidemove = (c & 4) ? 200.f : -150.f;
            PM_CategorizePosition(pmove);
            if (pmove->onground != -1) {
                pmove->velocity[2] = 0;
                PM_Friction(pmove);
                PM_WalkMove(pmove);
            } else {   /* PM_AirMove's gravity and slide */
                pmove->velocity[2] -= mv.gravity * pmove->frametime;
                PM_FlyMove(pmove);
            }
            PM_CategorizePosition(pmove);
        }
        sum = sum * 31 + (uint32_t)(int)(pmove->origin[0] * 8) + (uint32_t)(int)(pmove->origin[1] * 8) * 7 + (uint32_t)(int)pmove->origin[2];
    }
    return sum;
}

/* run() stores only to its stack: a scratch of nothing, so that every worker may run it */
static void *inst(void) { return wl_alloc(16); }

const wl_kernel wl_hl_pmove = {"hl_pmove", "Half-Life: PM_WalkMove, PM_FlyMove, PM_Friction, traces against box hulls", setup, inst, run};
