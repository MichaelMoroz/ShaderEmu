/*
gl_kept.c - the level's surfaces kept in GPU memory, drawn from there (docs/games/halflife.md)

Included at the end of ref/gl's gl_rsurf.c. When the light maps are built, every surface of the
world model that is one plain polygon becomes triangles in GPU memory, a texture's together, with
a word a vertex of where it is in one atlas of the light maps. A frame still finds what is seen
as ref/gl does; a seen surface then only checks its light map and is a run of those vertices, and
a texture's runs are a few lit draws.
*/
void hlglSay( const char *fmt, ... );	// (gl_shaderemu.c)
void *hlglKeptMemory( unsigned bytes );
int hlglLightAtlas( int count, int block, const int *names );
void hlglDrawKept( const void *vertices, const uint *light, const uint *firsts, const uint *counts, int runs );

#define KEPT_RUNS 512
#define KEPT_DECALED 128
#define KEPT_NOT ( SURF_DRAWSKY|SURF_DRAWTURB|SURF_DRAWTILED|SURF_CONVEYOR )

static int efrag_leaves = -1;
static mleaf_t **efrag_leaf;	// (R_KeptWorldGive)
static ushort *kept_tex;	// a kept surface's texture in the world's list; 0xffff: not drawn by texture
static int *kept_order, *kept_rank, kept_words;	// the surfaces with a texture in the order of their vertices
static uint *world_seen;	// a bit a rank, of the surfaces seen: the world worker's words, then its helper's

static struct
{
	model_t	*model;
	uint	*vertices, *light;	// in GPU memory: compact vertices, and their places in the atlas
	uint	*first, *count;		// a surface's vertices; count 0: it is drawn as before
	int	numsurfaces;
	int	lit;
	// the runs of the draw being gathered: one texture, one entity
	uint	run_first[KEPT_RUNS], run_count[KEPT_RUNS];
	int	runs, texture;
	cl_entity_t *entity;
	msurface_t *decaled[KEPT_DECALED];
	int	decaled_cull[KEPT_DECALED], numdecaled;
} kept;

static qboolean R_KeptCandidate( const msurface_t *fa )
{
	return fa->polys && !fa->polys->next && fa->polys->numverts >= 3 && !FBitSet( fa->flags, KEPT_NOT );
}

unsigned hlglWait( int k );
void hlglAtlasWriter( int k );
static int relit_out, relight_worker, glow_worker;	// (R_KeptFrameEnd's, the glows')
void R_StudioJobLevel( void );
static const int *R_EntitySurfaceOrderOf( int f, int n );
static int *esurf_order;	// (R_EntitySurfaceOrder)
static byte *esurf_sorted;

typedef struct { const texture_t *texture; int slot; } kept_slot_t;
static int R_KeptSlotCompare( const void *a, const void *b )
{
	const kept_slot_t *x = a, *y = b;
	return x->texture != y->texture ? ( x->texture < y->texture ? -1 : 1 ) : x->slot - y->slot;
}

// The kept candidates by texture: slot i's are by_texture[from[i]] to [from[i + 1]], in surface order,
// a surface under the first slot that holds its texture (as a scan of the slots in order finds it).
static void R_KeptByTexture( const model_t *m, int *by_texture, int *from )
{
	int n = m->numtextures;
	kept_slot_t *slots = malloc( sizeof( kept_slot_t ) * n );
	int *of = malloc( sizeof( int ) * m->numsurfaces ), *fill = malloc( sizeof( int ) * ( n + 1 ));

	memset( from, 0, sizeof( int ) * ( n + 1 ));
	if( !slots || !of || !fill )
	{
		free( slots ), free( of ), free( fill );
		return;
	}
	for( int i = 0; i < n; i++ )
		slots[i].texture = m->textures[i], slots[i].slot = i;
	qsort( slots, n, sizeof( kept_slot_t ), R_KeptSlotCompare );
	for( int j = 0; j < m->numsurfaces; j++ )
	{
		const texture_t *t = m->surfaces[j].texinfo->texture;
		int low = 0, high = n;	// the first of the slots holding t
		while( low < high )
		{
			int mid = ( low + high ) / 2;
			if( slots[mid].texture < t ) low = mid + 1;
			else high = mid;
		}
		of[j] = R_KeptCandidate( &m->surfaces[j] ) && low < n && slots[low].texture == t ? slots[low].slot : -1;
		if( of[j] >= 0 )
			from[of[j] + 1]++;
	}
	for( int i = 0; i < n; i++ )
		from[i + 1] += from[i];
	memcpy( fill, from, sizeof( int ) * n );
	for( int j = 0; j < m->numsurfaces; j++ )
		if( of[j] >= 0 )
			by_texture[fill[of[j]]++] = j;
	free( slots ), free( of ), free( fill );
}

static void R_SceneSet( void );
static void R_KeptBuild( void )
{
	R_SceneSet( );
	R_StudioJobLevel( );
	model_t *m = WORLDMODEL;
	int total = 0, blocks = 0;

	if( relit_out )
		hlglWait( relight_worker ), hlglAtlasWriter( 0 ), relit_out = 0;
	hlglWait( glow_worker );
	free( kept.first );
	free( kept_tex );
	free( kept_order ), kept_order = kept_rank = NULL;
	free( world_seen ), world_seen = NULL;
	free( efrag_leaf );
	free( esurf_order );
	free( esurf_sorted );
	esurf_order = NULL, esurf_sorted = NULL;
	kept_tex = NULL, efrag_leaf = NULL;
	efrag_leaves = -1;
	memset( &kept, 0, sizeof( kept ));
	if( !m || !m->numsurfaces )
		return;
	for( int j = 0; j < m->numsurfaces; j++ )
		if( R_KeptCandidate( &m->surfaces[j] ))
			total += ( m->surfaces[j].polys->numverts - 2 ) * 3;
	while( blocks < MAX_LIGHTMAPS && tr.lightmapTextures[blocks] )
		blocks++;
	kept.first = calloc( 2 * m->numsurfaces, sizeof( uint ));
	esurf_order = malloc( sizeof( int ) * m->numsurfaces );
	esurf_sorted = calloc( m->numsurfaces, 1 );
	if( !esurf_order || !esurf_sorted )
		free( esurf_order ), free( esurf_sorted ), esurf_order = NULL, esurf_sorted = NULL;
	kept.count = kept.first + m->numsurfaces;
	kept.vertices = hlglKeptMemory( total * 20 );
	if( !kept.vertices || !total )
	{
		gEngfuncs.Con_Printf( "the level is drawn as before: no room for %i vertices in GPU memory\n", total );
		kept.vertices = NULL;
		return;
	}
	kept.light = kept.vertices + total * 4;
	kept.lit = blocks > 0 && hlglLightAtlas( blocks, BLOCK_SIZE, tr.lightmapTextures );
	kept.model = m;
	kept.numsurfaces = m->numsurfaces;
	extern void ( *SE_LightAhead )( void );
	void R_LightAheadGive( void );
	SE_LightAhead = R_LightAheadGive;
	if( m->numtextures <= MAX_MAP_TEXTURES && ( kept_tex = malloc( sizeof( ushort ) * m->numsurfaces )))
		memset( kept_tex, 0xff, sizeof( ushort ) * m->numsurfaces );
	kept_words = ( m->numsurfaces + 127 ) / 128 * 4;
	kept_order = malloc( sizeof( int ) * 2 * m->numsurfaces );
	world_seen = malloc( sizeof( uint ) * 2 * kept_words );
	if( !kept_order || !world_seen )
		free( kept_order ), free( kept_tex ), kept_order = NULL, kept_tex = NULL;
	else kept_rank = kept_order + m->numsurfaces, memset( world_seen, 0, sizeof( uint ) * 2 * kept_words );
	int ranks = 0;

	// a texture's surfaces one after another, in the order the file has them (near ones near)
	int *by_texture = malloc( sizeof( int ) * ( m->numsurfaces + m->numtextures + 1 )), *from = by_texture + m->numsurfaces;
	if( !by_texture )
		return;
	R_KeptByTexture( m, by_texture, from );
	uint at = 0;
	for( int i = 0; i < m->numtextures; i++ )
	{
		for( int n = from[i]; n < from[i + 1]; n++ )
		{
			int j = by_texture[n];
			msurface_t *fa = &m->surfaces[j];

			glpoly2_t *p = fa->polys;
			float low_s = p->verts[0][3], low_t = p->verts[0][4];
			for( int k = 1; k < p->numverts; k++ )
			{
				low_s = Q_min( low_s, p->verts[k][3] );
				low_t = Q_min( low_t, p->verts[k][4] );
			}
			low_s = floorf( low_s ), low_t = floorf( low_t );
			kept.first[j] = at;
			for( int k = 1; k + 1 < p->numverts; k++ )
			{
				const int corner[3] = { 0, k, k + 1 };
				for( int c = 0; c < 3; c++, at++ )
				{
					const float *v = p->verts[corner[c]];
					uint *to = kept.vertices + at * 4;
					uint s = (uint)(int)(( v[3] - low_s ) * 1024.0f ) & 0xffff, t = (uint)(int)(( v[4] - low_t ) * 1024.0f ) & 0xffff;
					float ls = bound( 0.0f, v[5], 1.0f ), lt = bound( 0.0f, ( fa->lightmaptexturenum + v[6] ) / blocks, 1.0f );

					memcpy( to, v, sizeof( float ) * 3 );
					to[3] = s | t << 16;
					kept.light[at] = (uint)( ls * 65535.0f ) | (uint)( lt * 65535.0f ) << 16;
				}
			}
			kept.count[j] = at - kept.first[j];
			if( kept_tex && !( m->textures[i]->anim_total && m->textures[i]->name[0] == '-' ))
				kept_tex[j] = i, kept_rank[j] = ranks, kept_order[ranks++] = j;	// (a random tiling's frame is the surface's own)
		}
	}
	free( by_texture );
	for( int i = 1; i < m->numsubmodels; i++ )
		R_EntitySurfaceOrderOf( m->submodels[i].firstface, m->submodels[i].numfaces );
	gEngfuncs.Con_Printf( "the level is kept in GPU memory: %i vertices, %i light map blocks%s\n", total, blocks, kept.lit ? "" : " (not lit)" );
}

// The runs gathered drawn, and the decals of their surfaces after them.
static void R_KeptFlush( void )
{
	int n = 0, decaled = kept.numdecaled;
	qboolean lit;

	if( !kept.runs )
		return;
	// in order, and runs that follow one another as one
	for( int i = 1; i < kept.runs; i++ )
	{
		uint f = kept.run_first[i], c = kept.run_count[i];
		int k = i - 1;
		for( ; k >= 0 && kept.run_first[k] > f; k-- )
			kept.run_first[k + 1] = kept.run_first[k], kept.run_count[k + 1] = kept.run_count[k];
		kept.run_first[k + 1] = f, kept.run_count[k + 1] = c;
	}
	for( int i = 1; i < kept.runs; i++ )
	{
		if( kept.run_first[n] + kept.run_count[n] == kept.run_first[i] )
			kept.run_count[n] += kept.run_count[i];
		else n++, kept.run_first[n] = kept.run_first[i], kept.run_count[n] = kept.run_count[i];
	}
	lit = kept.lit && !r_fullbright->value && WORLDMODEL->lightdata && kept.entity
		&& kept.entity->curstate.rendermode != kRenderTransAdd && kept.entity->curstate.rendermode != kRenderGlow;
	hlglDrawKept( kept.vertices, lit ? kept.light : NULL, kept.run_first, kept.run_count, n + 1 );
	kept.runs = kept.numdecaled = 0;
	for( int i = 0; i < decaled; i++ )
		R_RenderDecalsForSurface( kept.decaled[i], kept.decaled_cull[i] );
}

// A surface drawn from the kept vertices (true), or not one of them.
static qboolean R_KeptSurface( msurface_t *fa, int cull_type )
{
	int j, texture = glState.currentTextures[glState.activeTMU];

	if( !kept.vertices || FBitSet( fa->flags, KEPT_NOT ))
		return false;
	j = fa - kept.model->surfaces;
	if( j < 0 || j >= kept.numsurfaces || !kept.count[j] )
		return false;
	if( kept.runs && ( texture != kept.texture || RI.currententity != kept.entity || kept.runs == KEPT_RUNS || kept.numdecaled == KEPT_DECALED ))
		R_KeptFlush();
	kept.texture = texture, kept.entity = RI.currententity;
	kept.run_first[kept.runs] = kept.first[j], kept.run_count[kept.runs] = kept.count[j];
	kept.runs++;
	R_CheckLightMap( fa );
	if( fa->pdecals )
	{
		kept.decaled[kept.numdecaled] = fa, kept.decaled_cull[kept.numdecaled] = cull_type;
		kept.numdecaled++;
	}
	return true;
}

// ---- the world's surfaces found by a worker core ----

int hlglKeptRoom( void *room, int draws, uint vertices, int lit );
void hlglKeptPut( void *room, uint block, int name, const void *vertices, const uint *light, const uint *firsts, const uint *counts, int runs );
static uint world_room[24] __attribute__(( aligned( 16 ))), ents_room[24] __attribute__(( aligned( 16 )));	// (the GL layer's rooms)
unsigned gpuAddress( const void *memory ), gpuMatricesBlock( void );
float *gpuModelviewBlock( void );
void gpuMatricesChanged( void );
int hlglWorldWorker( void ), hlglEntsWorker( void ), hlglGlowWorker( void ), hlglRelightWorker( void );
void hlglPost( int k, uint ( *fn )( uint, uint ), uint a0, uint a1 );
void hlglPostLate( int k, uint ( *fn )( uint, uint ), uint a0, uint a1 );

#define WORLD_RUNS 4096
#define WORLD_SURFS 1024
#define WORLD_ENTS 128
#define WORLD_ERUNS 2048
typedef struct { int count, full; msurface_t *s[WORLD_SURFS]; } surflist_t;

// What R_RecursiveWorldNode would have found, found by the worker: the kept surfaces as runs of a
// texture's vertices, and the others in lists for this core.
static struct __attribute__(( aligned( 16 )))
{
	gl_frustum_t frustum;
	vec3_t	modelorg;
	int	framecount, visframecount, facecull, nocull, dynamic, overflow;
	int	numruns, numtex, draws;
	uint	vertices;				// in the draws
	uint	sorted_first[WORLD_RUNS], sorted_count[WORLD_RUNS];
	ushort	tex[MAX_MAP_TEXTURES];			// the textures with runs, in order
	uint	tex_at[MAX_MAP_TEXTURES], tex_runs[MAX_MAP_TEXTURES];
	msurface_t *tex_surf[MAX_MAP_TEXTURES];		// one of its surfaces, for R_TextureAnimation
	surflist_t sky, slow, relight, decaled, bright;
} world;
// the brush entities seen, found by a worker of their own: theirs are drawn as runs too, or as before
static struct __attribute__(( aligned( 16 )))
{
	int	numents, numeruns;
	struct { cl_entity_t *e; vec3_t org; int fixed, later, clip; } ent[WORLD_ENTS];	// clip: the planes its box crosses
	int	ent_at[WORLD_ENTS] __attribute__(( aligned( 16 ))), ent_runs[WORLD_ENTS] __attribute__(( aligned( 16 )));
	int	ent_slow[WORLD_ENTS] __attribute__(( aligned( 16 )));	// slow: why (HL_WORLD=log)
	ushort	erun_tex[WORLD_ERUNS] __attribute__(( aligned( 16 )));
	uint	erun_first[WORLD_ERUNS] __attribute__(( aligned( 16 ))), erun_count[WORLD_ERUNS] __attribute__(( aligned( 16 )));
	msurface_t *erun_surf[WORLD_ERUNS] __attribute__(( aligned( 16 )));
	surflist_t relight __attribute__(( aligned( 16 )));
	int	half;	// the entities from here on found by the glow worker
} went;
// the light maps of the surfaces whose light styles changed, built by each worker into its own
#define WORLD_LIGHT_BYTES 0x20000
#define WORLD_ELIGHT_BYTES 0x8000
typedef struct
{
	int	count, used, size;
	byte	*bytes;
	struct { msurface_t *s; int at, smax, tmax, written; } built[WORLD_SURFS];	// written: into the atlas by the worker
	uint	blocklights[132 * 132 * 3];
} relit_t;
static byte relit_bytes[WORLD_LIGHT_BYTES] __attribute__(( aligned( 16 )));
static byte erelit_bytes[WORLD_ELIGHT_BYTES] __attribute__(( aligned( 16 )));
static relit_t relit __attribute__(( aligned( 16 ))) = { .size = WORLD_LIGHT_BYTES, .bytes = relit_bytes };
static relit_t erelit __attribute__(( aligned( 16 ))) = { .size = WORLD_ELIGHT_BYTES, .bytes = erelit_bytes };
// the glow sprites' traces from the eye, made by the worker too
#define WORLD_GLOWS 64
#include "pm_local.h"
void *SE_VisTraceOwn( void );
float SE_VisTraceFraction( void *own, vec3_t start, vec3_t end, int flags );
static struct __attribute__(( aligned( 16 )))
{
	int	count, ready;
	vec3_t	eye;
	struct { const cl_entity_t *e; vec3_t origin; float fraction; } glow[WORLD_GLOWS];
} glows;
// the fractions of the frame before, which this one draws with: its worker has the frame
static struct { int count; const cl_entity_t *e[WORLD_GLOWS]; float fraction[WORLD_GLOWS]; } glows_before;
static void *glow_own;
static cl_entity_t *world_taken[WORLD_ENTS];	// (this core's: drawn by R_KeptWorldFinish)
static int world_numtaken, world_finishing;
static cl_entity_t *world_later[WORLD_ENTS];	// (the translucent ones, drawn where they come)
static int world_later_at[WORLD_ENTS], world_numlater;
int hlglEntsHelper( void );
static int world_worker, ents_worker, ents_helper, glow_worker, relight_worker, world_out, ents_done;	// (this core's, outside the workers' texels)
static struct __attribute__(( aligned( 16 ))) { volatile int walked, marked, unused[2]; } world_walk;	// the world worker's: its lists are made, its nodes found

int mcw_on_worker( void );
#ifdef __gpuvm__
#define END_PASS() __asm__ volatile( "sync" )	// (mc.h's mc_end_pass)
#else
#define END_PASS() __asm__ volatile( ".word 0x0000100f" )	// (mc.h's mc_end_pass)
#endif
#ifdef __gpuvm__
static inline uint32_t tl_now( void ) { uint32_t v; __asm__ volatile( "mfsr %0, time_lo" : "=r"( v )); return v; }	// DEBUG
#else
static inline uint32_t tl_now( void ) { uint32_t v; __asm__ volatile( "rdtime %0" : "=r"( v )); return v; }	// DEBUG
#endif
typedef struct __attribute__(( aligned( 16 ))) { volatile uint32_t at, unused[3]; } tl_t;
static tl_t tl_world, tl_mark, tl_help, tl_ents, tl_enhelp, tl_glow, tl_relight;
static struct { uint32_t give, sum[12], n; } tl;
static void R_WorldList( surflist_t *l, msurface_t *s )
{
	if( l->count < WORLD_SURFS ) l->s[l->count++] = s;
	else l->full = 1;
}

// The world's nodes seen with surfaces, found by the world worker; their surfaces are then looked
// at in two halves, the first by it and the second by the relight worker into lists of its own.
#define WORLD_NODES 4096
static struct __attribute__(( aligned( 16 ))) { int count, surfaces, half; mnode_t *node[WORLD_NODES]; byte clip[WORLD_NODES]; } wnodes;
static struct __attribute__(( aligned( 16 ))) { surflist_t sky, slow, relight, decaled, bright; int numruns; } wpart;
static struct __attribute__(( aligned( 16 ))) { volatile int state, done, unused[2]; } wpart_state;	// state 1: the helper takes the half; 2, 3: it does not

static void R_WorldMark( mnode_t *node, uint clipflags )
{
	model_t *m = kept.model;
loc0:
	if( node->contents == CONTENTS_SOLID || node->visframe != world.visframecount )
		return;
	if( clipflags && !world.nocull )
	{
		for( int i = 0; i < 6; i++ )
		{
			if( !FBitSet( clipflags, BIT( i )))
				continue;
			int clipped = BOX_ON_PLANE_SIDE( node->minmaxs, node->minmaxs + 3, &world.frustum.planes[i] );
			if( clipped == 2 ) return;
			if( clipped == 1 ) ClearBits( clipflags, BIT( i ));
		}
	}
	if( node->contents < 0 )
	{
		mleaf_t *pleaf = (mleaf_t *)node;
		msurface_t **mark = pleaf->firstmarksurface;
		for( int i = 0; i < pleaf->nummarksurfaces; i++ )
			mark[i]->visframe = world.framecount;
		return;
	}

	int side = PlaneDiff( world.modelorg, node->plane ) >= 0.0f ? 0 : 1;
	R_WorldMark( node_child( node, side, m ), clipflags );
	int n = node_numsurfaces( node, m );
	if( n && wnodes.count < WORLD_NODES )
		wnodes.node[wnodes.count] = node, wnodes.clip[wnodes.count++] = clipflags, wnodes.surfaces += n;
	else if( n ) world.overflow = 1;
	node = node_child( node, !side, m );
	goto loc0;
}

static void R_WorldSurfaces( int helper, int from, int to )
{
	model_t *m = kept.model;
	surflist_t *sky = helper ? &wpart.sky : &world.sky, *slow = helper ? &wpart.slow : &world.slow, *relight = helper ? &wpart.relight : &world.relight;
	surflist_t *decaled = helper ? &wpart.decaled : &world.decaled, *bright = helper ? &wpart.bright : &world.bright;
	uint *seen = world_seen + ( helper ? kept_words : 0 );
	int runs = 0;

	for( int i = from; i < to; i++ )
	{
		mnode_t *node = wnodes.node[i];
		uint clipflags = wnodes.clip[i];
		int first = node_firstsurface( node, m ), last = first + node_numsurfaces( node, m );
		for( int j = first; j < last; j++ )
		{
			msurface_t *surf = &m->surfaces[j];
			texture_t *t;

			if( surf->visframe != world.framecount || !surf->texinfo || !( t = surf->texinfo->texture ))
				continue;
			// R_CullSurface's tests, for the world
			if( !world.nocull && world.facecull != GL_NONE && !VectorIsNull( surf->plane->normal ))
			{
				float dist = PlaneDiff( world.modelorg, surf->plane );
				if( FBitSet( surf->flags, SURF_PLANEBACK )) dist = -dist;
				if( world.facecull == GL_FRONT ? dist <= BACKFACE_EPSILON : dist >= -BACKFACE_EPSILON )
					continue;
			}
			if( !world.nocull && clipflags && GL_FrustumCullBox( &world.frustum, surf->info->mins, surf->info->maxs, clipflags ))
				continue;

			if( FBitSet( surf->flags, SURF_DRAWSKY ))
			{
				R_WorldList( sky, surf );
				continue;
			}
			int k = kept_tex[j];
			if( !kept.count[j] || k == 0xffff || runs == WORLD_RUNS / 2 )
			{
				R_WorldList( slow, surf );
				continue;
			}
			runs++;
			seen[kept_rank[j] >> 5] |= 1u << ( kept_rank[j] & 31 );
			for( int maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++ )
				if( g_lightstylevalue[surf->styles[maps]] != surf->cached_light[maps] )
				{
					R_WorldList( relight, surf );
					break;
				}
			if( surf->pdecals )
				R_WorldList( decaled, surf );
			if( t->fb_texturenum || t->dt_texturenum || t->anim_total )
				R_WorldList( bright, surf );
		}
	}
	if( helper ) wpart.numruns = runs;
	else world.numruns = runs;
}

static void R_WorldListAdd( surflist_t *to, const surflist_t *from )
{
	for( int i = 0; i < from->count; i++ )
		R_WorldList( to, from->s[i] );
	to->full |= from->full;
}

// The helper's half of the surfaces, from the relight worker: false when it is not to take it.
static qboolean R_WorldHelp( void )
{
	if( wpart_state.state || !mcw_on_worker( ))
	{
		if( !wpart_state.state ) wpart_state.state = 2;
		return false;
	}
	wpart_state.state = 1;
	while( !world_walk.marked )
		END_PASS( );
	R_WorldSurfaces( 1, wnodes.half, wnodes.count );
	wpart_state.done = 1, tl_help.at = tl_now( );
	return true;
}

// An entity's surfaces as runs by texture (0), or why one of them must be drawn as before.
// A brush model's surfaces in the order of their kept vertices (of texture, then place), sorted
// when the level is built; NULL when there is no room for the orders.
static const int *R_EntitySurfaceOrderOf( int f, int n )
{
	if( !esurf_order || f < 0 || n <= 0 || f + n > kept.numsurfaces )
		return NULL;
	int *o = esurf_order + f;
	if( !esurf_sorted[f] )
	{
		for( int i = 0; i < n; i++ )
		{
			int j = f + i, k = i;
			uint key = kept.count[j] ? kept.first[j] : 0xffffffffu;
			for( ; k > 0 && ( kept.count[o[k - 1]] ? kept.first[o[k - 1]] : 0xffffffffu ) > key; k-- )
				o[k] = o[k - 1];
			o[k] = j;
		}
		esurf_sorted[f] = 1;
	}
	return o;
}
static const int *R_EntitySurfaceOrder( const model_t *m )
{
	return R_EntitySurfaceOrderOf( m->firstmodelsurface, m->nummodelsurfaces );
}

// the glow worker's half: its runs from WORLD_ERUNS / 2 on, its light maps to build
static struct __attribute__(( aligned( 16 ))) { surflist_t relight; volatile int done; } ents_help;
static int R_WorldEntity( int n, int *cursor, int end, surflist_t *relight )
{
	model_t *m = went.ent[n].e->model;
	const float *org = went.ent[n].org;
	const int *order = R_EntitySurfaceOrder( m );
	int at = *cursor, runs = 0;

	went.ent_at[n] = at;
	for( int i = 0; i < m->nummodelsurfaces; i++ )
	{
		int j = order ? order[i] : m->firstmodelsurface + i;	// (in order, the sort below moves nothing)
		msurface_t *surf = &m->surfaces[j];
		texture_t *t;

		if( FBitSet( surf->flags, SURF_DRAWTURB|SURF_DRAWSKY ))
			return 1;
		if( !surf->texinfo || !( t = surf->texinfo->texture ))
			continue;
		if( !world.nocull && world.facecull != GL_NONE && !VectorIsNull( surf->plane->normal ))
		{
			float dist = PlaneDiff( org, surf->plane );
			if( FBitSet( surf->flags, SURF_PLANEBACK )) dist = -dist;
			if( world.facecull == GL_FRONT ? dist <= BACKFACE_EPSILON : dist >= -BACKFACE_EPSILON )
				continue;
		}
		if( !world.nocull && went.ent[n].fixed && went.ent[n].clip && GL_FrustumCullBox( &world.frustum, surf->info->mins, surf->info->maxs, went.ent[n].clip ))
			continue;
		int k = kept_tex[j];
		if( !kept.count[j] || k == 0xffff || surf->pdecals || t->fb_texturenum || t->dt_texturenum || at + runs == end )
			return !kept.count[j] ? 2 : k == 0xffff ? 3 : surf->pdecals ? 4 : t->fb_texturenum ? 5 : t->dt_texturenum ? 6 : 7;
		for( int maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++ )
			if( g_lightstylevalue[surf->styles[maps]] != surf->cached_light[maps] )
			{
				R_WorldList( relight, surf );
				break;
			}
		// in order of texture and place
		uint f = kept.first[j], r = at + runs++;
		for( ; r > at && ( went.erun_tex[r - 1] > k || ( went.erun_tex[r - 1] == k && went.erun_first[r - 1] > f )); r-- )
		{
			went.erun_tex[r] = went.erun_tex[r - 1], went.erun_surf[r] = went.erun_surf[r - 1];
			went.erun_first[r] = went.erun_first[r - 1], went.erun_count[r] = went.erun_count[r - 1];
		}
		went.erun_tex[r] = k, went.erun_surf[r] = surf, went.erun_first[r] = f, went.erun_count[r] = kept.count[j];
	}
	// runs that follow one another as one
	int to = at;
	for( int r = at + 1; r < at + runs; r++ )
	{
		if( went.erun_tex[r] == went.erun_tex[to] && went.erun_first[to] + went.erun_count[to] == went.erun_first[r] )
			went.erun_count[to] += went.erun_count[r];
		else
		{
			to++;
			went.erun_tex[to] = went.erun_tex[r], went.erun_surf[to] = went.erun_surf[r];
			went.erun_first[to] = went.erun_first[r], went.erun_count[to] = went.erun_count[r];
		}
	}
	went.ent_runs[n] = runs ? to - at + 1 : 0;
	*cursor = at + went.ent_runs[n];
	return 0;
}

// R_CheckLightMap's work but the upload, for the surfaces of the list
static void R_Relight( surflist_t *l, relit_t *r )
{
	r->count = r->used = 0;
	for( int i = 0; world.dynamic && i < l->count; i++ )
	{
		msurface_t *fa = l->s[i];
		int sample_size = gEngfuncs.Mod_SampleSizeForFace( fa );
		int smax = ( fa->info->lightextents[0] / sample_size ) + 1, tmax = ( fa->info->lightextents[1] / sample_size ) + 1;

		if( smax >= 132 || tmax >= 132 || r->used + smax * tmax * 4 > r->size )
			continue;
		R_BuildLightMapTo( r->blocklights, fa, r->bytes + r->used, smax * 4, true );
		R_UpdateSurfaceCachedLight( fa );
		r->built[r->count].s = fa, r->built[r->count].at = r->used;
		r->built[r->count].smax = smax, r->built[r->count].tmax = tmax;
		r->count++, r->used += smax * tmax * 4;
	}
}

static struct __attribute__(( aligned( 16 ))) { uint32_t walk, sort, unused[2]; } wdbg, wdbg_said;	// DEBUG: the world worker's, this core's copy
#ifdef __gpuvm__
static inline uint32_t wd_now( void ) { uint32_t v; __asm__ volatile( "mfsr %0, cycle" : "=r"( v )); return v; }
#else
static inline uint32_t wd_now( void ) { uint32_t v; __asm__ volatile( "rdinstret %0" : "=r"( v )); return v; }
#endif
static struct __attribute__(( aligned( 16 ))) { uint32_t walk, light, write, ents, ewrite, unused[3]; } rdbg, rdbg_said;	// DEBUG: the relight worker's, this core's copy
static struct __attribute__(( aligned( 16 ))) { uint32_t n, draw, world, unused; } rdbg0;	// DEBUG: this core's
static uint R_WorldJob( uint clipflags, uint unused )
{
	int n = 0;
	uint32_t w0 = wd_now( );

	wnodes.count = wnodes.surfaces = 0;
	R_WorldMark( kept.model->nodes, clipflags );
	wdbg.unused[0] += wd_now( ) - w0, wdbg.unused[1] += wnodes.count;
	int half = 0;
	for( int i = 0, sum = 0; i < wnodes.count && sum * 2 < wnodes.surfaces; i++ )
		sum += node_numsurfaces( wnodes.node[i], kept.model ), half = i + 1;
	wnodes.half = half;
	if( !mcw_on_worker( ))
		wpart_state.state = 3;	// (here and now on this core: the helper is not out yet)
	world_walk.marked = 1, tl_mark.at = tl_now( );
	R_WorldSurfaces( 0, 0, half );
	while( !wpart_state.state )
		END_PASS( );
	if( wpart_state.state != 1 )
		R_WorldSurfaces( 0, half, wnodes.count );
	else
	{
		int runs = world.numruns;
		while( !wpart_state.done )
			END_PASS( );
		R_WorldListAdd( &world.sky, &wpart.sky ), R_WorldListAdd( &world.slow, &wpart.slow ), R_WorldListAdd( &world.relight, &wpart.relight );
		R_WorldListAdd( &world.decaled, &wpart.decaled ), R_WorldListAdd( &world.bright, &wpart.bright );
		world.numruns = runs + wpart.numruns;
	}
	uint32_t w1 = wd_now( );
	world_walk.walked = 1;	// (R_RelightJob builds the light maps meanwhile)
	// the runs by texture, each texture's in order and those that follow one another as one
	static const byte debruijn[32] = { 0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
		31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9 };
	uint at = 0, vertices = 0;
	int t = -1;
	for( int w = 0; w < kept_words; w++ )
	{
		uint bits = world_seen[w] | world_seen[w + kept_words];
		if( !bits )
			continue;
		world_seen[w] = world_seen[w + kept_words] = 0;
		do
		{
			int j = kept_order[w * 32 + debruijn[(( bits & -bits ) * 0x077cb531u ) >> 27]], k = kept_tex[j];
			uint f = kept.first[j], c = kept.count[j];
			bits &= bits - 1, vertices += c;
			if( k == t && world.sorted_first[at - 1] + world.sorted_count[at - 1] == f )
			{
				world.sorted_count[at - 1] += c;
				continue;
			}
			if( k != t )
				t = k, world.tex[n++] = k, world.tex_at[k] = at, world.tex_runs[k] = 0, world.tex_surf[k] = &kept.model->surfaces[j];
			world.tex_runs[k]++;
			world.sorted_first[at] = f, world.sorted_count[at] = c, at++;
		} while( bits );
	}
	world.numtex = n, world.draws = at, world.vertices = vertices;
	wdbg.walk += w1 - w0, wdbg.sort += wd_now( ) - w1;
	tl_world.at = tl_now( );
	return 0;
}

// The world's draws written into the room this core took (hlglKeptRoom), each texture's name given.
static texture_t *R_TextureAnimationOf( msurface_t *s, const cl_entity_t *e );
static uint R_WorldDrawJob( uint lit, uint unused )
{
	for( int k = 0; k < world.numtex; k++ )
	{
		int t = world.tex[k];
		uint n = R_TextureAnimationOf( world.tex_surf[t], NULL )->gl_texturenum;	// (the world's frame is 0)
		int name = R_GetTexture( n > 0 && n < MAX_TEXTURES ? n : tr.defaultTexture )->texnum;
		hlglKeptPut( world_room, 0, name, kept.vertices, lit ? kept.light : NULL, world.sorted_first + world.tex_at[t], world.sorted_count + world.tex_at[t], world.tex_runs[t] );
	}
	return 0;
}

// The light maps built written into the atlas, which this worker alone writes while it is out.
int hlglLightBlock( unsigned name, int x, int y, int width, int height, const byte *rgba );
void hlglAtlasWriter( int k );
static void R_RelitWrite( relit_t *r )
{
	for( int k = 0; k < r->count; k++ )
	{
		msurface_t *fa = r->built[k].s;
		uint n = tr.lightmapTextures[fa->lightmaptexturenum];
		r->built[k].written = n > 0 && n < MAX_TEXTURES
			&& hlglLightBlock( R_GetTexture( n )->texnum, fa->light_s, fa->light_t, r->built[k].smax, r->built[k].tmax, r->bytes + r->built[k].at );
	}
}

static struct __attribute__(( aligned( 16 ))) { volatile int done, unused[3]; } ents_lit;	// the entities' worker's: erelit is built

// The world's light maps, once its worker has found the surfaces, and all those built written.
static uint R_RelightJob( uint unused0, uint unused1 )
{
	if( !R_WorldHelp( ) && !mcw_on_worker( ))
		hlglWait( world_worker ), hlglWait( ents_worker );	// (here and now on this core: its core was taken)
	uint32_t r0 = wd_now( );
	while( !world_walk.walked )
		{ END_PASS( ); rdbg.walk += 1000; }
	uint32_t r1 = wd_now( );
	R_Relight( &world.relight, &relit );
	uint32_t r2 = wd_now( );
	R_RelitWrite( &relit );
	uint32_t r3 = wd_now( );
	while( !ents_lit.done )
		{ END_PASS( ); rdbg.ents += 1000; }
	uint32_t r4 = wd_now( );
	R_RelitWrite( &erelit );
	tl_relight.at = tl_now( );
	rdbg.walk += r1 - r0, rdbg.light += r2 - r1, rdbg.write += r3 - r2, rdbg.ents += r4 - r3, rdbg.ewrite += wd_now( ) - r4;
	return 0;
}

static struct __attribute__(( aligned( 16 ))) { uint32_t ents, light, surfs, n; } edbg;	// DEBUG
#ifdef __gpuvm__
static inline uint32_t ed_now( void ) { uint32_t v; __asm__ volatile( "mfsr %0, cycle" : "=r"( v )); return v; }
#else
static inline uint32_t ed_now( void ) { uint32_t v; __asm__ volatile( "rdinstret %0" : "=r"( v )); return v; }
#endif
static uint R_EntsJob( uint unused0, uint unused1 )
{
	uint32_t t0 = ed_now( );
	int cursor = 0;
	for( int n = 0; n < went.half; n++ )
		went.ent_slow[n] = R_WorldEntity( n, &cursor, WORLD_ERUNS / 2, &went.relight ), edbg.surfs += went.ent[n].e->model->nummodelsurfaces;
	while( !ents_help.done )
		END_PASS( );
	R_WorldListAdd( &went.relight, &ents_help.relight );
	uint32_t t1 = ed_now( );
	R_Relight( &went.relight, &erelit );
	ents_lit.done = 1, tl_ents.at = tl_now( );
	edbg.ents += t1 - t0, edbg.light += ed_now( ) - t1, edbg.n += went.numents;
	return 0;
}

static uint R_EntsHelpJob( uint unused0, uint unused1 )
{
	int cursor = WORLD_ERUNS / 2;
	for( int n = went.half; n < went.numents; n++ )
		went.ent_slow[n] = R_WorldEntity( n, &cursor, WORLD_ERUNS, &ents_help.relight );
	ents_help.done = 1, tl_enhelp.at = tl_now( );
	return 0;
}

static uint R_GlowJob( uint unused0, uint unused1 )
{
	uint32_t g0 = wd_now( );
	for( int i = 0; i < glows.count; i++ )
		glows.glow[i].fraction = SE_VisTraceFraction( glow_own, glows.eye, glows.glow[i].origin, PM_GLASS_IGNORE|PM_STUDIO_IGNORE );
	tl_glow.at = tl_now( ), tl_glow.unused[0] = wd_now( ) - g0, tl_glow.unused[1] = glows.count;
	return 0;
}

static void R_RelitUpload( surflist_t *l, relit_t *r );
static int relit_out;	// the world's light maps still to upload
static void R_WorldJobsWait( void )
{
	hlglWait( world_worker ), hlglWait( ents_worker ), hlglWait( ents_helper );
	relit_out = world_out, world_out = 0;
}

// The world's light maps uploaded when the next frame's are asked for: a frame late, which no
// flicker shows, and the worker has the rest of the frame.
static void R_KeptFrameEnd( void )
{
	if( !relit_out )
		return;
	hlglWait( relight_worker );
	hlglAtlasWriter( 0 );
	R_RelitUpload( &world.relight, &relit );
	R_RelitUpload( &went.relight, &erelit );
	relit_out = 0;
}

// The world's surfaces given to a worker (true), to be drawn by R_KeptWorldFinish.
static qboolean R_KeptWorldGive( uint clipflags )
{
	static int off = -1;
	model_t *m = kept.model;

	static int log = -1;
	if( off < 0 ) off = getenv( "HL_WORLD" ) && !strcmp( getenv( "HL_WORLD" ), "inline" );
	if( log < 0 ) log = getenv( "HL_WORLD" ) && !strcmp( getenv( "HL_WORLD" ), "log" );
	if( log && !world_out && !( tr.framecount & 127 ))
	{
		for( int n = 0; n < went.numents; n++ )
			if( went.ent_slow[n] )
				hlglSay( "hl world: %s %d slow %d\n", went.ent[n].e->model->name, went.ent[n].e->curstate.rendermode, went.ent_slow[n] );
		for( int i = 0, solids = tr.draw_list->num_solid_entities; i < solids + (int)tr.draw_list->num_trans_entities; i++ )
		{
			cl_entity_t *e = i < solids ? tr.draw_list->solid_entities[i] : tr.draw_list->trans_entities[i - solids];
			if( e->model && e->model->type == mod_brush )
				hlglSay( "hl world: %s %s mode %d turned %d\n", i < solids ? "solid" : "trans", e->model->name,
					e->curstate.rendermode, e->angles[0] != 0.0f || e->angles[1] != 0.0f || e->angles[2] != 0.0f );
		}
	}
	if( world_out )
		R_WorldJobsWait( );
	R_KeptFrameEnd( );
	hlglWait( glow_worker );
	glows_before.count = glows.count;
	for( int i = 0; i < glows.count; i++ )
		glows_before.e[i] = glows.glow[i].e, glows_before.fraction[i] = glows.glow[i].fraction;
	glows.count = 0;
	if( tl.give )
	{
		uint32_t now = tl_now( ), *v[7] = { &tl_mark.at, &tl_help.at, &tl_world.at, &tl_enhelp.at, &tl_ents.at, &tl_glow.at, &tl_relight.at };
		for( int i = 0; i < 7; i++ ) if( *v[i] > tl.give ) tl.sum[i] += *v[i] - tl.give;
		tl.sum[9] += now - tl.give, tl.n++, tl.sum[10] += tl_glow.unused[0], tl.sum[11] += tl_glow.unused[1];
		if( tl.n == 256 )
		{
			hlglSay( "hl timeline (passes after the give): mark %.1f help %.1f world %.1f enhelp %.1f ents %.1f glow %.1f relight %.1f; asks ents %.1f world %.1f; frame %.1f; glow %u instructions, %u glows\n",
				tl.sum[0] / 3136.0, tl.sum[1] / 3136.0, tl.sum[2] / 3136.0, tl.sum[3] / 3136.0, tl.sum[4] / 3136.0, tl.sum[5] / 3136.0, tl.sum[6] / 3136.0, tl.sum[7] / 3136.0, tl.sum[8] / 3136.0, tl.sum[9] / 3136.0, tl.sum[10] / 256, tl.sum[11] / 256 );
			memset( tl.sum, 0, sizeof( tl.sum )), tl.n = 0;
		}
	}
	tl.give = tl_now( );
	world_numtaken = 0;
	if( off || !kept.vertices || m != WORLDMODEL || !kept_tex || ( world_worker = hlglWorldWorker( )) <= 0
		|| ( ents_worker = hlglEntsWorker( )) <= 0 || ( glow_worker = hlglGlowWorker( )) <= 0 || ( ents_helper = hlglEntsHelper( )) <= 0 || ( relight_worker = hlglRelightWorker( )) <= 0 )
		return false;
	// the world's walk first: it reads nothing of what is found below
	world.frustum = RI.frustum;
	VectorCopy( tr.modelorg, world.modelorg );
	world.framecount = tr.framecount, world.visframecount = tr.visframecount;
	world.facecull = glState.faceCull, world.nocull = r_nocull.value != 0.0f, world.dynamic = r_dynamic->value != 0.0f;
	world.numruns = world.overflow = 0;
	world.sky.count = world.slow.count = world.relight.count = world.decaled.count = world.bright.count = 0;
	world.sky.full = world.slow.full = world.relight.full = world.decaled.full = world.bright.full = 0;
	wpart.sky.count = wpart.slow.count = wpart.relight.count = wpart.decaled.count = wpart.bright.count = 0;
	wpart.sky.full = wpart.slow.full = wpart.relight.full = wpart.decaled.full = wpart.bright.full = 0;
	wpart_state.state = wpart_state.done = 0;
	world_walk.walked = world_walk.marked = 0;
	hlglPost( world_worker, R_WorldJob, clipflags, 0 );
	// the static entities of the leaves seen, here and now: the entities are drawn before the world
	if( efrag_leaves < 0 )
	{
		efrag_leaves = 0;
		for( int i = 1; i <= m->numleafs; i++ )
			if( m->leafs[i].efrags ) efrag_leaves++;
		efrag_leaf = efrag_leaves ? malloc( sizeof( *efrag_leaf ) * efrag_leaves ) : NULL;
		for( int i = 1, n = 0; efrag_leaf && i <= m->numleafs; i++ )
			if( m->leafs[i].efrags ) efrag_leaf[n++] = &m->leafs[i];
	}
	for( int i = 0; i < efrag_leaves; i++ )
	{
		mleaf_t *l = efrag_leaf[i];
		if( l->visframe == tr.visframecount && l->efrags && l->contents != CONTENTS_SOLID
			&& ( r_nocull.value || !GL_FrustumCullBox( &RI.frustum, l->minmaxs, l->minmaxs + 3, clipflags )))
			gEngfuncs.R_StoreEfrags( &l->efrags, tr.realframecount );
	}
	// the solid brush entities seen, of the world's model, as they are
	went.numents = world_numtaken = 0;
	// the glow sprites that are where their entities are
	glows.count = 0;
	if( !glow_own ) glow_own = SE_VisTraceOwn( );
	VectorCopy( RI.rvp.vieworigin, glows.eye );
	for( int i = 0; glow_own && !r_traceglow.value && i < tr.draw_list->num_trans_entities && glows.count < WORLD_GLOWS; i++ )
	{
		cl_entity_t *e = tr.draw_list->trans_entities[i];
		if( !e->model || e->model->type != mod_sprite || e->curstate.rendermode != kRenderGlow || e->curstate.aiment > 0 )
			continue;
		vec3_t v;	// (R_SpriteOccluded's test: off the screen, it is not traced)
		TriWorldToScreen( e->origin, v );
		if( v[0] < RI.rvp.viewport[0] || v[0] > RI.rvp.viewport[0] + RI.rvp.viewport[2] || v[1] < RI.rvp.viewport[1] || v[1] > RI.rvp.viewport[1] + RI.rvp.viewport[3] )
			continue;
		glows.glow[glows.count].e = e;
		VectorCopy( e->origin, glows.glow[glows.count].origin );
		glows.count++;
	}
	world_numlater = 0;
	for( int i = 0, solids = tr.draw_list->num_solid_entities; i < solids + (int)tr.draw_list->num_trans_entities
		&& !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ) && went.numents < WORLD_ENTS; i++ )
	{
		cl_entity_t *e = i < solids ? tr.draw_list->solid_entities[i] : tr.draw_list->trans_entities[i - solids];
		model_t *bm = e->model;
		int mode = e->curstate.rendermode;
		vec3_t mins, maxs;

		if( !bm || bm->type != mod_brush || bm->surfaces != m->surfaces
			|| FBitSet( bm->flags, MODEL_LIQUID ) || ( i < solids ? mode != kRenderNormal : mode == kRenderTransColor || mode == kRenderGlow ))
			continue;
		qboolean turned = !VectorIsNull( e->angles );
		for( int k = 0; k < 3; k++ )
		{
			mins[k] = e->origin[k] + ( turned ? -bm->radius : bm->mins[k] );
			maxs[k] = e->origin[k] + ( turned ? bm->radius : bm->maxs[k] );
		}
		if( R_CullBox( mins, maxs ))
			continue;
		went.ent[went.numents].e = e;
		went.ent[went.numents].fixed = R_StaticEntity( e );
		int clip = RI.frustum.clipFlags;
		for( int p = 0; p < 6; p++ )
			if( FBitSet( clip, BIT( p )) && BOX_ON_PLANE_SIDE( mins, maxs, &RI.frustum.planes[p] ) == 1 )
				ClearBits( clip, BIT( p ));
		went.ent[went.numents].clip = clip;
		if( turned )
		{
			matrix4x4 matrix;
			Matrix4x4_CreateFromEntity( matrix, e->angles, e->origin, 1.0f );
			Matrix4x4_VectorITransform( matrix, RI.cullorigin, went.ent[went.numents].org );
		}
		else VectorSubtract( RI.cullorigin, e->origin, went.ent[went.numents].org );
		went.ent[went.numents].later = i >= solids;
		if( i < solids )
			world_taken[world_numtaken++] = e;
		else world_later[world_numlater] = e, world_later_at[world_numlater++] = went.numents;
		went.numents++;
	}
	went.relight.count = went.relight.full = 0;
	ents_lit.done = 0;
	went.half = Q_min( went.numents, ( went.numents / 2 + 3 ) & ~3 );
	ents_help.relight.count = ents_help.relight.full = 0, ents_help.done = 0;
	hlglPost( ents_helper, R_EntsHelpJob, 0, 0 );	// (first: the entities' job waits for its half)
	hlglPost( ents_worker, R_EntsJob, 0, 0 );
	hlglPostLate( glow_worker, R_GlowJob, 0, 0 );
	hlglPostLate( relight_worker, R_RelightJob, 0, 0 );
	hlglAtlasWriter( relight_worker );
	world_out = 1, ents_done = 0;
	return true;
}

// Entity n of the job drawn from its runs, as R_DrawBrushModel would.
static void R_KeptBrushDraw( int n )
{
	cl_entity_t *e = went.ent[n].e;
	int mode = e->curstate.rendermode;
	qboolean lit = kept.lit && !r_fullbright->value && WORLDMODEL->lightdata && mode != kRenderTransAdd;

	e->visframe = tr.realframecount;
	if( !went.ent_runs[n] )
		return;
	if( !VectorIsNull( e->angles ))
		R_RotateForEntity( e );
	else if( !VectorIsNull( e->origin ))
		R_TranslateForEntity( e );
	else R_LoadIdentity( );	// (the same matrix, and nothing to load when it is already)
	R_SetRenderMode( e );
	if( gl_polyoffset_bmodels.value )
		GL_PushPolygonOffset( -0.5f, -gl_polyoffset_bmodels.value );
	for( int r = went.ent_at[n], end = r + went.ent_runs[n]; r < end; )
	{
		int g = r;
		while( r < end && went.erun_tex[r] == went.erun_tex[g] ) r++;
		GL_Bind( XASH_TEXTURE0, R_TextureAnimation( went.erun_surf[g] )->gl_texturenum );
		hlglDrawKept( kept.vertices, lit ? kept.light : NULL, went.erun_first + g, went.erun_count + g, r - g );
	}
	if( gl_polyoffset_bmodels.value )
		GL_PopPolygonOffset();
	if( mode != kRenderNormal )
	{
		pglDisable( GL_ALPHA_TEST );
		pglAlphaFunc( GL_GREATER, DEFAULT_ALPHATEST );
		pglDisable( GL_BLEND );
		pglDepthMask( GL_TRUE );
	}
	R_LoadIdentity();
}

// R_TextureAnimation of entity e, on any core.
static texture_t *R_TextureAnimationOf( msurface_t *s, const cl_entity_t *e )
{
	texture_t *base = s->texinfo->texture;
	int reletive, count = 0;

	if( e && e->curstate.frame && base->alternate_anims )
		base = base->alternate_anims;
	if( !base->anim_total )
		return base;
	if( base->name[0] == '-' )
	{
		int tx = (int)(( s->texturemins[0] + ( base->width << 16 )) / base->width ) % MOD_FRAMES;
		int ty = (int)(( s->texturemins[1] + ( base->height << 16 )) / base->height ) % MOD_FRAMES;
		reletive = rtable[tx][ty] % base->anim_total;
	}
	else reletive = (int)( SE_CL_TIME * ( FBitSet( R_GetTexture( base->gl_texturenum )->flags, TF_QUAKEPAL ) ? 10 : 20 )) % base->anim_total;
	while( base->anim_min > reletive || base->anim_max <= reletive )
		if( !( base = base->anim_next ) || ++count > MOD_FRAMES )
			return s->texinfo->texture;
	return base;
}

// The opaque brush entities' draws, made by their worker once its job is done: this core takes the
// room for them and a block of matrices for each that moved; the job fills both.
static struct __attribute__(( aligned( 16 ))) { matrix4x4 worldview; int count, lit, n[WORLD_ENTS]; float *rows[WORLD_ENTS]; uint block[WORLD_ENTS]; } edraw;
static byte ent_drawn[WORLD_ENTS];
static uint R_EntsDrawJob( uint unused0, uint unused1 )
{
	for( int k = 0; k < edraw.count; k++ )
	{
		int n = edraw.n[k];
		cl_entity_t *e = went.ent[n].e;
		if( edraw.rows[k] )	// (R_RotateForEntity's or R_TranslateForEntity's)
		{
			matrix4x4 object, modelview;
			Matrix4x4_CreateFromEntity( object, VectorIsNull( e->angles ) ? vec3_origin : e->angles, e->origin, 1.0f );
			Matrix4x4_ConcatTransforms( modelview, edraw.worldview, object );
			for( int i = 0; i < 12; i++ )
				edraw.rows[k][i] = modelview[i / 4][i % 4];
		}
		for( int r = went.ent_at[n], end = r + went.ent_runs[n]; r < end; )
		{
			int g = r;
			while( r < end && went.erun_tex[r] == went.erun_tex[g] ) r++;
			uint t = R_TextureAnimationOf( went.erun_surf[g], e )->gl_texturenum;
			hlglKeptPut( ents_room, edraw.block[k], R_GetTexture( t > 0 && t < MAX_TEXTURES ? t : tr.defaultTexture )->texnum,
				kept.vertices, edraw.lit ? kept.light : NULL, went.erun_first + g, went.erun_count + g, r - g );
		}
	}
	return 0;
}
static void R_EntsDrawOnWorker( void )
{
	static int off = -1;
	int runs = 0;
	uint vertices = 0;

	if( off < 0 ) off = getenv( "HL_ENTS_DRAW" ) && !strcmp( getenv( "HL_ENTS_DRAW" ), "inline" );
	memset( ent_drawn, 0, sizeof( ent_drawn ));
	edraw.count = 0;
	if( off || ents_worker <= 0 )	// (a polygon offset changes nothing on this GPU)
		return;
	for( int n = 0; n < went.numents; n++ )
		if( !went.ent_slow[n] && !went.ent[n].later && went.ent_runs[n] && went.ent[n].e->curstate.rendermode == kRenderNormal )
			edraw.n[edraw.count++] = n;
	if( !edraw.count )
		return;
	R_LoadIdentity( );
	R_SetRenderMode( went.ent[edraw.n[0]].e );
	uint world_block = gpuMatricesBlock( );
	int kept_n = 0;
	for( int k = 0; k < edraw.count; k++ )
	{
		int n = edraw.n[k];
		cl_entity_t *e = went.ent[n].e;
		float *rows = NULL;
		if(( !VectorIsNull( e->angles ) || !VectorIsNull( e->origin )) && !( rows = gpuModelviewBlock( )))
			continue;	// (no block for it: drawn as before)
		edraw.n[kept_n] = n, edraw.rows[kept_n] = rows, edraw.block[kept_n] = rows ? gpuAddress( rows - 16 ) : world_block;
		kept_n++;
		runs += went.ent_runs[n];
		for( int r = went.ent_at[n]; r < went.ent_at[n] + went.ent_runs[n]; r++ )
			vertices += went.erun_count[r];
	}
	gpuMatricesChanged( );
	edraw.count = kept_n;
	edraw.lit = kept.lit && !r_fullbright->value && WORLDMODEL->lightdata;
	Matrix4x4_Copy( edraw.worldview, RI.worldviewMatrix );
	if( !kept_n || !hlglKeptRoom( ents_room, runs, vertices, edraw.lit ))
	{
		edraw.count = 0;
		return;
	}
	for( int k = 0; k < edraw.count; k++ )
		ent_drawn[edraw.n[k]] = 1, went.ent[edraw.n[k]].e->visframe = tr.realframecount;
	hlglPost( ents_worker, R_EntsDrawJob, 0, 0 );
}

// The light maps a worker built and did not write uploaded, and those of the list it did not build here.
static void R_RelitUpload( surflist_t *l, relit_t *r )
{
	for( int i = 0, k = 0; i < l->count; i++ )
	{
		msurface_t *fa = l->s[i];
		if( k == r->count || r->built[k].s != fa )
		{
			R_CheckLightMap( fa );
			continue;
		}
		if( r->built[k].written )
		{
			k++;
			continue;
		}
		GL_Bind( XASH_TEXTURE0, tr.lightmapTextures[fa->lightmaptexturenum] );
		pglTexSubImage2D( GL_TEXTURE_2D, 0, fa->light_s, fa->light_t, r->built[k].smax, r->built[k].tmax, GL_RGBA, GL_UNSIGNED_BYTE, r->bytes + r->built[k].at );
		k++;
	}
}

// The world drawn from what its worker found, after the solid entities: the GPU draws the passes
// in turn, and the opaque one by depth.
// The brush entities drawn from what their worker found, after the solid entities, and the glows taken.
void R_KeptEntsFinish( void )
{
	if( !world_out || ents_done )
		return;
	tl.sum[7] += tl_now( ) - tl.give;
	rdbg0.n += hlglWait( ents_worker ) * 1000;
	ents_done = 1;
	if( !( tr.framecount & 255 ))
		hlglSay( "hl ents job a frame: entities %u (%u, %u surfaces), lights %u; world walk %u sort %u mark %u nodes %u\n", edbg.ents / 256, edbg.n / 256, edbg.surfs / 256, edbg.light / 256, ( wdbg.walk - wdbg_said.walk ) / 256, ( wdbg.sort - wdbg_said.sort ) / 256, ( wdbg.unused[0] - wdbg_said.unused[0] ) / 256, ( wdbg.unused[1] - wdbg_said.unused[1] ) / 256 ), memset( &edbg, 0, sizeof( edbg )), wdbg_said = wdbg,
		hlglSay( "hl relight a frame: walk %u light %u (%u) write %u ents %u write %u; ents %u draw %u world %u\n", ( rdbg.walk - rdbg_said.walk ) / 256, ( rdbg.light - rdbg_said.light ) / 256, 0,
			( rdbg.write - rdbg_said.write ) / 256, ( rdbg.ents - rdbg_said.ents ) / 256, ( rdbg.ewrite - rdbg_said.ewrite ) / 256, rdbg0.n / 256, rdbg0.draw / 256, rdbg0.world / 256 ), rdbg_said = rdbg, memset( &rdbg0, 0, sizeof( rdbg0 ));
	world_finishing = 1;
	R_EntsDrawOnWorker( );
	for( int n = 0; n < went.numents; n++ )
	{
		cl_entity_t *e = went.ent[n].e;

		RI.currententity = e, RI.currentmodel = e->model;
		if( went.ent_slow[n] )
		{
			R_DrawBrushModel( e );
			continue;
		}
		if( !went.ent[n].later && !ent_drawn[n] )
			R_KeptBrushDraw( n );
	}
	world_finishing = 0;
	RI.currententity = CL_GetEntityByIndex( 0 );
	RI.currentmodel = RI.currententity->model;
}

// The world still to draw: R_KeptWorldFinish at the end of the entities (their opaque pass is drawn first).
qboolean R_KeptWorldLate( void )
{
	return world_out;
}

void R_KeptWorldFinish( void )
{
	if( !world_out )
		return;
	tl.sum[8] += tl_now( ) - tl.give;
	R_KeptEntsFinish( );
	rdbg0.draw += hlglWait( ents_worker ) * 1000, rdbg0.world += hlglWait( world_worker ) * 1000;
	R_WorldJobsWait( );

	memset( gl_lms.lightmap_surfaces, 0, sizeof( gl_lms.lightmap_surfaces ));
	R_ClearSeparate( &draw_fullbrights, fullbright_surfaces );
	R_ClearSeparate( &draw_details, detail_surfaces );
	gl_lms.dynamic_surfaces = NULL;
	pglDisable( GL_ALPHA_TEST );
	pglDisable( GL_BLEND );
	tr.blend = 1.0f;
	VectorCopy( RI.cullorigin, tr.modelorg );
	for( int i = 0; i < world.slow.count; i++ )
	{
		msurface_t *s = world.slow.s[i];
		s->texturechain = s->texinfo->texture->texturechain;
		s->texinfo->texture->texturechain = s;
	}
	for( int i = 0; i < world.sky.count; i++ )
		world.sky.s[i]->texturechain = skychain, skychain = world.sky.s[i];
	R_DrawTextureChains();

	qboolean lit = kept.lit && !r_fullbright->value && WORLDMODEL->lightdata;
	if( world_worker > 0 && hlglKeptRoom( world_room, world.draws, world.vertices, lit ))
		hlglPost( world_worker, R_WorldDrawJob, lit, 0 );
	else for( int k = 0; k < world.numtex; k++ )
	{
		int t = world.tex[k];
		GL_Bind( XASH_TEXTURE0, R_TextureAnimation( world.tex_surf[t] )->gl_texturenum );
		hlglDrawKept( kept.vertices, lit ? kept.light : NULL, world.sorted_first + world.tex_at[t], world.sorted_count + world.tex_at[t], world.tex_runs[t] );
	}
	for( int i = 0; i < world.bright.count; i++ )
	{
		texture_t *t = R_TextureAnimation( world.bright.s[i] );
		R_RenderFullbrightForSurface( world.bright.s[i], t );
		R_RenderDetailsForSurface( world.bright.s[i], t );
	}
	for( int i = 0; i < world.decaled.count; i++ )
		R_RenderDecalsForSurface( world.decaled.s[i], CULL_VISIBLE );
	if( world.overflow || world.sky.full || world.slow.full || world.relight.full || world.decaled.full || world.bright.full || went.relight.full )
		gEngfuncs.Con_Printf( "the world's lists are full: some surfaces are not drawn\n" );

	if( !ENGINE_GET_PARM( PARM_DEV_OVERVIEW ))
	{
		DrawDecalsBatch();
		GL_ResetFogColor();
		R_BlendLightmaps();
		R_RenderFullbrights( R_HasEnabledVBO( ));
		R_RenderDetails( R_HasEnabledVBO() ? 2 : 3 );
		R_DrawTriangleOutlines();
		if( skychain )
			R_DrawSkyBox();
	}
	tr.num_draw_decals = 0;
	skychain = NULL;
	gEngfuncs.R_DrawWorldHull();
	RI.currententity = CL_GetEntityByIndex( 0 );
	RI.currentmodel = RI.currententity->model;
}

void R_KeptEntsFinish( void );
// The entity drawn by R_KeptWorldFinish or here from its runs (true), or to be drawn as before.
static qboolean R_KeptBrushTaken( const cl_entity_t *e )
{
	if( world_finishing )
		return false;
	for( int i = 0; i < world_numtaken; i++ )
		if( world_taken[i] == e )
			return true;
	for( int i = 0; i < world_numlater; i++ )
		if( world_later[i] == e )
		{
			if( world_out && !ents_done )	// (the opaque ones too: the GPU draws them first all the same)
			{
				cl_entity_t *current = RI.currententity;
				model_t *model = RI.currentmodel;
				float blend = tr.blend;
				R_KeptEntsFinish( );
				RI.currententity = current, RI.currentmodel = model, tr.blend = blend;
			}
			if( went.ent_slow[world_later_at[i]] )
				return false;
			R_KeptBrushDraw( world_later_at[i] );
			return true;
		}
	return false;
}

// The fraction of the trace from the eye to a glow sprite that its worker made in the frame before
// (true); a glow new to this frame is traced here.
qboolean R_KeptGlowFraction( const cl_entity_t *e, const vec3_t origin, float *fraction )
{
	for( int i = 0; i < glows_before.count; i++ )
		if( glows_before.e[i] == e )
		{
			*fraction = glows_before.fraction[i];
			return true;
		}
	return false;
}

// ---- the light of the things to draw, found ahead by a worker ----

#define AHEAD_MAX 64
int hlglLightWorker( void );
uint R_LightEpoch( void );
qboolean R_LightKept( const cl_entity_t *ent, const vec3_t origin );
int R_LightKey( const cl_entity_t *ent );
void R_LevelLightAt( const cl_entity_t *ent, const vec3_t origin, vec3_t lightDir, colorVec *plight, vec3_t lightspot, vec3_t lightvec, uint64_t *styles );

static struct __attribute__(( aligned( 16 )))
{
	int	count;
	uint	stamp;
	struct { const cl_entity_t *e; vec3_t origin; } in[AHEAD_MAX];
} ahead;
static struct __attribute__(( aligned( 16 ))) { vec3_t dir, spot, vec; colorVec light; uint64_t styles; int ready; } ahead_out[AHEAD_MAX];
static short ahead_slot[1024 + 256];	// (this core's: an entity's place in ahead, plus one)
static int ahead_worker;

static uint R_LightAheadJob( uint a0, uint a1 )
{
	for( int i = 0; i < ahead.count; i++ )
	{
		const cl_entity_t *e = ahead.in[i].e;
		if( R_LightKept( e, ahead.in[i].origin ))
			continue;	// (this core has it)
		VectorSet( ahead_out[i].dir, 0.0f, 0.0f, FBitSet( e->curstate.effects, EF_INVLIGHT ) ? 1.0f : -1.0f );
		R_LevelLightAt( e, ahead.in[i].origin, ahead_out[i].dir, &ahead_out[i].light, ahead_out[i].spot, ahead_out[i].vec, &ahead_out[i].styles );
		ahead_out[i].ready = 1;
	}
	return 0;
}

// ---- the bones of the studio models to draw, set up ahead by four workers (the client's code) ----

#define BONES_FLOATS ( 48 * 1024 )
int hlglBonesWorker( int i );
int hlglPoll( int k );
extern int ( *SE_StudioBonesAhead )( cl_entity_t *e, int core, double clTime, double clOldTime, float *bones, float *lights, float *rotation, float *prevframe );
extern void ( *SE_StudioAheadReady )( void );
extern int se_studio_no_followers;
extern int ( *SE_StudioBonesTaken )( cl_entity_t *e, double clTime, float *bones, float *lights, int numbones, float *prevframe );

static struct __attribute__(( aligned( 16 )))
{
	int	count;
	double	time, oldtime;
	struct { cl_entity_t *e; int at, numbones; } in[AHEAD_MAX];
} bones;
static struct __attribute__(( aligned( 16 ))) { float frame; int ready; } bones_out[AHEAD_MAX];	// ready 1, or 2: not set up
static float bones_pool[BONES_FLOATS] __attribute__(( aligned( 16 )));
static float bones_rotation[4][3][4] __attribute__(( aligned( 16 )));
static float bones_rot[AHEAD_MAX][3][4] __attribute__(( aligned( 16 )));	// each entity's, kept for its draw
static short bones_slot[1024 + 256];	// (this core's)
static int bones_posted;

static uint R_BonesAheadJob( uint core, uint unused )
{
	for( int i = core - 1; i < bones.count; i += 4 )
	{
		float *b = bones_pool + bones.in[i].at;
		int ok = SE_StudioBonesAhead( bones.in[i].e, core, bones.time, bones.oldtime, b, b + bones.in[i].numbones * 12, &bones_rotation[core - 1][0][0], &bones_out[i].frame );
		if( ok )
			memcpy( bones_rot[i], bones_rotation[core - 1], sizeof( bones_rot[i] ));
		bones_out[i].ready = ok ? 1 : 2;
	}
	return 0;
}

// The bones of the model being drawn where the worker left them, if they came from there (its
// draw's jobs read them there: the pool stays until the next frame's are given, after the frame).
const float *se_bones_source;
int se_bones_source_n;
// The bones the worker set up for the entity copied in (true), if it has set them up yet.
static int R_BonesAheadTaken( cl_entity_t *e, double time, float *bone, float *light, int numbones, float *prevframe )
{
	int i = bones_slot[R_LightKey( e )] - 1;

	if( i < 0 || bones.in[i].e != e || bones.time != time || bones.in[i].numbones != numbones )
		return 0;
	// not set up yet: its worker is waited for, sooner done than setting them up here
	while( !*(volatile int *)&bones_out[i].ready && !hlglPoll( hlglBonesWorker( i & 3 )))
		;
	if( bones_out[i].ready != 1 )
		return 0;
	se_bones_source = bones_pool + bones.in[i].at, se_bones_source_n = numbones;
	memcpy( bone, bones_pool + bones.in[i].at, sizeof( float ) * 12 * numbones );
	memcpy( light, bones_pool + bones.in[i].at + 12 * numbones, sizeof( float ) * 12 * numbones );
	*prevframe = bones_out[i].frame;
	return 1;
}

// The rotation the worker set the entity's bones up with (true): its draw then makes none
// (StudioSetUpTransform's, from the same state and time).
extern int ( *SE_StudioRotationTaken )( cl_entity_t *e, double clTime, float *rotation );
static int R_RotationAheadTaken( cl_entity_t *e, double time, float *rotation )
{
	int i = bones_slot[R_LightKey( e )] - 1;

	if( i < 0 || bones.in[i].e != e || bones.time != time )
		return 0;
	while( !*(volatile int *)&bones_out[i].ready && !hlglPoll( hlglBonesWorker( i & 3 )))
		;
	if( bones_out[i].ready != 1 )
		return 0;
	memcpy( rotation, bones_rot[i], sizeof( bones_rot[i] ));
	return 1;
}

static void R_BonesAheadGive( void )
{
	static int off = -1;

	if( off < 0 ) off = getenv( "HL_BONES" ) && !strcmp( getenv( "HL_BONES" ), "inline" );
	for( int k = 0; bones_posted && k < 4; k++ )
		hlglWait( hlglBonesWorker( k ));
	bones_posted = 0;
	if( bones.count )
		memset( bones_slot, 0, sizeof( bones_slot ));	// (whole: the entities may be copies written again since)
	bones.count = 0;
	if( off || !SE_StudioBonesAhead || hlglBonesWorker( 0 ) <= 0 )
		return;
	if( !SE_StudioBonesTaken )
	{
		SE_StudioAheadReady( );
		SE_StudioBonesTaken = R_BonesAheadTaken;
		if( !getenv( "HL_ROTATION" ) || strcmp( getenv( "HL_ROTATION" ), "inline" ))	// (HL_ROTATION=inline: made at each draw)
			SE_StudioRotationTaken = R_RotationAheadTaken;
	}
	bones.time = SE_CL_TIME, bones.oldtime = SE_CL_OLDTIME;
	for( int i = 0, at = 0; i < ahead.count && bones.count < AHEAD_MAX; i++ )
	{
		cl_entity_t *e = (cl_entity_t *)ahead.in[i].e;
		studiohdr_t *hdr = (studiohdr_t *)e->model->cache.data;
		if( !hdr || e->player || at + hdr->numbones * 24 > BONES_FLOATS || bones_slot[R_LightKey( e )] )
			continue;
		bones.in[bones.count].e = e, bones.in[bones.count].at = at, bones.in[bones.count].numbones = hdr->numbones;
		bones_out[bones.count].ready = 0;
		at += hdr->numbones * 24;
		bones_slot[R_LightKey( e )] = ++bones.count;
	}
	for( int k = 0; k < 4 && k < bones.count; k++ )
		hlglPost( hlglBonesWorker( k ), R_BonesAheadJob, k + 1, 0 );
	bones_posted = 1;
}

// The studio models to draw given to a worker to find their light (CL_EmitEntities' end).
void R_LightAheadGive( void )
{
	static int off = -1;

	if( off < 0 ) off = getenv( "HL_LIGHT" ) && !strcmp( getenv( "HL_LIGHT" ), "inline" );
	if( ahead_worker )
		hlglWait( ahead_worker ), ahead_worker = 0;
	if( ahead.count )
		memset( ahead_slot, 0, sizeof( ahead_slot ));	// (whole: the entities may be copies written again since)
	ahead.count = 0;
	se_studio_no_followers = 0;
	if( off || !tr.draw_list || !WORLDMODEL || !WORLDMODEL->lightdata || r_fullbright->value || hlglLightWorker( ) <= 0 )
		return;
	int followers = 0, all = tr.draw_list->num_solid_entities + tr.draw_list->num_trans_entities;
	for( int i = 0, solids = tr.draw_list->num_solid_entities; i < all && ahead.count < AHEAD_MAX; i++ )
	{
		cl_entity_t *e = i < solids ? tr.draw_list->solid_entities[i] : tr.draw_list->trans_entities[i - solids];
		followers |= e->curstate.movetype == MOVETYPE_FOLLOW;
		if( !e->model || e->model->type != mod_studio || FBitSet( e->curstate.effects, EF_FULLBRIGHT ) || ahead_slot[R_LightKey( e )] )
			continue;
		ahead.in[ahead.count].e = e;
		VectorCopy( e->origin, ahead.in[ahead.count].origin );
		ahead_out[ahead.count].ready = 0;
		ahead_slot[R_LightKey( e )] = ++ahead.count;
	}
	se_studio_no_followers = !followers && ahead.count < AHEAD_MAX;	// (else the list was not seen whole)
	if( !ahead.count )
		return;
	ahead.stamp = R_LightEpoch( );
	ahead_worker = hlglLightWorker( );
	hlglPost( ahead_worker, R_LightAheadJob, 0, 0 );
	R_BonesAheadGive( );
}

// What the worker found for the entity where it is (true), if it has found it yet.
qboolean R_LightAheadOf( const cl_entity_t *ent, const vec3_t origin, colorVec *light, vec3_t dir, vec3_t spot, vec3_t vec, uint64_t *styles, uint *stamp )
{
	int i = ahead_slot[R_LightKey( ent )] - 1;

	if( i < 0 || ahead.in[i].e != ent || !VectorCompare( ahead.in[i].origin, origin ) || !ahead_out[i].ready )
		return false;
	*light = ahead_out[i].light;
	VectorCopy( ahead_out[i].dir, dir );
	VectorCopy( ahead_out[i].spot, spot );
	VectorCopy( ahead_out[i].vec, vec );
	*styles = ahead_out[i].styles, *stamp = ahead.stamp;
	return true;
}

// ---- the frame drawn on a worker a frame behind (rnd_shaderemu.c): what it draws, copied ----

#define SCENE_COPIES 256
extern draw_list_t *se_add_list;
extern dlight_t se_render_dlights[], se_render_elights[];
static draw_list_t scene_list;	// the worker's, of the copies
static cl_entity_t scene_ents[SCENE_COPIES + 1];	// the last the view model's
static struct { cl_entity_t *live; int muzzle; } scene_of[SCENE_COPIES + 1];
static int scene_count;
static cl_entity_t *scene_viewent;
static dlight_t *scene_dlights, *scene_elights;

// All of an entity the drawing reads: not its baseline, its state before or its position history.
static void R_SceneCopyTo( cl_entity_t *c, cl_entity_t *e, int slot )
{
	c->index = e->index, c->player = e->player;
	memcpy( &c->curstate, &e->curstate, sizeof( c->curstate ));
	memcpy( &c->mouth, &e->mouth, sizeof( cl_entity_t ) - offsetof( cl_entity_t, mouth ));
	scene_of[slot].live = e, scene_of[slot].muzzle = FBitSet( e->curstate.effects, EF_MUZZLEFLASH );
}
static cl_entity_t *R_SceneCopy( cl_entity_t *e )
{
	if( scene_count >= SCENE_COPIES )
		return e;
	R_SceneCopyTo( &scene_ents[scene_count], e, scene_count );
	return &scene_ents[scene_count++];
}

static void R_ScenePipe( int on )
{
	if( on )
	{
		se_add_list = tr.draw_list, tr.draw_list = &scene_list;
		scene_viewent = tr.viewent, tr.viewent = &scene_ents[SCENE_COPIES];
		scene_dlights = gp_dlights, gp_dlights = se_render_dlights;
		scene_elights = tr.elights, tr.elights = se_render_elights;
		return;
	}
	if( !se_add_list )
		return;
	tr.draw_list = se_add_list, se_add_list = NULL;
	tr.viewent = scene_viewent, gp_dlights = scene_dlights, tr.elights = scene_elights;
}

// This core's list of the frame to draw, copied for the worker with its entities.
static void R_SceneGive( void )
{
	const draw_list_t *from = se_add_list;

	scene_count = 0;
	scene_list.num_solid_entities = from->num_solid_entities;
	scene_list.num_trans_entities = from->num_trans_entities;
	scene_list.num_beam_entities = from->num_beam_entities;
	for( int i = 0; i < from->num_solid_entities; i++ )
		scene_list.solid_entities[i] = R_SceneCopy( from->solid_entities[i] );
	for( int i = 0; i < from->num_trans_entities; i++ )
		scene_list.trans_entities[i] = R_SceneCopy( from->trans_entities[i] );
	for( int i = 0; i < from->num_beam_entities; i++ )
		scene_list.beam_entities[i] = R_SceneCopy( from->beam_entities[i] );
	R_SceneCopyTo( &scene_ents[SCENE_COPIES], scene_viewent, SCENE_COPIES );
}

// What the drawing wrote of its copies, back where they came from: attachments, the frame it
// was at, a muzzle flash shown (only an entity of the client's: a temporary one may be another now).
static void R_SceneTakeOne( cl_entity_t *c, int slot, cl_entity_t *( *by_index )( int ))
{
	cl_entity_t *e = scene_of[slot].live;

	if( slot < SCENE_COPIES && by_index( c->index ) != e )
		return;
	memcpy( e->attachment, c->attachment, sizeof( e->attachment ));
	e->latched.prevframe = c->latched.prevframe;
	if( scene_of[slot].muzzle && !FBitSet( c->curstate.effects, EF_MUZZLEFLASH ))
		ClearBits( e->curstate.effects, EF_MUZZLEFLASH );
}
static void R_SceneTaken( cl_entity_t *( *by_index )( int ))
{
	for( int i = 0; i < scene_count; i++ )
		R_SceneTakeOne( &scene_ents[i], i, by_index );
	R_SceneTakeOne( &scene_ents[SCENE_COPIES], SCENE_COPIES, by_index );
}

// The entity a copy was made of.
static const cl_entity_t *R_SceneLive( const cl_entity_t *e )
{
	if( e >= scene_ents && e <= &scene_ents[SCENE_COPIES] )
		return scene_of[e - scene_ents].live;
	return e;
}

extern void ( *SE_ScenePipe )( int on ), ( *SE_SceneGive )( void ), ( *SE_SceneTaken )( cl_entity_t *( *by_index )( int ));
extern const cl_entity_t *( *SE_SceneLive )( const cl_entity_t *e );
static void R_SceneSet( void )
{
	SE_ScenePipe = R_ScenePipe, SE_SceneGive = R_SceneGive, SE_SceneTaken = R_SceneTaken, SE_SceneLive = R_SceneLive;
}
