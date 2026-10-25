/*
 * phys_step: nxphys's boxes (linux/physics): the library itself, physics.c compiled here unchanged, driven as
 * nxphys.c drives it with one share: phys_step with the look for contacts every PHYS_EVERY-th substep,
 * phys_grid the substep before a look, the three buffers in turn. The room is nxphys's (12 m each way, the grid's
 * cells PHYS_CELL); 120 boxes of mixed sizes dropped in a heap and let fall for 30 substeps in setup. A unit:
 * that state put back (states, contacts, grid), then 2 substeps (one look).
 */
#include <stdlib.h>
#include <string.h>
#include "wl.h"
#include "../../linux/physics/physics.c"

#define BODIES 120
#define ROOM 12.0f
#define ROOM_HIGH 8
#define SETTLE 30
#define SUBSTEPS 2

static struct phys world;
static struct body_state *start_states;
static struct body_info *start_info;
static struct manifold *start_manifolds;
static int base;

static void written_at(int buffer, int turn)
{
    for (int i = 0; i < world.bodies; i++) world.states[buffer][i].key = phys_key(&world, world.states[buffer][i].pos);
    phys_nears(&world, buffer);
    phys_grid_all(&world, buffer, turn);
}

/* nxphys's share_phase for one share, with no look ahead */
static void substep(int phase)
{
    int from = phase % PHYS_BUFFERS, kind = phase % PHYS_EVERY, turn = phase / PHYS_EVERY + 1;
    phys_step(&world, 0, 1, from, kind == 0 ? turn : 0, turn);
    if (kind == PHYS_EVERY - 1) phys_grid(&world, 0, from, (phase + 1) / PHYS_EVERY + 1);
}

static void setup(void)
{
    uint32_t seed = 0xb0c5;
    int cells;
    memset(&world, 0, sizeof world);
    world.look_ahead = (PHYS_EVERY + 1) * PHYS_DT;
    world.half_x = world.half_z = ROOM;
    world.gravity = 9.81f;
    world.gx = world.gz = (int)(2.0f * ROOM / PHYS_CELL);
    world.gy = ROOM_HIGH;
    world.cell_shares = 1, world.cell_first = 0;
    cells = world.gx * world.gy * world.gz;
    for (int k = 0; k < PHYS_BUFFERS; k++) {
        world.states[k] = wl_alloc(BODIES * sizeof(struct body_state));
        world.poses[k] = wl_alloc(BODIES * sizeof(struct body_pose));
        world.nears[k] = wl_alloc(BODIES * sizeof(struct body_near));
    }
    world.shapes = wl_alloc(BODIES * sizeof(struct body_shape));
    world.info = wl_alloc(BODIES * sizeof(struct body_info));
    world.manifolds = wl_alloc(BODIES * PHYS_MANIFOLDS * sizeof(struct manifold));
    world.grids[0] = wl_alloc((unsigned)cells * sizeof(struct gcell));
    world.grids[1] = wl_alloc((unsigned)cells * sizeof(struct gcell));
    world.bodies = BODIES;
    world.held = 0;
    for (int h = 0; h < PHYS_HOLDS; h++) world.holds[h].body = -1;
    for (int i = 0; i < BODIES; i++) {   /* nxphys's put(): upright, at rest, in a heap over the middle */
        struct body_state *s = &world.states[0][i];
        float hx = 0.15f + 0.3f * wl_randf(&seed), hy = 0.15f + 0.3f * wl_randf(&seed), hz = 0.15f + 0.3f * wl_randf(&seed);
        phys_box(&world, i, hx, hy, hz, 1.0f);
        s->pos[0] = -3 + 6 * wl_randf(&seed), s->pos[1] = 0.5f + (float)(i / 24) * 0.9f, s->pos[2] = -3 + 6 * wl_randf(&seed);
        s->q[3] = 1.0f;
    }
    written_at(0, 1);
    for (int p = 0; p < SETTLE; p++) substep(p);
    base = SETTLE;
    start_states = wl_alloc(BODIES * sizeof(struct body_state) * PHYS_BUFFERS);
    start_info = wl_alloc(BODIES * sizeof(struct body_info));
    start_manifolds = wl_alloc(BODIES * PHYS_MANIFOLDS * sizeof(struct manifold));
    for (int k = 0; k < PHYS_BUFFERS; k++) memcpy(start_states + k * BODIES, world.states[k], BODIES * sizeof(struct body_state));
    memcpy(start_info, world.info, BODIES * sizeof(struct body_info));
    memcpy(start_manifolds, world.manifolds, BODIES * PHYS_MANIFOLDS * sizeof(struct manifold));
}

static uint32_t run(void *ctx)
{
    uint32_t sum = 0;
    (void)ctx;
    for (int k = 0; k < PHYS_BUFFERS; k++) memcpy(world.states[k], start_states + k * BODIES, BODIES * sizeof(struct body_state));
    memcpy(world.info, start_info, BODIES * sizeof(struct body_info));
    memcpy(world.manifolds, start_manifolds, BODIES * PHYS_MANIFOLDS * sizeof(struct manifold));
    written_at(base % PHYS_BUFFERS, base / PHYS_EVERY + 1);
    for (int p = base; p < base + SUBSTEPS; p++) substep(p);
    const struct body_state *s = world.states[(base + SUBSTEPS) % PHYS_BUFFERS];
    for (int i = 0; i < BODIES; i++) sum = sum * 31 + wl_fold(s[i].pos[0] + s[i].pos[1] * 3 + s[i].pos[2] * 7);
    return sum;
}

const wl_kernel wl_phys_step = {"phys_step", "nxphys: physics.c's substeps, looks for contacts and grid", setup, 0, run};
