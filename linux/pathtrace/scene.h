/* The mesh viewer's scene (docs/apps/raytrace.md, "nxpath"): meshes as they were read, objects that
 * place a mesh in the world, lights, one camera, and the world the path tracer traces, which is
 * every object's triangles where they stand under one tree of boxes. */
#ifndef PATHTRACE_SCENE_H
#define PATHTRACE_SCENE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SC_NAME 32
#define SC_MESHES 32
#define SC_OBJECTS 64
#define SC_LIGHTS 8
/* What sc_material() makes an object of: where its numbers start, no more. */
enum { SC_MATT, SC_PLASTIC, SC_METAL, SC_MIRROR, SC_GLASS, SC_GLOW, SC_KINDS };

/* A mesh in its own space: three corners a triangle, nine floats, and as many of normals (a
 * corner's is its vertex's, the mean of the faces round it, when the file gave none). */
struct sc_mesh {
	char name[SC_NAME];
	int triangles;
	float *positions, *normals;
	float low[3], high[3];
	void *shown;		/* the viewer's own: the mesh as the GPU draws it */
};

/* A mesh in the world: turned (degrees, about x, then y, then z), sized and put in its place.
 * `matrix` is those as one, a column after another as OpenGL has it: sc_object_matrix(). */
struct sc_object {
	char name[SC_NAME];
	int mesh;
	float place[3], turn[3], size[3];
	float matrix[16];
	float colour[3];
	float rough;		/* 0 a mirror's shine to 1 none: the metal's, the coat's over a colour, the glass's */
	float metal;		/* how much of it mirrors, in its colour */
	float glass;		/* how much of it lets light through */
	float ior;		/* how much glass bends light and a coat mirrors: 1.5 */
	float density;		/* how soon glass has taken the light that is not of its colour, a unit of the way */
	float glow;		/* its own light, times its colour */
	void *shown;		/* the viewer's own */
};

/* A light is a ball that shines its colour: `power` is of the whole ball, whatever its radius. */
struct sc_light {
	char name[SC_NAME];
	float place[3], radius, colour[3], power;
};

/* The camera goes round a point: `turn` about the world's y and `tilt` up from level, in
 * degrees, `far` away. sc_camera_set() makes the eye and its three ways of them. `lens` is how
 * far the view opens up and down at one unit ahead (0.6: as nxphys), `aspect` how much wider
 * than high what it sees is (0: as the picture's pixels are counted). */
struct sc_camera {
	float target[3], turn, tilt, far, lens, aspect;
	float eye[3], right[3], up[3], ahead[3];
};

struct sc_scene {
	struct sc_mesh meshes[SC_MESHES];
	struct sc_object objects[SC_OBJECTS];
	struct sc_light lights[SC_LIGHTS];
	int mesh_count, object_count, light_count;
	struct sc_camera camera;
	float sky[3], sky_power;	/* what a ray that meets nothing brings back */
};
extern struct sc_scene sc;

/* ---- mesh.c ---- */

/* Reads a mesh file (Wavefront .obj; Stanford .ply, written as text or as little-endian
 * bytes) into a free place of sc.meshes: its number, or -1 with `why` saying what was wrong.
 * Faces of more than three corners become fans. */
int sc_mesh_load(const char *path, char *why, int size);
/* Meshes made here: a square of the ground (y 0, a unit each way from the middle), a box and
 * a ball of radius one half round the origin (the ball with `rings` rings). Their numbers. */
int sc_mesh_plane(void);
int sc_mesh_box(void);
int sc_mesh_ball(int rings);
void sc_mesh_free(int mesh);
void sc_object_matrix(struct sc_object *object);
void sc_material(struct sc_object *object, int kind);
void sc_camera_set(struct sc_camera *camera);

/* ---- bvh.c: the world ---- */

/* A triangle of the world: a corner, its two edges from there, the corners' normals, and the
 * object it is of. 80 bytes: five of the machine's texels. */
struct sc_triangle { float corner[3], edge1[3], edge2[3], normal[3][3]; uint32_t object, unused; };
/* A box of the tree: it holds the triangles from `a`, `b` of them, or (b 0) the two boxes from `a`. */
struct sc_node { float low[3]; uint32_t a; float high[3]; uint32_t b; };
struct sc_world {
	struct sc_triangle *triangles;
	struct sc_node *nodes;		/* nodes[0] is the whole world */
	int triangle_count, node_count;
	uint32_t build_ms, build_workers;	/* the last build: how long, and how many workers had a part */
};
extern struct sc_world sc_world;

/* Makes sc_world of the scene as it is: every object's triangles in the world's space, and
 * the tree over them, built by up to `workers` worker cores of mcw (0: this core alone; the
 * workers must be open and have no job). False when there is no memory for it. The tree is
 * the same whichever cores built it. */
int sc_world_build(int workers);
/* For a test: the nearest triangle a ray meets by trying every one, with no tree (-1: none). */
int sc_world_nearest_slow(const float *from, const float *way, float *distance);

/* ---- tracer.c: the path tracer ---- */

/* How far a worker is with its share: its own 16 bytes, which the core that shows reads. */
struct pt_slot { volatile uint32_t samples, rays, unused[2]; };
/* What a picture is traced with: written between two pictures by the core that hands the
 * shares out, only read by the ones that trace. The camera and the lights are sc's. */
struct pt_picture {
	int width, height;	/* the width a multiple of four */
	int tile_w, tile_h, tiles;
	uint32_t stride, step;	/* the order the tiles are traced in: pt_share */
	int samples;		/* how many of every pixel the picture is to have */
	int stop;		/* 1: a share ends with the sample it is at; 2: with its tile, and the picture is not kept */
	struct pt_slot *slots;	/* one a worker, by its number */
	int bounces;		/* how many times a path may turn */
	int owners;		/* each tile framed in its core's colour */
	uint32_t seed;
	const uint32_t *glowing;	/* the world's triangles that glow, */
	const float *glow_upto;	/* and the area of them up to and with each: pt_glowing() */
	int glowing_count;
	float *sums;		/* four floats a pixel, 16 bytes: the light of the samples so far, and its rays */
	uint32_t *pixels;	/* the picture, 0x00RRGGBB */
};
extern struct pt_picture pt;

/* One more sample of every pixel of a tile: `how` is the sample's number (from 0), with the
 * core's number in its top byte. Returns the rays cast. */
uint32_t pt_tile(uint32_t tile, uint32_t how);
/* A worker's job: its share of the picture, sample after sample from where its slot is.
 * which: the worker's place w (from 0) among n, n from bit 16. how: its number from bit 24,
 * below that how many samples at most (0: all). Ends early when its core is wanted back. */
uint32_t pt_share(uint32_t which, uint32_t how);
/* On lanes (docs/machine/lanes.md): a batch of `count` samples from s0 of every pixel, one index a
 * pixel's sample, made in steps (pt_batch_step, the batch its context: "Steps") into vals, PT_STEP_TEXELS
 * an index, sample after sample; then pixels p0 to p1 of it folded into the sums in order, and shown
 * (pt_batch_fold); and whether the picture is made after a sample. */
#define PT_STEP_TEXELS 6
struct pt_batch {
	float *vals;
	uint32_t s0, count, pixels;
};
uint32_t pt_batch_step(uint32_t i, void *batch, uint32_t round);
uint32_t pt_batch_check(const struct pt_batch *b, uint32_t first, uint32_t count);
void pt_batch_fold(const struct pt_batch *b, uint32_t p0, uint32_t p1, int show);
int pt_shown_after(uint32_t sample);
/* The world's glowing triangles, which a matt surface asks for their light as it does a
 * light's: after sc_world_build() and after any glow changed. False when there is no memory. */
int pt_glowing(void);
/* Before the shares are handed out: the tracer's code and the world read, so that no worker
 * stops at a page of them. */
void pt_read_all(void);
/* The nearest thing an eye's ray through a point of the picture meets, for the viewer's
 * click: an object's number, -1 - a light's number, or SC_OBJECTS for nothing. Needs the
 * world built. x and y in pixels of a picture `width` by `height`. */
int pt_pick(float x, float y, int width, int height);
/* The way an eye's ray goes through that point of such a picture, for a test of the camera. */
void pt_way(float x, float y, int width, int height, float *way);

/* ---- gizmo.cpp: Dear ImGui and ImGuizmo, for the handles of what is selected only ---- */

enum { GIZMO_MOVE, GIZMO_TURN, GIZMO_SIZE };
/* Once, after seglInit: false when it cannot be. */
int gizmo_init(unsigned int window);
/* One frame's handles over the picture just drawn (before the swap), with the matrices it was
 * drawn with. `matrix` is the selected thing's: 1 when a drag changed it, 2 while the pointer
 * is over a handle or holds one (the click is the handle's then, not the view's). The pointer
 * in the picture's pixels, `buttons` bit 0 the left one. */
int gizmo_frame(const float *view, const float *projection, float *matrix, int what, int width, int height, int x, int y, int buttons);

#ifdef __cplusplus
}
#endif
#endif
