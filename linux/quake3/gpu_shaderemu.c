// The machine's GPU for Quake III Arena (docs/games/quake3.md): programs/linux/gles.c as it is (its
// window, its frames' two sets, its matrices, its swap), and after it draws whose every word
// the renderer says itself: its textures are in the ROM, which the library's own have no way
// to say, and a draw may be lit by a light map (docs/machine/gpu.md).

// A frame of this game is more vertices than the library's default has room for.
#define SET_SIZE 0x00080000u
#define UNIFORMS_IN 0x00030000u
#define VERTICES_IN 0x00038000u
#define MAX_COMMANDS 3072
#ifdef SE_HALVES
// A list built in two parts at once (Jedi Outcast's drawing on two workers: linux/jk2/gl_jk2.c, "The drawing on two
// workers"): the part this copy builds ends where these say, set by gpuHalfSplit (the first part's) and gpuHalfBegin (the
// second's, built by a second copy of this file, SE_DRAW_B), and put back by gpuHalfMerge
#include <stdint.h>
static uint32_t half_vertex_end, half_commands_end = 3072, half_blocks_end = 256, half_mesh_end = 196608;
#define GLES_VERTEX_END ( half_vertex_end ? half_vertex_end : set_at + SET_SIZE )
#define GLES_COMMANDS_END half_commands_end
static uint32_t used_last[4];   // matrix blocks refused (a draw with the block before's matrices), and at the last merge: blocks, KB, commands
#define GLES_BLOCKS_END ( half_blocks_end + ( blocks >= half_blocks_end ? ( used_last[0]++, 0u ) : 0u ) )
#define GLES_MESH_END half_mesh_end
#endif
#include "gles.c"
#include "gpu_shaderemu.h"

const volatile unsigned* gpuControl(int offset) {
    return gpu ? (const volatile unsigned*)(gpu + offset) : 0;
}

// Where the window's first pixel is on the display: its place in each window it is inside.
void gpuWindowOrigin(int* x, int* y) {
    GR_WINDOW_INFO info;
    *x = *y = 0;
    for (GR_WINDOW_ID id = window; gpu && id; id = info.parent) {
        GrGetWindowInfo(id, &info);
        *x += info.x, *y += info.y;
    }
}

unsigned gpuAddress(const void* memory) { return GPU_PHYS + (uint32_t)((const uint8_t*)memory - gpu); }

void gpuProjection(const float* columns) {
    matrix_mode = GL_PROJECTION;
    load_columns(columns, 0);
    matrix_mode = GL_MODELVIEW;
}
void gpuModelview(const float* columns) { load_columns(columns, 0); }

void gpuModelviewRows(const float* rows) {
    matrix* m = &modelview[modelview_top];
    for (int i = 0; i < 12; i++) m->r[i / 4][i % 4] = rows[i];
    // the block the draws that follow read: the one before with these rows in it (its other
    // words, the projection with the window folded in, are the same)
    if (matrices_dirty || !blocks || blocks >= GLES_BLOCKS_END) {
        matrices_dirty = 1;
        return;
    }
    num* u = (num*)(gpu + set_at + UNIFORMS_IN) + 32 * blocks;
    for (int i = 0; i < 16; i++) u[i] = u[i - 32];
    for (int i = 0; i < 12; i++) u[16 + i] = rows[i];
    blocks++;
}

// The next block of the frame's matrices with the projection of the one before, for the draws
// that follow: its three rows of four, for the caller to fill. 0 when the matrices changed
// otherwise since the last draw (gpuModelviewRows then).
float* gpuModelviewBlock(void) {
    if (matrices_dirty || !blocks || blocks >= GLES_BLOCKS_END) return 0;
    num* u = (num*)(gpu + set_at + UNIFORMS_IN) + 32 * blocks++;
    for (int i = 0; i < 16; i++) u[i] = u[i - 32];
    return u + 16;
}

// The words of a draw's command but for where its vertices are, how many, and its matrices.
static inline void command_state(uint32_t* c, const gpu_state* s) {
    c[4] = VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_COMPACT | VERTEX_FLOAT | s->vertex;
    c[5] = s->fragment;
    c[7] = s->texture;
    c[8] = s->width;
    c[9] = s->height;
    c[10] = s->key;
    c[11] = s->colour;
    c[12] = s->more[0], c[13] = s->more[1], c[14] = s->more[2], c[15] = s->more[3];
}

static gpu_state joined;      // what the frame's last command was made with, when more may join it
static uint32_t* joined_command;
static uint32_t joined_block, joined_end;   // its matrices, and where its vertices end
static uint32_t frame_last = 0xffffffffu;   // where gpuFrameMemory's last memory begins, nothing taken since (gpuFrameTrim)

static inline int same_state(const gpu_state* a, const gpu_state* b) {
    const uint32_t *x = (const uint32_t*)a, *y = (const uint32_t*)b;
    uint32_t differ = 0;
    for (unsigned i = 0; i < sizeof(gpu_state) / 4; i++) differ |= x[i] ^ y[i];
    return !differ;
}

unsigned gpuMemoryMark(void) { return texture_top; }
void gpuMemoryRelease(unsigned mark) {
    seglSettle();   // (the list being drawn may read what is given back and written again)
    texture_top = mark;
}

unsigned* gpuDrawAt(const gpu_state* s, unsigned vertices, int mesh) {
    uint32_t block = matrix_block();
    uint32_t* c = command(CMD_DRAW, (uint32_t)mesh);
    if (!c) return 0;
    c[1] = vertices;
    c[2] = (uint32_t)mesh;
    c[6] = block;
    command_state(c, s);
    passes_used |= (1u << (s->fragment >> 16 & 15)) & 0xfffe;
    last_draw = 0;
    joined_command = 0;
    return c;
}

unsigned* gpuDrawRoom(const gpu_state* s, int stored, int mesh) {
    if (vertex_top + (uint32_t)stored * 16 > GLES_VERTEX_END || mesh_vertices + (uint32_t)mesh > GLES_MESH_END) return 0;
    uint32_t* to = (uint32_t*)(gpu + vertex_top);
    uint32_t block = matrices_dirty || !blocks ? matrix_block() : GPU_PHYS + set_at + UNIFORMS_IN + 128 * (blocks - 1);
    if (joined_command && joined_command == (uint32_t*)(gpu + set_at) + 16 * (commands - 1) && block == joined_block &&
        joined_end == vertex_top && same_state(s, &joined)) {
        joined_command[2] += (uint32_t)mesh;
        mesh_vertices += (uint32_t)mesh;
    } else {
        uint32_t* c = command(CMD_DRAW, (uint32_t)mesh);
        if (!c) return 0;
        c[1] = GPU_PHYS + vertex_top;
        c[2] = (uint32_t)mesh;
        c[6] = block;
        command_state(c, s);
        passes_used |= (1u << (s->fragment >> 16 & 15)) & 0xfffe;
        joined = *s, joined_command = c, joined_block = block;
        last_draw = 0;
    }
    vertex_top += (uint32_t)stored * 16;
    joined_end = vertex_top;
    frame_last = 0xffffffffu;
    return to;
}

// Room for `count` draws of `mesh` vertices in all with the matrices as they are, for any core
// to write with gpuDrawPut before the frame is shown, `passes` the passes they draw in (a bit each).
unsigned* gpuDrawsRoom(int count, unsigned mesh, unsigned passes, unsigned* block, unsigned* first_mesh) {
    if (count <= 0 || commands + (unsigned)count > GLES_COMMANDS_END || mesh_vertices + mesh > GLES_MESH_END) return 0;
    *block = matrix_block();
    uint32_t* c = (uint32_t*)(gpu + set_at) + 16 * commands;
    for (int i = 0; i < count; i++) c[16 * i] = CMD_DRAW;   // (this core's store first: the pages are a worker's to write)
    *first_mesh = mesh_vertices;
    commands += (unsigned)count, mesh_vertices += mesh;
    passes_used |= passes & 0xfffe;
    last_draw = 0;
    joined_command = 0;
    return c;
}
// The block the draws made now read (written if the matrices changed), and the matrices marked
// changed, so that the next draw writes a block of its own after blocks taken for others to fill.
unsigned gpuMatricesBlock(void) { return matrix_block(); }
void gpuMatricesChanged(void) { matrices_dirty = 1; }
void gpuDrawPut(unsigned* c, const gpu_state* s, unsigned block, unsigned vertices, unsigned first_mesh, int mesh) {
    c[1] = vertices;
    c[2] = (uint32_t)mesh;
    c[3] = first_mesh;
    c[6] = block;
    command_state(c, s);
}

void* gpuFrameMemory(unsigned bytes) {
    bytes = (bytes + 15) & ~15u;
    if (!gpu || vertex_top + bytes > GLES_VERTEX_END) return 0;
    void* at = gpu + vertex_top;
    frame_last = vertex_top;
    vertex_top += bytes;
    joined_command = 0;   // (what follows is not next to the last command's vertices)
    return at;
}
// The frame's memory last given (by gpuFrameMemory, nothing taken since) cut to `bytes`: room taken for the most a
// renderer may write, kept for what it wrote (Jedi Outcast's tess: docs/games/jk2.md, "The arrays where they are made").
// 0: not the last given, or larger than it was.
int gpuFrameTrim(void* at, unsigned bytes) {
    if (!gpu || !at) return 0;
    const uint32_t off = (uint32_t)((uint8_t*)at - (uint8_t*)gpu);
    bytes = (bytes + 15) & ~15u;
    if (off != frame_last || off + bytes > vertex_top) return 0;
    vertex_top = off + bytes;
    return 1;
}
// Whether `bytes` at `p` lie in what this frame's memory has given (a draw may point at them until the frame is shown)
int gpuFrameHolds(const void* p, unsigned bytes) {
    if (!gpu || !p) return 0;
    const uint32_t off = (uint32_t)((const uint8_t*)p - (const uint8_t*)gpu);
    return off >= set_at + VERTICES_IN && off + bytes <= vertex_top;
}

int gpuKeep(int count, int mesh) {
    if (!gpu || count < 0 || count > MAX_COMMANDS - 64 || (uint32_t)mesh > MAX_MESH - 4096) return -1;
    seglSettle();
    kept = (uint32_t)count;
    kept_passes = passes_used = 0;
    kept_mesh = (uint32_t)mesh;
    commands = kept;
    mesh_vertices = kept_mesh;
    last_draw = 0;
    joined_command = 0;
    blocks = 0;
    return 0;
}

unsigned* gpuKept(int set, int index, const gpu_state* s, unsigned vertices, int first_mesh, int mesh) {
    uint32_t* c = kept_command(set, index);
    c[0] = CMD_DRAW;
    c[1] = vertices;
    c[2] = (uint32_t)mesh;
    c[3] = (uint32_t)first_mesh;
    c[6] = GPU_PHYS + SETS_AT + (uint32_t)set * SET_SIZE + UNIFORMS_IN;
    command_state(c, s);
    kept_passes |= (1u << (s->fragment >> 16 & 15)) & 0xfffe;
    passes_used |= kept_passes;
    return c;
}

unsigned* gpuKeptCommand(int index) { return kept_command(set_at != SETS_AT, index); }

// A sum over the list being built, its matrix blocks and vertices; places in its set and in the
// file at `rom` as offsets, which differ from one image to the next (QUAKE3_SUM).
static uint32_t sum_rom;
static uint32_t list_sum(uint32_t sum, const uint32_t* w, uint32_t words) {
    uint32_t low = GPU_PHYS + set_at, high = low + SET_SIZE;
    for (; words; words--, w++)
        sum = (sum ^ (*w - low < high - low ? *w - low + 0x40000000u : *w - sum_rom < 0x1000000u ? *w - sum_rom + 0x50000000u : *w)) * 0x01000193u;
    return sum;
}
unsigned gpuListSum(unsigned rom) {
    if (!gpu) return 0;
    sum_rom = rom;
    uint32_t sum = passes_used;
    // (only the words written: a clear's first two and its fourth, a block's 28)
    for (uint32_t i = 0; i < commands; i++) {
        const uint32_t* c = (const uint32_t*)(gpu + set_at) + 16 * i;
        sum = c[0] == CMD_DRAW ? list_sum(sum, c, 16) : list_sum(list_sum(sum, c, 2), c + 3, 1);
    }
    for (uint32_t i = 0; i < blocks; i++) sum = list_sum(sum, (const uint32_t*)(gpu + set_at + UNIFORMS_IN) + 32 * i, 28);
    return list_sum(sum, (const uint32_t*)(gpu + set_at + VERTICES_IN), (vertex_top - set_at - VERTICES_IN) / 4);
}

void gpuTouchSets(void (*touch)(void*, unsigned)) {
    if (gpu) touch(gpu + SETS_AT, 2 * SET_SIZE);
}

void gpuSwap(int wait) {
    joined_command = 0;
    swap(wait);
}

// The swap in two halves for a frame drawn on a worker core: gpuEnd by the core that drew it (the
// next frame goes to the other set), gpuShow by the first core later. e: set, commands, passes.
#ifdef SE_HALVES
void gpuUsedLast(unsigned* u) { for (int i = 0; i < 4; i++) u[i] = used_last[i]; }
#endif
void gpuEnd(unsigned* e) {
    joined_command = 0;
    e[0] = set_at, e[1] = commands, e[2] = passes_used;
    set_at = set_at == SETS_AT ? SETS_AT + SET_SIZE : SETS_AT;
    commands = kept;
    mesh_vertices = kept_mesh;
    blocks = 0;
    passes_used = kept_passes;
    last_draw = 0;
    vertex_top = set_at + VERTICES_IN;
    frame_last = 0xffffffffu;
    matrices_dirty = 1;
}

void gpuShow(const unsigned* e) {
    volatile uint32_t* regs = (volatile uint32_t*)gpu;
    if (regs[REG_BUFFERS / 4] != buffers_seen || swaps++ == 0) {
        buffers_seen = regs[REG_BUFFERS / 4];
        GrGetWindowInfo(window, &window_info);
    }
    if (!e[1] || !window_info.realized || !window_info.surface_address) return;
    for (;;) {
        while (__atomic_exchange_n(&regs[REG_LOCK / 4], 1, __ATOMIC_ACQUIRE)) sched_yield();
        if (regs[REG_SUBMIT / 4] == 0) break;
        __atomic_store_n(&regs[REG_LOCK / 4], 0, __ATOMIC_RELEASE);
        rest();
    }
    regs[REG_INTO / 4] = window_info.surface_address;
    regs[REG_INTO / 4 + 1] = window_info.width;
    regs[REG_INTO / 4 + 2] = window_info.height;
    regs[REG_INTO / 4 + 3] = window_info.surface_row;
    regs[REG_SUBMIT / 4 + 1] = GPU_PHYS + e[0];
    regs[REG_SUBMIT / 4 + 2] = e[1];
    regs[REG_SUBMIT / 4] = 1 | 4 | e[2] << 8;
    shown_set = e[0];
    shown_commands = e[1] | e[2] << 16;
    __atomic_store_n(&regs[REG_LOCK / 4], 0, __ATOMIC_RELEASE);
    unsettled = 1;
    unsettled_set = e[0];
    regs[REG_VOLUME / 4 + 1] = e[1];
    regs[REG_VOLUME / 4 + 3] = window_info.width | window_info.height << 16 | world_up << 28 | world_flat;
    regs[REG_VOLUME / 4] = GPU_PHYS + e[0];
    regs[REG_VOLUME / 4 + 2]++;
    fps_caption(&window_info, regs[0x34 / 4]);
}

// Whether the GPU has no list waiting to be drawn (on any core: a word it clears).
int gpuIdle(void) { return gpu && ((volatile uint32_t*)gpu)[REG_SUBMIT / 4] == 0; }

#ifdef SE_HALVES
// ---- a list built in two parts at once (linux/jk2/gl_jk2.c, "The drawing on two workers") ----
// The first part's builder (this file) splits what is left of the frame's set: commands, matrix blocks, vertex memory and
// the device's mesh, the upper half of each the second's; the second (this file built again with SE_DRAW_B, on its own
// worker) begins its part there with the window's size; when it is done its commands are moved to follow the first's and
// the first goes on after all the second took. A command says its own vertices, matrices and mesh: moved, it is the same.
typedef struct {
    uint8_t* gpu;
    uint32_t set_at, cmd_from, cmd_to, blocks_from, blocks_to, vertex_from, vertex_to, mesh_from, mesh_to, passes;
    GR_WINDOW_INFO window_info;
} gpu_half;
_Static_assert(sizeof(gpu_half) <= 256, "gpu_half: a caller keeps 256 bytes");
// (the first's: what the second part may take; 0 when too little is left to split)
int gpuHalfSplit(void* hp) {
    gpu_half* h = hp;
    if (!gpu || half_vertex_end) return 0;
    const uint32_t end = set_at + SET_SIZE;
    if (end - vertex_top < 64 * 1024 || MAX_COMMANDS - commands < 256 || MAX_BLOCKS - blocks < 32) return 0;
    h->gpu = gpu, h->set_at = set_at, h->window_info = window_info;
    h->cmd_from = commands + (MAX_COMMANDS - commands) / 2;
    h->blocks_from = blocks + (MAX_BLOCKS - blocks) / 2;
    h->vertex_from = (vertex_top + (end - vertex_top) / 2) & ~15u;
    h->mesh_from = mesh_vertices + (MAX_MESH - mesh_vertices) / 2;
    h->cmd_to = h->cmd_from, h->blocks_to = h->blocks_from, h->vertex_to = h->vertex_from, h->mesh_to = h->mesh_from, h->passes = 0;
    half_commands_end = h->cmd_from, half_blocks_end = h->blocks_from, half_vertex_end = h->vertex_from, half_mesh_end = h->mesh_from;
    return 1;
}
// (the second's, on its own worker: its part begins where the first left it room)
void gpuHalfBegin(const void* hp) {
    const gpu_half* h = hp;
    gpu = h->gpu, set_at = h->set_at, window_info = h->window_info;
    commands = h->cmd_from, blocks = h->blocks_from, vertex_top = h->vertex_from, mesh_vertices = h->mesh_from;
    half_commands_end = MAX_COMMANDS, half_blocks_end = MAX_BLOCKS, half_vertex_end = 0, half_mesh_end = MAX_MESH;
    kept = 0, kept_mesh = 0, passes_used = 0, last_draw = 0, joined_command = 0, matrices_dirty = 1, frame_last = 0xffffffffu;
}
// (the second's, when its part is done: how far it went)
void gpuHalfEnd(void* hp) {
    gpu_half* h = hp;
    h->cmd_to = commands, h->blocks_to = blocks, h->vertex_to = vertex_top, h->mesh_to = mesh_vertices, h->passes = passes_used;
}
// (the first's, the second done: its commands after the first's, and the first going on after all it took)
void gpuHalfMerge(const void* hp) {
    const gpu_half* h = hp;
    uint32_t* list = (uint32_t*)(gpu + set_at);
    const uint32_t n = h->cmd_to - h->cmd_from;
    if (n) memmove(list + 16 * commands, list + 16 * h->cmd_from, (size_t)n * 64);
    commands += n;
    passes_used |= h->passes;
    if (h->blocks_to > blocks) blocks = h->blocks_to;
    if (h->vertex_to > vertex_top) vertex_top = h->vertex_to;
    if (h->mesh_to > mesh_vertices) mesh_vertices = h->mesh_to;
    half_commands_end = MAX_COMMANDS, half_blocks_end = MAX_BLOCKS, half_vertex_end = 0, half_mesh_end = MAX_MESH;
    last_draw = 0, joined_command = 0, matrices_dirty = 1, frame_last = 0xffffffffu;
    used_last[1] = blocks, used_last[2] = (vertex_top - set_at - VERTICES_IN) >> 10, used_last[3] = commands;
}
#endif
