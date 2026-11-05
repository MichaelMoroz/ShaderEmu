// The machine's GPU for Quake III Arena (docs/games/quake3.md): programs/linux/gles.c as it is (its
// window, its frames' two sets, its matrices, its swap), and after it draws whose every word
// the renderer says itself: its textures are in the ROM, which the library's own have no way
// to say, and a draw may be lit by a light map (docs/machine/gpu.md).

// A frame of this game is more vertices than the library's default has room for.
#define SET_SIZE 0x00080000u
#define UNIFORMS_IN 0x00030000u
#define VERTICES_IN 0x00038000u
#define MAX_COMMANDS 3072
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
    if (matrices_dirty || !blocks || blocks >= MAX_BLOCKS) {
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
    if (matrices_dirty || !blocks || blocks >= MAX_BLOCKS) return 0;
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
    if (vertex_top + (uint32_t)stored * 16 > set_at + SET_SIZE || mesh_vertices + (uint32_t)mesh > MAX_MESH) return 0;
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
    return to;
}

// Room for `count` draws of `mesh` vertices in all with the matrices as they are, for any core
// to write with gpuDrawPut before the frame is shown, `passes` the passes they draw in (a bit each).
unsigned* gpuDrawsRoom(int count, unsigned mesh, unsigned passes, unsigned* block, unsigned* first_mesh) {
    if (count <= 0 || commands + (unsigned)count > MAX_COMMANDS || mesh_vertices + mesh > MAX_MESH) return 0;
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
    if (!gpu || vertex_top + bytes > set_at + SET_SIZE) return 0;
    void* at = gpu + vertex_top;
    vertex_top += bytes;
    joined_command = 0;   // (what follows is not next to the last command's vertices)
    return at;
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
    // (only the words written: a clear's first two and its fourth, a block's 28; a texture or light map in the VRAM as
    // what it was uploaded from, and the uploads not at all: when a picture takes its slot is not what was drawn)
    for (uint32_t i = 0; i < commands; i++) {
        const uint32_t* c = (const uint32_t*)(gpu + set_at) + 16 * i;
        if (c[0] == CMD_UPLOAD) continue;
        if (c[0] == CMD_DRAW) {
            uint32_t w[16];
            for (int k = 0; k < 16; k++) w[k] = c[k];
            if (w[7] && w[7] >> 30 == 0) w[7] = vram_source_of(w[7]);
            if ((w[5] & 0x800) && w[13] && w[13] >> 30 == 0) w[13] = vram_source_of(w[13]);
            sum = list_sum(sum, w, 16);
        } else {
            sum = list_sum(list_sum(sum, c, 2), c + 3, 1);
        }
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
void gpuEnd(unsigned* e) {
    joined_command = 0;
    // (the VRAM's uploads waiting go into this list: bit 0 of the passes, which is no pass, says one has levels)
    uint32_t levels = commands ? uploads_put() : 0;
    uploads_done();
    e[0] = set_at, e[1] = commands, e[2] = passes_used | (levels ? 1u : 0u);
    set_at = set_at == SETS_AT ? SETS_AT + SET_SIZE : SETS_AT;
    commands = kept;
    mesh_vertices = kept_mesh;
    blocks = 0;
    passes_used = kept_passes;
    last_draw = 0;
    vertex_top = set_at + VERTICES_IN;
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
    regs[REG_SUBMIT / 4] = 1 | 4 | (e[2] & 1u ? 8u : 0u) | (e[2] & ~1u) << 8;   // (SUBMIT_UPLOADS when its uploads have levels)
    shown_set = e[0];
    shown_commands = e[1] | (e[2] & ~1u) << 16;
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
