// The OpenGL 1.1 OpenJK's renderer (code/rd-vanilla) calls, on the machine's GPU (docs/games/jk2.md, "The picture"):
// Half-Life's (linux/halflife/gl_shaderemu.c, included as it is) over Quake III's draws (linux/quake3/gpu_shaderemu.c).
// build.sh renames the renderer's calls glX to jk2_glX; here each goes to that file's s_glX once the window is open
// (jk2glOpen), and before that, or with no window at all (a run with no Nano-X server: the sums), does what nullgl.c
// does, so that the game is the same with a picture or without.
#include "../halflife/gl_shaderemu.c"
#include "mcwx.h"

// what gl_shaderemu.c asks of Half-Life's drawing on a worker, which this game does not have
int SE_RenderWho(void) { return 0; }
int SE_RenderOut(void) { return 0; }
uint32_t SE_RenderOnCore0(uint32_t (*fn)(void*), void* args) { return fn(args); }
// (se_asks, which gl_shaderemu.c names in a block of its own, is defined in sys_shaderemu.cpp)

static int on;   // the window is open and the GPU draws
int jk2glOn(void) { return on; }

// ---- the drawing on a worker, a frame behind (docs/games/jk2.md, "The drawing on a worker") ----
// At a frame's end (R_IssueRenderCommands) rd-vanilla's commands are given to a worker of their own, which runs its
// back end (RB_ExecuteRenderCommands: the shaders' stages, Ghoul2's skinning, this file's GL) on a stack of its own
// while this core makes the next frame into the other backEndData. The frame is shown (the GPU's list swapped) by this
// core when the job is done, at the next give or any settle. Any GL call this core makes while the job is out (a
// texture uploaded, a sync of the renderer's) settles it first. JK2_RENDER=core: the drawing this core's, as before.
uint32_t jk2_call_on_stack(uint32_t (*fn)(uint32_t), uint32_t arg, void* stack_top);
__asm__(".text\n.globl jk2_call_on_stack\n.type jk2_call_on_stack, @function\njk2_call_on_stack:\n"
        "	addi a2, a2, -16\n	sw ra, 12(a2)\n	sw sp, 8(a2)\n	mv sp, a2\n	mv t0, a0\n	mv a0, a1\n	jalr t0\n"
        "	lw ra, 12(sp)\n	lw sp, 8(sp)\n	ret\n");
#define RENDER_STACK (256 * 1024)
static uint8_t render_stack[RENDER_STACK] __attribute__((aligned(16)));
static struct __attribute__((aligned(16))) { int mode, worker, out, presenting; unsigned gives, settles, gl_settles, waits; } rw0;  // this core's
static struct __attribute__((aligned(16))) { void (*fn)(const void*); const void* arg; int swap, unused; } job;  // the given frame: the worker's while out
static struct __attribute__((aligned(16))) { volatile unsigned go, unused[3]; } job_go;  // this core's: the frame may begin
static void gl_swap(void);
static void memo_init(void);
static int on_render(void) {
    char here;
    return (uint8_t*)&here >= render_stack && (uint8_t*)&here < render_stack + RENDER_STACK;
}
int jk2RenderWorkerHere(void) { return on_render(); }
static uint32_t render_body(uint32_t given) {
    while (job_go.go != given) mc_end_pass();   // (until this core has given the frame's skins out: SE_SkinGive)
    job.fn(job.arg);
    return 0;
}
static uint32_t render_job(uint32_t a0, uint32_t a1) {
    (void)a1;
    return jk2_call_on_stack(render_body, a0, render_stack + RENDER_STACK);
}
extern char _start[], etext[];
static void render_open(void) {
    const char* how = getenv("JK2_RENDER");
    rw0.mode = 0;
    if (how && !strcmp(how, "core")) return;
    int n = mcw_open(getenv("JK2_WORKERS") ? atoi(getenv("JK2_WORKERS")) : 32);   // (as sys_shaderemu.cpp's: the first to open decides)
    if (n <= 0) return;
    mcw_touch_code(_start, (unsigned)(etext - _start));   // (a worker stops at a page of code no core has run)
    mcw_touch(render_stack, RENDER_STACK);
    mcw_touch(&job, sizeof job);
    // (a worker on one of the first 16 cores, with the larger write cache: the drawing stores every vertex's words)
    rw0.worker = n;
    for (int k = 1; k <= n; k++)
        if (mcw_fast(k)) { rw0.worker = k; break; }
    rw0.mode = 1;
    printf("jk2: the drawing on worker %d (%s write cache), a frame behind\n", rw0.worker, mcw_fast(rw0.worker) ? "a large" : "a small");
}
// (in play only: a menu's few draws of many vertices are faster on this core, whose write cache is the larger)
int SE_InPlay(void);
int jk2RenderOn(void) { return on && rw0.mode == 1 && SE_InPlay(); }
// The frame out done and shown
void jk2RenderSettle(void) {
    if (!rw0.out || on_render()) return;
    while (!mcw_done(rw0.worker)) mcw_rest_for(&rw0.worker, 1), rw0.waits++;
    rw0.out = 0, rw0.settles++;
    if (job.swap) job.swap = 0, gl_swap();
}
// (a GL call of this core's while the frame is out: the drawing's state is the worker's until it is done)
#define SETTLE() do { if (rw0.out && !on_render()) rw0.gl_settles++, jk2RenderSettle(); } while (0)
void jk2RenderGive(void (*fn)(const void*), const void* arg) {
    jk2RenderSettle();
    job.fn = fn, job.arg = arg, job.swap = 0;
    mcw_post(rw0.worker, render_job, rw0.gives + 1, 0);
    rw0.out = 1, rw0.gives++;
}
// ---- the mix painted on a worker of its own while the next frame's game runs (sys_shaderemu.cpp's SE_MixGive;
// docs/games/jk2.md, "The mix on a worker") ----
#define MIX_STACK (128 * 1024)
static uint8_t mix_stack[MIX_STACK] __attribute__((aligned(16)));
static struct __attribute__((aligned(16))) { int worker, out, unused[2]; } mw0;   // this core's
static struct __attribute__((aligned(16))) { void (*fn)(void); int unused[3]; } mix_job;   // the worker's while out
static uint32_t mix_body(uint32_t unused) {
    (void)unused;
    mix_job.fn();
    return 0;
}
static uint32_t mix_post(uint32_t a0, uint32_t a1) {
    (void)a0, (void)a1;
    return jk2_call_on_stack(mix_body, 0, mix_stack + MIX_STACK);
}
int jk2MixOn(void) {
    if (!mw0.worker && rw0.mode == 1) {   // (a worker of the larger caches other than the drawing's)
        const char* how = getenv("JK2_MIX");
        mw0.worker = -1;
        if (!(how && !strcmp(how, "core"))) {
            for (int k = 1; k <= mcw_count(); k++)
                if (k != rw0.worker && mcw_fast(k)) { mw0.worker = k; break; }
            if (mw0.worker > 0) mcw_touch(mix_stack, MIX_STACK), mcw_touch(&mix_job, sizeof mix_job);
        }
    }
    return on && mw0.worker > 0;
}
void jk2MixSettle(void) {
    if (!mw0.out) return;
    while (!mcw_done(mw0.worker)) mcw_rest_for(&mw0.worker, 1);
    mw0.out = 0;
}
void jk2MixGive(void (*fn)(void)) {
    jk2MixSettle();
    mix_job.fn = fn;
    mcw_post(mw0.worker, mix_post, 0, 0);
    mw0.out = 1;
}

// The given frame may begin (posted first, so that no helper's job takes its worker)
void jk2RenderGo(void) { job_go.go = rw0.gives; }

// ---- Ghoul2's skinning on helper workers (tr_ghoul2.cpp, SE_SkinGive): the frame's surfaces in parts ----
static mcw_handle skin_jobs[16];
static int skin_count;
int jk2SkinPost(uint32_t (*fn)(uint32_t, uint32_t), uint32_t a0, uint32_t a1) {
    if (skin_count >= 16) return 0;
    mcw_handle h = mcw_try_call(fn, a0, a1);
    if (h) skin_jobs[skin_count++] = h;
    return h != 0;
}
void jk2SkinJoin(void) {
    for (int i = 0; i < skin_count; i++) mcw_join(skin_jobs[i]);
    skin_count = 0;
}
void jk2Touch(void* memory, unsigned bytes) { mcw_touch(memory, bytes); }
void jk2WaitPass(void) { mc_end_pass(); }
// The worker's RB_SwapBuffers: whether the swap is left to this core (the worker draws); else the window's own
int jk2RenderSwapMark(void) {
    if (!on_render()) return 0;
    job.swap = 1;
    return 1;
}
// The window's frame (its input, keys and counts) once a frame is given, its GPU list shown at the settle
void SE_WindowFrame(void);
void jk2RenderPresent(void) {
    rw0.presenting = 1;
    SE_WindowFrame();
    rw0.presenting = 0;
}
// The worker's state served while this core waits elsewhere (a page or a system call it stopped at)
void jk2RenderServe(void) {
    if (rw0.out) mcw_done(rw0.worker);
}
void jk2RenderStats(unsigned* gives, unsigned* settles, unsigned* gl_settles, unsigned* waits) {
    *gives = rw0.gives, *settles = rw0.settles, *gl_settles = rw0.gl_settles, *waits = rw0.waits;
    rw0.gives = rw0.settles = rw0.gl_settles = rw0.waits = 0;
}

int jk2glOpen(unsigned window) {
    if (on) return 0;
    if (!getenv("HLGL_WORKERS")) setenv("HLGL_WORKERS", "0", 0);   // (none yet: textures are converted by this core)
    if (!getenv("HL_POOL_MB")) setenv("HL_POOL_MB", "36", 0);   // (the demo's level and menus at r_picmip 2: 31.6 MB)
    if (hlglOpen(window) < 0) return -1;
    on = 1;
    render_open();
    memo_init();
    return 0;
}
static int depth_seen;   // a draw of this frame was depth-tested (fast_triangles: what is drawn before it is behind it)
static int gl_frames, gl_log_at = -1;
static void gl_swap(void) {
    hlglSwap();
    depth_seen = 0;
    if (gl_log_at < 0) gl_log_at = getenv("JK2_GL_LOG") ? atoi(getenv("JK2_GL_LOG")) : 0;
    gl_frames++;
}
void jk2glSwap(void) {
    if (!on || rw0.presenting) return;   // (a frame given to the worker: shown when it is done)
    SETTLE();
    gl_swap();
}
void jk2glStats(unsigned* in_gpu, unsigned* gpu_size, unsigned* left_out, unsigned* vertices) {
    unsigned tiles;
    jk2RenderSettle();   // (the counts are the drawing's: RVC_RACES found this core clearing one the worker added to)
    if (on) hlglStats(&tiles, in_gpu, gpu_size, left_out, vertices);
    else *in_gpu = *gpu_size = *left_out = *vertices = 0;
}

typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef unsigned char GLubyte;

#define ON_THEN(call) do { if (on) { SETTLE(); call; } } while (0)
void jk2_glAlphaFunc(GLenum f, GLclampf r) { (void)f, (void)r; }
void jk2_glArrayElement(GLint i) { ON_THEN(s_glArrayElement(i)); }
void jk2_glBegin(GLenum m) { ON_THEN(s_glBegin(m)); }
void jk2_glEnd(void) { ON_THEN(s_glEnd()); }
void jk2_glBindTexture(GLenum t, GLuint n) { ON_THEN(s_glBindTexture(t, n)); }
void jk2_glBlendFunc(GLenum s, GLenum d) { ON_THEN(s_glBlendFunc(s, d)); }
void jk2_glCallList(GLuint l) { (void)l; }
void jk2_glClear(GLbitfield m) { ON_THEN(s_glClear(m)); }
void jk2_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { ON_THEN(s_glClearColor(r, g, b, a)); }
void jk2_glClearDepth(GLclampd d) { (void)d; }
void jk2_glClearStencil(GLint s) { (void)s; }
void jk2_glClipPlane(GLenum p, const GLdouble* e) { (void)p, (void)e; }
void jk2_glColor3f(GLfloat r, GLfloat g, GLfloat b) { ON_THEN(s_glColor3f(r, g, b)); }
void jk2_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { ON_THEN(s_glColor4f(r, g, b, a)); }
void jk2_glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { ON_THEN(s_glColor4ub(r, g, b, a)); }
void jk2_glColor4ubv(const GLubyte* v) { ON_THEN(s_glColor4ubv(v)); }
void jk2_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { (void)r, (void)g, (void)b, (void)a; }
void jk2_glColorPointer(GLint size, GLenum type, GLsizei stride, const void* p) { ON_THEN(s_glColorPointer(size, type, stride, p)); }
void jk2_glCopyTexImage2D(GLenum t, GLint l, GLenum f, GLint x, GLint y, GLsizei w, GLsizei h, GLint b) {}
void jk2_glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {}
void jk2_glCullFace(GLenum m) { (void)m; }
void jk2_glDeleteLists(GLuint l, GLsizei r) { (void)l, (void)r; }
void jk2_glDeleteTextures(GLsizei n, const GLuint* t) { ON_THEN(s_glDeleteTextures(n, t)); }
void jk2_glDepthFunc(GLenum f) { (void)f; }
void jk2_glDepthMask(GLboolean f) { (void)f; }
void jk2_glDepthRange(GLclampd n, GLclampd f) { (void)n, (void)f; }
void jk2_glDisable(GLenum c) { ON_THEN(s_glDisable(c)); }
void jk2_glEnable(GLenum c) { ON_THEN(s_glEnable(c)); }
void jk2_glDisableClientState(GLenum a) { ON_THEN(s_glDisableClientState(a)); }
void jk2_glEnableClientState(GLenum a) { ON_THEN(s_glEnableClientState(a)); }
void jk2_glDrawArrays(GLenum m, GLint first, GLsizei count) { ON_THEN(s_glDrawArrays(m, first, count)); }
void jk2_glDrawBuffer(GLenum m) { (void)m; }
// (gl_shaderemu.c's primitive holds MAX_PRIMITIVE vertices: the renderer's triangles in pieces of whole triangles)
// rd-vanilla's triangles (docs/games/jk2.md, "Fewer instructions"): its arrays as it always sets them (positions of
// floats, two float coordinates, colours of four bytes) and 32-bit indices, drawn as draw_vertices() draws them (the
// same words), but each vertex made once, over the range of vertices the indices name, and an index then a copy of
// its vertex's four words: about 20 instructions an index of a model against 165, where every index copied a vertex
// twice on the way. The coordinates' least and most and whether all have one colour are over that range (rd-vanilla's
// arrays hold the surfaces' vertices, all of them named). Anything else, and a pulled offset, goes the plain way.
// JK2_GL=plain: always.
static int fast_off = -1, fast_passes;
static uint32_t made[MAX_PRIMITIVE * 4] __attribute__((aligned(16)));   // a vertex's words, from the range's first
static int fast_ok(void) {
    if (fast_off < 0) {
        const char* g = getenv("JK2_GL");
        fast_off = g && !strcmp(g, "plain");
        fast_passes = g && !strcmp(g, "passes");
    }
    return !fast_off && va_pos.on && va_pos.type == GL_FLOAT && va_pos.size >= 3 && va_coord.on && va_coord.type == GL_FLOAT &&
        va_coord.size >= 2 && va_colour.on && va_colour.type == GL_UNSIGNED_BYTE && va_colour.size == 4 && !(offsetting && offset_pull != 0);
}
// A large draw that is as it was (docs/games/jk2.md, "Fewer instructions"): the menu's turning logo is 4,320 indices of
// 1,399 vertices whose first stage (its colour, its own coordinates) is the same every frame, the model turning by the
// matrix. Its GPU words are kept in GPU memory of their own (hlglKeptMemory), with what they were made of, and a draw of
// the same state, indices, vertices, coordinates and colours is drawn from there: a comparison of its inputs, about a
// third of making it again, and none of its 69 KB stored. Draws of 600 indices and more, the last 8. JK2_GL=nomemo: not.
#define MEMO_SLOTS 8
#define MEMO_LEAST 600
typedef struct { int count, n, far, frame; uint32_t keep, colour; unsigned used; gpu_state s; uint32_t *in, *idx; } memo_t;
static memo_t memo[MEMO_SLOTS];
static uint8_t* memo_gpu;
static int memo_state = -1;   // -1 not yet asked, 0 off, 1 on
static unsigned memo_clock;
// (*shape: a kept draw of the same state and size whose words differ: one that changes every frame, as the logo's
// second stage, which takes its coordinates from the eye)
static memo_t* memo_find(const gpu_state* s, int count, int n, int far, uint32_t keep, const uint8_t* pos, int ps,
                         const uint8_t* tc, int ts, const uint8_t* col, int cs, const uint32_t* idx, uint32_t lo, memo_t** shape) {
    *shape = 0;
    for (int e = 0; e < MEMO_SLOTS; e++) {
        memo_t* m = &memo[e];
        if (!m->used || m->count != count || m->n != n || m->far != far || m->keep != keep || memcmp(&m->s, s, sizeof *s)) continue;
        *shape = m;
        const uint32_t* in = m->in;
        int k = 0;
        for (; k < n; k++, in += 6) {
            const uint32_t* p = (const uint32_t*)(pos + (size_t)k * ps);
            const uint32_t* t = (const uint32_t*)(tc + (size_t)k * ts);
            if (p[0] != in[0] || p[1] != in[1] || p[2] != in[2] || t[0] != in[3] || t[1] != in[4] ||
                *(const uint32_t*)(col + (size_t)k * cs) != in[5]) break;
        }
        if (k < n) continue;
        int i = 0;
        for (; i < count && idx[i] - lo == m->idx[i]; i++) {
        }
        if (i == count) return m;
    }
    return 0;
}
// The slot a new large draw is kept in: the one used longest ago
// (made on this core when the window opens: the drawing may be on a worker, which may not malloc)
static void memo_init(void) {
    const char* g = getenv("JK2_GL");
    memo_state = 0;
    if (!(g && !strcmp(g, "nomemo")) && (memo_gpu = hlglKeptMemory(MEMO_SLOTS * MAX_PRIMITIVE * 16))) {
        memo_state = 1;
        for (int e = 0; e < MEMO_SLOTS && memo_state; e++)
            if (!(memo[e].in = malloc(MAX_PRIMITIVE * 6 * 4)) || !(memo[e].idx = malloc(MAX_PRIMITIVE * 4))) memo_state = 0;
        for (int e = 0; e < MEMO_SLOTS && memo_state; e++)
            mcw_touch(memo[e].in, MAX_PRIMITIVE * 6 * 4), mcw_touch(memo[e].idx, MAX_PRIMITIVE * 4);
        mcw_touch(memo_gpu, MEMO_SLOTS * MAX_PRIMITIVE * 16);
    }
}
static memo_t* memo_slot(void) {
    if (memo_state <= 0) return 0;
    memo_t* best = &memo[0];
    for (int e = 1; e < MEMO_SLOTS; e++)
        if (memo[e].used < best->used) best = &memo[e];
    return best;
}

// (the colour as the plain way makes it of four bytes: alpha's complement on top)
#define WORD_OF(c) ((255u - ((c) >> 24)) << 24 | ((c) & 0xff) << 16 | ((c) & 0xff00) | ((c) >> 16 & 0xff))
static int fast_triangles(int count, const uint32_t* idx) {
    count -= count % 3;
    if (count < 3) return 1;
    uint32_t lo = idx[0], hi = idx[0];
    for (int i = 1; i < count; i++) {
        uint32_t k = idx[i];
        if (k < lo) lo = k;
        if (k > hi) hi = k;
    }
    int n = (int)(hi - lo) + 1;
    if (n > MAX_PRIMITIVE) return 0;
    gpu_state s;
    if (!draw_state(&s)) {
        if (gl_log_at > 0 && gl_log_at == gl_frames) printf("jk2gl: draw %d indices skipped: texture %d not in GPU memory\n", count, (int)bound);
        return 1;
    }
    // Two things the GPU's passes do otherwise than OpenGL (docs/games/jk2.md, "What the menu was missing"):
    // - one depth buffer a frame, and a pass draws after every pass before it whatever the list's order: rd-vanilla's 2D
    //   (z 0, the near plane, mostly depth-tested) covered the 3D a menu draws after it (the turning logo: OpenGL
    //   clears the depth for a scene), which the shim cannot clear. So 2D before the frame's first 3D draw goes to the
    //   depth-tested passes at the far plane (z made -1 under rd-vanilla's ortho of 0 to 1): what comes later in 3D is
    //   in front of it; and 2D after it to the passes without depth: on top, as in the list. JK2_GL=passes: not.
    // - "added" scales by alpha, which GL_ONE, GL_ONE does not: the menu's saber halos, whose alpha is a wave from -1
    //   to 1, were drawn at their alpha, so not at all half the time. Their alpha is made 1 here.
    int far = 0;
    uint32_t keep = 0xffffffffu;
    if (blending && blend_src == GL_ONE && blend_dst == GL_ONE) keep = 0x00ffffffu;
    if (!fast_passes) {
        float m[16];
        seglGetMatrix(GL_PROJECTION, m);	// (columns: rd-vanilla's 2D is an ortho of depth 0 to 1)
        unsigned pass = s.fragment >> 16 & 7;
        if (!(m[10] == -2.0f && m[14] == -1.0f && m[11] == 0.0f && m[15] == 1.0f)) {
            depth_seen = 1;
        } else if (!depth_seen) {
            far = 1;   // 2D before the frame's 3D: behind it
            if (pass >= 4) s.fragment -= GPU_PASS(4);
        } else if (pass < 4) {
            s.fragment += GPU_PASS(pass ? 4 : 5);   // 2D after it: on top (OpenGL's 3D had its own depth cleared)
        }
    }
    const uint32_t far_z = bits(-0.9999f);
    if (gl_log_at > 0 && gl_log_at == gl_frames) {   // JK2_GL_LOG=F: frame F's draws, one a line
        float m[16];
        const float* p0 = (const float*)(va_pos.p + (size_t)idx[0] * va_pos.stride);
        seglGetMatrix(GL_PROJECTION, m);
        printf("jk2gl: draw %d indices %d vertices, texture %d, pass %u, depth %d, blend %x %x, far %d, colour %08x, "
               "first %.1f %.1f %.1f, projection %.3f %.3f %.3f %.3f\n", count, n, texturing ? (int)bound : -1,
               s.fragment >> 16 & 7, depth_test, blending ? blend_src : 0, blending ? blend_dst : 0, far,
               *(const uint32_t*)(va_colour.p + (size_t)idx[0] * va_colour.stride), p0[0], p0[1], p0[2], m[0], m[5], m[10], m[14]);
    }
    const uint8_t *pos = va_pos.p + (size_t)lo * va_pos.stride, *tc = va_coord.p + (size_t)lo * va_coord.stride,
                  *col = va_colour.p + (size_t)lo * va_colour.stride;
    int ps = va_pos.stride, ts = va_coord.stride, cs = va_colour.stride;
    memo_t *kept = 0, *shape;
    if (count >= MEMO_LEAST && memo_state != 0 && !replace) {
        if ((kept = memo_find(&s, count, n, far, keep, pos, ps, tc, ts, col, cs, idx, lo, &shape))) {
            gpu_state ks = s;
            ks.colour = kept->colour;
            gpuDrawAt(&ks, gpuAddress(memo_gpu + (size_t)(kept - memo) * MAX_PRIMITIVE * 16), count);
            kept->used = ++memo_clock, kept->frame = gl_frames;
            drawn_vertices += (uint32_t)count;
            if (gl_log_at > 0 && gl_log_at == gl_frames) printf("jk2gl: draw %d indices %d vertices kept, slot %d\n", count, n, (int)(kept - memo));
            return 1;
        }
        if (shape && shape->frame >= gl_frames - 1 && (gl_frames & 15)) {
            shape->frame = gl_frames;   // (it changes from frame to frame: kept as it is, and tried again every 16th frame)
            kept = 0;
        } else {
            kept = shape ? shape : memo_slot();
            if (kept && kept->used && kept->frame >= gl_frames - 1) seglSettle();   // (the list being drawn may read it)
        }
        if (gl_log_at > 0 && gl_log_at == gl_frames) printf("jk2gl: draw %d indices %d vertices not kept (memo %d), slot %d\n", count, n, memo_state, kept ? (int)(kept - memo) : -1);
    }
    gpu_state s0 = s;   // (the state as the memo compares it, before its colour)
    int textured = (s.fragment & 0xff) != GPU_COLOUR;
    float low_s = 0, low_t = 0, high_s = 0, high_t = 0;
    uint32_t first = *(const uint32_t*)col;
    int one = 1;
    if (textured) {
        const float* t0 = (const float*)tc;
        low_s = high_s = t0[0], low_t = high_t = t0[1];
        for (int k = 1; k < n; k++) {
            const float* t = (const float*)(tc + (size_t)k * ts);
            float ss = t[0], tt = t[1];
            if (ss < low_s) low_s = ss;
            if (ss > high_s) high_s = ss;
            if (tt < low_t) low_t = tt;
            if (tt > high_t) high_t = tt;
            one &= *(const uint32_t*)(col + (size_t)k * cs) == first;
        }
        if (!(s.fragment & GPU_CLAMPED)) low_s = floor_of(low_s), low_t = floor_of(low_t);
        else low_s = low_t = 0;
    } else {
        for (int k = 1; k < n && one; k++) one = *(const uint32_t*)(col + (size_t)k * cs) == first;
    }
    first = WORD_OF(first) & keep;
    if (replace && textured) one = 1, first = 0x00ffffff;
    uint32_t* table = 0;
    if (one || high_s - low_s >= 4 || high_t - low_t >= 4) {
        s.colour = first;
        for (int k = 0; k < n; k++) {
            const float* p = (const float*)(pos + (size_t)k * ps);
            const float* t = (const float*)(tc + (size_t)k * ts);
            uint32_t* m = &made[k * 4];
            m[0] = bits(p[0]), m[1] = bits(p[1]), m[2] = far ? far_z : bits(p[2]);
            m[3] = ((uint32_t)(int32_t)((t[0] - low_s) * 1024.0f) & 0xffff) | (uint32_t)(int32_t)((t[1] - low_t) * 1024.0f) << 16;
        }
    } else if (n <= 256) {
        // a vertex's colour is the table's word for its tag, which is its place in the range
        s.vertex = GPU_TAGGED | GPU_TABLE;
        if (!(table = gpuFrameMemory(256 * 4))) return 1;
        s.colour = gpuAddress(table);
        for (int k = 0; k < n; k++) {
            uint32_t c = *(const uint32_t*)(col + (size_t)k * cs);
            const float* p = (const float*)(pos + (size_t)k * ps);
            const float* t = (const float*)(tc + (size_t)k * ts);
            uint32_t* m = &made[k * 4];
            table[k] = WORD_OF(c) & keep;
            m[0] = bits(p[0]), m[1] = bits(p[1]), m[2] = far ? far_z : bits(p[2]);
            m[3] = ((uint32_t)((t[0] - low_s) * 1024.0f) & 0xfff) | ((uint32_t)((t[1] - low_t) * 1024.0f) & 0xfff) << 12 | (uint32_t)k << 24;
        }
    } else {
        // more vertices than a table has colours: a table each 255 indices, as the plain way
        s.vertex = GPU_TAGGED | GPU_TABLE;
        for (int done = 0; done < count;) {
            int c = count - done > 255 ? 255 : count - done;
            if (!(table = gpuFrameMemory(256 * 4))) return 1;
            s.colour = gpuAddress(table);
            uint32_t* to = gpuDrawRoom(&s, c, c);
            if (!to) return 1;
            for (int i = 0; i < c; i++, to += 4) {
                uint32_t k = idx[done + i] - lo, w = *(const uint32_t*)(col + (size_t)k * cs);
                const float* p = (const float*)(pos + (size_t)k * ps);
                const float* t = (const float*)(tc + (size_t)k * ts);
                table[i] = WORD_OF(w) & keep;
                to[0] = bits(p[0]), to[1] = bits(p[1]), to[2] = far ? far_z : bits(p[2]);
                to[3] = ((uint32_t)((t[0] - low_s) * 1024.0f) & 0xfff) | ((uint32_t)((t[1] - low_t) * 1024.0f) & 0xfff) << 12 | (uint32_t)i << 24;
            }
            done += c;
        }
        drawn_vertices += (uint32_t)count;
        return 1;
    }
    uint32_t* to;
    if (kept && !table) {
        to = (uint32_t*)(memo_gpu + (size_t)(kept - memo) * MAX_PRIMITIVE * 16);
        uint32_t* in = kept->in;
        for (int k = 0; k < n; k++, in += 6) {
            const uint32_t* p = (const uint32_t*)(pos + (size_t)k * ps);
            const uint32_t* t = (const uint32_t*)(tc + (size_t)k * ts);
            in[0] = p[0], in[1] = p[1], in[2] = p[2], in[3] = t[0], in[4] = t[1], in[5] = *(const uint32_t*)(col + (size_t)k * cs);
        }
        for (int i = 0; i < count; i++) kept->idx[i] = idx[i] - lo;
        kept->s = s0, kept->count = count, kept->n = n, kept->far = far, kept->keep = keep, kept->colour = s.colour;
        kept->used = ++memo_clock, kept->frame = gl_frames;
        gpuDrawAt(&s, gpuAddress(to), count);
    } else if (!(to = gpuDrawRoom(&s, count, count))) {
        return 1;
    }
    for (int i = 0; i < count; i++, to += 4) {
        const uint32_t* m = &made[(idx[i] - lo) * 4];
        uint32_t a = m[0], b = m[1], c = m[2], d = m[3];
        to[0] = a, to[1] = b, to[2] = c, to[3] = d;
    }
    drawn_vertices += (uint32_t)count;
    return 1;
}
#undef WORD_OF

void jk2_glDrawElements(GLenum m, GLsizei count, GLenum type, const void* indices) {
    if (!on) return;
    SETTLE();
    if (m == GL_TRIANGLES && type == GL_UNSIGNED_INT && fast_ok()) {
        int piece = MAX_PRIMITIVE / 3 * 3;
        int done = 0;
        for (; done < count; done += piece)
            if (!fast_triangles(count - done < piece ? count - done : piece, (const uint32_t*)indices + done)) break;
        if (done >= count) return;
        // (a range of vertices longer than a primitive: the rest the plain way)
        indices = (const uint32_t*)indices + done, count -= done;
    }
    if (m != GL_TRIANGLES || count <= MAX_PRIMITIVE) {
        s_glDrawElements(m, count, type, indices);
        return;
    }
    int size = type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 1, piece = MAX_PRIMITIVE / 3 * 3;
    for (int done = 0; done < count; done += piece)
        s_glDrawElements(m, count - done < piece ? count - done : piece, type, (const char*)indices + (size_t)done * size);
}
void jk2_glEndList(void) {}
void jk2_glNewList(GLuint l, GLenum m) { (void)l, (void)m; }
void jk2_glFinish(void) {}
static GLuint lists = 1;
GLuint jk2_glGenLists(GLsizei range) {
    GLuint first = lists;
    lists += range;
    return first;
}
GLenum jk2_glGetError(void) { return 0; }
void jk2_glGetFloatv(GLenum name, GLfloat* v) {
    if (on) { SETTLE(); s_glGetFloatv(name, v); }
    else memset(v, 0, 16 * sizeof(GLfloat));
}
void jk2_glGetIntegerv(GLenum name, GLint* v) {
    switch (name) {
    case 0x84E2: *v = 1; return;              // GL_MAX_TEXTURE_UNITS_ARB: one, the renderer's plain paths
    case 0x0D05: case 0x0CF5: *v = 4; return; // GL_PACK_ALIGNMENT, GL_UNPACK_ALIGNMENT (0 made the screenshot of a save divide by it)
    // GL_MAX_TEXTURE_SIZE: 512 on the GPU (the menus' and loading screens' pictures of 1,024 were 12 of its 24 MB, and
    // the window is 640 wide); without a window as nullgl.c
    case 0x0D33: *v = on ? 512 : 2048; return;
    }
    if (on) { SETTLE(); s_glGetIntegerv(name, v); }
    else if (name == 0x0BA2) v[0] = v[1] = 0, v[2] = 640, v[3] = 480;
    else *v = 0;
}
const GLubyte* jk2_glGetString(GLenum name) {
    switch (name) {
    case 0x1F00: return (const GLubyte*)"ShaderEmu";
    case 0x1F01: return (const GLubyte*)(on ? "the machine's GPU" : "null");
    case 0x1F02: return (const GLubyte*)"1.1";
    default: return (const GLubyte*)"";   // no extension: the renderer takes its plain paths
    }
}
GLboolean jk2_glIsEnabled(GLenum c) { if (!on) return 0; SETTLE(); return s_glIsEnabled(c); }
void jk2_glLineWidth(GLfloat w) { (void)w; }
void jk2_glLoadIdentity(void) { ON_THEN(s_glLoadIdentity()); }
void jk2_glLoadMatrixf(const GLfloat* m) { ON_THEN(s_glLoadMatrixf(m)); }
void jk2_glMatrixMode(GLenum m) { ON_THEN(s_glMatrixMode(m)); }
void jk2_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) { ON_THEN(s_glOrtho(l, r, b, t, n, f)); }
void jk2_glPolygonMode(GLenum f, GLenum m) { (void)f, (void)m; }
void jk2_glPolygonOffset(GLfloat f, GLfloat u) { ON_THEN(s_glPolygonOffset(f, u)); }
void jk2_glPopMatrix(void) { ON_THEN(s_glPopMatrix()); }
void jk2_glPushMatrix(void) { ON_THEN(s_glPushMatrix()); }
// a black picture, for a saved game's screenshot (the rows padded to 4 bytes)
void jk2_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
    int bytes = format == 0x1907 ? 3 : 4;
    (void)x, (void)y, (void)type;
    memset(pixels, 0, (size_t)((width * bytes + 3) & ~3) * height);
}
void jk2_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { (void)x, (void)y, (void)w, (void)h; }
void jk2_glShadeModel(GLenum m) { (void)m; }
void jk2_glStencilFunc(GLenum f, GLint r, GLuint m) { (void)f, (void)r, (void)m; }
void jk2_glStencilMask(GLuint m) { (void)m; }
void jk2_glStencilOp(GLenum a, GLenum b, GLenum c) { (void)a, (void)b, (void)c; }
void jk2_glTexCoord2f(GLfloat s, GLfloat t) { ON_THEN(s_glTexCoord2f(s, t)); }
void jk2_glTexCoord2fv(const GLfloat* v) { ON_THEN(s_glTexCoord2fv(v)); }
void jk2_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* p) { ON_THEN(s_glTexCoordPointer(size, type, stride, p)); }
void jk2_glTexEnvf(GLenum t, GLenum n, GLfloat v) { ON_THEN(s_glTexEnvf(t, n, v)); }
void jk2_glTexImage2D(GLenum t, GLint l, GLint internal, GLsizei w, GLsizei h, GLint b, GLenum format, GLenum type, const void* p) {
    ON_THEN(s_glTexImage2D(t, l, internal, w, h, b, format, type, p));
}
void jk2_glTexParameterf(GLenum t, GLenum n, GLfloat v) { ON_THEN(s_glTexParameterf(t, n, v)); }
void jk2_glTexParameterfv(GLenum t, GLenum n, const GLfloat* v) { ON_THEN(s_glTexParameterfv(t, n, v)); }
void jk2_glTexParameteri(GLenum t, GLenum n, GLint v) { ON_THEN(s_glTexParameteri(t, n, v)); }
void jk2_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* p) {
    ON_THEN(s_glTexSubImage2D(t, l, x, y, w, h, format, type, p));
}
void jk2_glTranslatef(GLfloat x, GLfloat y, GLfloat z) { ON_THEN(s_glTranslatef(x, y, z)); }
void jk2_glVertex2f(GLfloat x, GLfloat y) { ON_THEN(s_glVertex2f(x, y)); }
void jk2_glVertex3f(GLfloat x, GLfloat y, GLfloat z) { ON_THEN(s_glVertex3f(x, y, z)); }
void jk2_glVertex3fv(const GLfloat* v) { ON_THEN(s_glVertex3fv(v)); }
void jk2_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* p) { ON_THEN(s_glVertexPointer(size, type, stride, p)); }
void jk2_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { ON_THEN(s_glViewport(x, y, w, h)); }
