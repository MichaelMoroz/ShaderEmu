/*
rnd_shaderemu.c - the frame's 3D drawing on a worker core (docs/games/halflife.md)

The default (HL_RENDER=worker): frame n's GL_RenderFrame runs on a worker of its own while this core
makes frame n + 1, from copies of what it draws (gl_kept.c), then frame n's 2D that this core
recorded (HL_2D=core: this core draws it at the settle, and frame n is shown with frame n + 1's 2D).
HL_RENDER=wait: on the worker, this core waiting for it. HL_RENDER=core: the drawing is this core's.
*/
#include <setjmp.h>
#include <stddef.h>
#include "common.h"
#include "client.h"
#include "mcw.h"
#include "se_server.h"

char se_render_stack[SE_RENDER_STACK] __attribute__(( aligned( 16 )));
double se_render_time, se_render_oldtime;	// the client's times of the frame the worker draws
int hlglRenderWorker( void );
void hlglRingsStart( void ), hlglRingsStop( void ), hlglRenderJobsWait( void ), hlglRingsServe( void ), hlglJobsWait( void );
unsigned hlglRenderSum( void );
uint32_t se_call_on_stack( uint32_t ( *fn )( uint32_t ), uint32_t arg, void *stack_top );
void SE_LightsGive( void ), SE_LightsTaken( void ), SE_EfxGive( void ), SE_EfxTaken( void ), SE_LightStylesGive( void );
extern void ( *SE_LightAhead )( void );

// The renderer's (gl_kept.c): the drawing's lists copied, and what it wrote of the copies taken back.
void ( *SE_ScenePipe )( int on ), ( *SE_SceneGive )( void ), ( *SE_SceneAhead )( void ), ( *SE_SceneTaken )( cl_entity_t *( *by_index )( int ));
const cl_entity_t *( *SE_SceneLive )( const cl_entity_t *e );

static struct { volatile uint seq; uint32_t ( *fn )( void * ); void *args; uint unused; } ask __attribute__(( aligned( 16 )));	// the worker's
static struct { volatile uint seq; uint32_t result; uint unused[2]; } answer __attribute__(( aligned( 16 )));	// this core's
static struct { uint used, lost, failed, sum; char error[1024]; char kept[4096]; } work __attribute__(( aligned( 16 )));	// the worker's
static struct SE_APART { int count, unused[3]; struct { mstudioevent_t event; const cl_entity_t *e; } kept[32]; } events;	// the worker's
static struct SE_APART { jmp_buf at; } abort_of;
void *se_render_pm, *SE_VisTraceOwn( void );	// the drawing's playermove for CL_VisTraceLine (cl_pmove.c)
int se_render_light_level[4] SE_APART = { -1 };	// the player's light the drawing found (ref_common.c), a texel of its own
// Calls of the renderer made when the drawing is settled, in order: a0 to a7 and the caller's first
// four stack words (ilp32: floats too), replayed by se_ref_replay.
#define LATER 1024	// (the opening titles record some 140 a frame)
typedef struct SE_APART { int count, unused[3]; struct { uint32_t w[12]; uint8_t fn; } at[LATER]; } later_t;
void se_ref_replay( void *fn, const uint32_t *w );
extern void *se_ref_real[];
static later_t later_of[2], *rec = later_of;	// this core records into one while the drawing plays the other
static int play_2d, hand_2d;	// the frame's 2D drawn by its own drawing, after its 3D (HL_2D=core: by this core at the settle)
static struct SE_APART { ref_viewpass_t rvp; lightstyle_t *styles; later_t *play; } given;	// this core's, read by the worker
static struct SE_APART { ref_viewpass_t rvp; lightstyle_t *styles; int have; } next;	// this core's
static int mode = -1, worker, out, piped, sums = -1;
static uint settles;
static int ref_count;
static inline uint32_t rd_time( void ) { uint32_t v; __asm__ volatile( "rdtime %0" : "=r"( v )); return v; }
static inline uint32_t rd_ins( void ) { uint32_t v; __asm__ volatile( "rdinstret %0" : "=r"( v )); return v; }
static struct SE_APART { uint32_t start, end, ins, waits; int most; } job_tl;
unsigned hlglRingWaits( int *most );	// the worker's
extern unsigned mcw_faults, mcw_calls;
static struct { uint32_t give, sum[8], n, longest, asks, faults, calls, lw; int lm; } tl;	// this core's: from the give to the job's start, end, the settle, its end

int SE_RenderWho( void ) { return SE_OnRender(); }
int SE_RenderOut( void ) { return out; }
int SE_RenderPiped( void ) { return piped; }

static void setup( void )
{
	const char *how = getenv( "HL_RENDER" ) ? getenv( "HL_RENDER" ) : "worker";

	mode = 0;
	sums = getenv( "HLGL_SUM" ) && atoi( getenv( "HLGL_SUM" )) == 2;
	if(( Q_strcmp( how, "worker" ) && Q_strcmp( how, "wait" )) || ( worker = hlglRenderWorker( )) <= 0 )
		return;
	mcw_touch( se_render_stack, SE_RENDER_STACK );
	mcw_touch( &work, sizeof( work ));
	mcw_touch( &ask, sizeof( ask ));
	mcw_touch( &events, sizeof( events ));
	if(( se_render_pm = SE_VisTraceOwn( )))
		mcw_touch( se_render_pm, sizeof( playermove_t ));
	mode = Q_strcmp( how, "wait" ) ? 2 : 1;
	play_2d = mode == 2 && !( getenv( "HL_2D" ) && !Q_strcmp( getenv( "HL_2D" ), "core" ));
	Con_Printf( "the frame's drawing is on worker core %d%s\n", worker, mode == 2 ? ", a frame behind" : "" );
}

uint32_t SE_RenderOnCore0( uint32_t ( *fn )( void * ), void *args )
{
	if( !SE_OnRender( ))
		return fn( args );
	ask.fn = fn, ask.args = args;
	mc_publish( );	// (the call before its number: mc.h)
	ask.seq = ask.seq + 1;
	while( answer.seq != ask.seq )
		mc_end_pass();
	return answer.result;
}

void SE_RenderServe( void )
{
	if( !out )
		return;
	if( ask.seq != answer.seq )
	{
		answer.result = ask.fn( ask.args );
		answer.seq = ask.seq;
	}
	mcw_done( worker );	// (a page or a system call it stopped at, served)
}

void SE_RenderKeepPrint( const char *text )
{
	uint n = Q_strlen( text ) + 1;

	if( work.used + n > sizeof( work.kept ))
	{
		work.lost++;
		return;
	}
	memcpy( work.kept + work.used, text, n );
	work.used += n;
}

void SE_RenderAbort( const char *text, int fatal )
{
	Q_strncpy( work.error, text, sizeof( work.error ));
	work.failed = fatal ? 2 : 1;
	longjmp( abort_of.at, 1 );
}

// A studio model's event on the worker kept (true), to be made by this core when the frame is taken.
int SE_RenderKeepEvent( const mstudioevent_t *event, const cl_entity_t *e )
{
	if( !SE_OnRender( ) || !piped )
		return 0;
	if( events.count < 32 )
		events.kept[events.count].event = *event, events.kept[events.count++].e = e;
	return 1;
}

#define REF_INDEX( name ) ( offsetof( ref_interface_t, name ) / sizeof( void * ))
extern void *se_ref_real[];

static uint32_t job_body( uint32_t unused )
{
	job_tl.start = rd_time(), job_tl.ins = rd_ins();
	if( setjmp( abort_of.at ))
	{
		hlglRenderJobsWait();
		return 1;
	}
	if( piped )
	{
		// (this core's frame made these before its drawing; they are the drawing's now)
		if( given.styles )
			(( void ( * )( lightstyle_t * ))se_ref_real[REF_INDEX( CL_RunLightStyles )] )( given.styles );
		SE_LightStylesGive();
		if( SE_LightAhead )
			SE_LightAhead();
	}
	ref.dllFuncs.GL_RenderFrame( &given.rvp );
	for( int i = 0; given.play && i < given.play->count; i++ )
		se_ref_replay( se_ref_real[given.play->at[i].fn], given.play->at[i].w );
	if( sums )	// (else this core's ring jobs are waited for by the frame's swap)
		hlglRenderJobsWait(), work.sum = hlglRenderSum();
	job_tl.ins = rd_ins() - job_tl.ins, job_tl.end = rd_time();
	if( ref_count > 0 )
		job_tl.waits = hlglRingWaits( &job_tl.most );
	return 0;
}

void se_ref_replay( void *fn, const uint32_t *w );
__asm__( ".text\n.balign 4\n.globl se_ref_replay\nse_ref_replay:\n"
	"	addi sp, sp, -32\n	sw ra, 28(sp)\n	mv t0, a0\n	mv t1, a1\n"
	"	lw t2, 32(t1)\n	sw t2, 0(sp)\n	lw t2, 36(t1)\n	sw t2, 4(sp)\n"
	"	lw t2, 40(t1)\n	sw t2, 8(sp)\n	lw t2, 44(t1)\n	sw t2, 12(sp)\n"
	"	lw a0, 0(t1)\n	lw a1, 4(t1)\n	lw a2, 8(t1)\n	lw a3, 12(t1)\n"
	"	lw a4, 16(t1)\n	lw a5, 20(t1)\n	lw a6, 24(t1)\n	lw a7, 28(t1)\n"
	"	jalr t0\n	lw ra, 28(sp)\n	addi sp, sp, 32\n	ret\n" );

static uint32_t render_job( uint32_t a0, uint32_t a1 )
{
	return se_call_on_stack( job_body, 0, se_render_stack + SE_RENDER_STACK );
}

// The drawing out on its worker done, what it kept made here, its lines printed and its error raised.
void SE_RenderSettle( void )
{
	if( !out )
		return;
	uint32_t settle = rd_time();
	while( !mcw_done( worker ))
	{
		SE_ServerServe();
		hlglRingsServe();
		if( piped && cls.state == ca_active )
			S_ExtraUpdate();	// (what the frame's start and 2D mix after it is then short)
		mcw_rest_for( &worker, 1 );
	}
	out = 0;
	if( se_render_light_level[0] >= 0 )
		cl.local.light_level = se_render_light_level[0], se_render_light_level[0] = -1;
	if( !hand_2d )
	{
		for( int i = 0; i < rec->count; i++ )
			se_ref_replay( se_ref_real[rec->at[i].fn], rec->at[i].w );
		rec->count = 0;
	}
	if( tl.give )
		tl.sum[0] += job_tl.start - tl.give, tl.sum[1] += job_tl.end - tl.give, tl.sum[2] += settle - tl.give, tl.sum[3] += rd_time() - tl.give, tl.sum[6] += job_tl.ins, tl.n++;
	{ uint32_t now = rd_time( ), end = job_tl.end;
	  if( end - tl.give > tl.longest ) tl.longest = end - tl.give, tl.lw = job_tl.waits, tl.lm = job_tl.most; tl.sum[4] += end > settle ? end - settle : 0, tl.sum[5] += now - ( end > settle ? end : settle ); }
	if( piped )
	{
		SE_SceneTaken( CL_GetEntityByIndex );
		SE_EfxTaken();
		SE_LightsTaken();
		for( int i = 0; i < events.count; i++ )
			clgame.dllFuncs.pfnStudioEvent( &events.kept[i].event, SE_SceneLive( events.kept[i].e ));
		events.count = 0;
	}
	if( sums )
		fprintf( stderr, "hlgl: at %.3f %08x\n", piped ? se_render_time : cl.time, work.sum );
	for( uint at = 0; at < work.used; at += Q_strlen( work.kept + at ) + 1 )
		Sys_Print( work.kept + at );
	work.used = 0;
	if( work.lost )
		Con_Printf( "(%u of the drawing's lines lost)\n", work.lost ), work.lost = 0;
	if( work.failed )
	{
		int fatal = work.failed == 2;
		work.failed = 0;
		if( fatal ) Sys_Error( "%s", work.error );
		else Host_Error( "%s", work.error );
	}
}

// The frame a frame behind given out, once this core has shown the one before (SCR_UpdateScreen's end).
void SE_RenderGive( void )
{
	if( !piped || !next.have )
		return;
	SE_RenderSettle();
	next.have = 0;
	hlglRenderJobsWait();	// (the late ones too, which may read the copies)
	SE_SceneGive();
	SE_LightsGive();
	SE_EfxGive();
	se_render_time = cl.time, se_render_oldtime = cl.oldtime;
	given.rvp = next.rvp, given.styles = next.styles, next.styles = NULL;
	given.play = hand_2d && rec->count ? rec : NULL, hand_2d = 0;
	if( given.play )
		rec = &later_of[rec == later_of], rec->count = 0;
	if( tl.give && tl.n ) tl.sum[7] += rd_time() - tl.give;
	tl.give = rd_time();
	mcw_post( worker, render_job, 0, 0 );
	out = 1;
}

// No frame out, and the drawing this core's: a level ends, the client is cleared.
void SE_RenderStop( void )
{
	SE_RenderSettle();
	if( piped )
		SE_ScenePipe( 0 ), piped = 0;
	next.have = 0, next.styles = NULL;
	if( hand_2d )
		hand_2d = 0, rec->count = 0;	// (the swap's 2D is not drawn: no frame was given)
	hlglRingsStop();
}

// Before a level is changed, loaded or left: the server's frame, the drawing and their jobs done,
// the workers free (a level's data is freed and read anew next, the next map inflated on them).
void hlglJobsWaitAll( void );
int SE_ServerBusy( void );
void SE_LevelSettle( void )
{
	int server = SE_ServerBusy( ), drawing = out && !mcw_done( worker );
	SE_ServerSettle();
	SE_RenderStop();
	hlglJobsWaitAll();
	Con_Reportf( "a level goes: the server's frame was %s, the drawing %s\n", server ? "out" : "done", drawing ? "out" : out ? "done" : "none" );
}

// GL_RenderFrame, on the drawing's worker when it has one.
void SE_RenderFrame( const ref_viewpass_t *rvp )
{
	if( mode < 0 )
		setup();
	if( mode <= 0 || cls.state != ca_active || ( mode == 2 && !SE_ScenePipe ))
	{
		SE_RenderStop();
		GL_RenderFrame( rvp );
		if( sums )
			hlglJobsWait(), fprintf( stderr, "hlgl: at %.3f %08x\n", cl.time, hlglRenderSum( ));
		return;
	}
	hlglRingsStart();
	VectorCopy( rvp->vieworigin, refState.vieworg );
	VectorCopy( rvp->viewangles, refState.viewangles );
	if( mode == 1 )
	{
		given.rvp = *rvp, given.styles = NULL;
		mcw_post( worker, render_job, 0, 0 );
		out = 1;
		SE_RenderSettle();
		return;
	}
	if( !piped )
		SE_ScenePipe( 1 ), piped = 1;
	next.rvp = *rvp, next.have = 1;
}

// ---- the renderer's functions as this core calls them while the drawing is out ----

#define REF_THUNKS 100
void *se_ref_real[REF_THUNKS];
extern void *se_ref_thunks[REF_THUNKS];
static uint ref_calls[REF_THUNKS], settled_by[REF_THUNKS];
static int ref_window, ref_count = -1, ref_frames;

// Each thunk asks se_ref_guard first: 0 and the call is not made (it returns 0).
#define SE_THUNK( n ) ".balign 4\nse_ref_thunk_" #n ":\n" \
	"	addi sp, sp, -48\n	sw a0, 0(sp)\n	sw a1, 4(sp)\n	sw a2, 8(sp)\n	sw a3, 12(sp)\n" \
	"	sw a4, 16(sp)\n	sw a5, 20(sp)\n	sw a6, 24(sp)\n	sw a7, 28(sp)\n	sw ra, 32(sp)\n" \
	"	mv a1, a0\n	mv a2, sp\n	li a0, " #n "\n	call se_ref_guard\n	mv t1, a0\n" \
	"	lw a0, 0(sp)\n	lw a1, 4(sp)\n	lw a2, 8(sp)\n	lw a3, 12(sp)\n	lw a4, 16(sp)\n" \
	"	lw a5, 20(sp)\n	lw a6, 24(sp)\n	lw a7, 28(sp)\n	lw ra, 32(sp)\n	addi sp, sp, 48\n" \
	"	beqz t1, 1f\n	la t0, se_ref_real\n	lw t0, " #n "*4(t0)\n	jr t0\n1:\n	li a0, 0\n	ret\n"
#define SE_THUNKS( a ) SE_THUNK( a##0 ) SE_THUNK( a##1 ) SE_THUNK( a##2 ) SE_THUNK( a##3 ) SE_THUNK( a##4 ) \
	SE_THUNK( a##5 ) SE_THUNK( a##6 ) SE_THUNK( a##7 ) SE_THUNK( a##8 ) SE_THUNK( a##9 )
#define SE_WORD( n ) "	.word se_ref_thunk_" #n "\n"
#define SE_WORDS( a ) SE_WORD( a##0 ) SE_WORD( a##1 ) SE_WORD( a##2 ) SE_WORD( a##3 ) SE_WORD( a##4 ) \
	SE_WORD( a##5 ) SE_WORD( a##6 ) SE_WORD( a##7 ) SE_WORD( a##8 ) SE_WORD( a##9 )
__asm__( ".text\n"
	SE_THUNKS( ) SE_THUNKS( 1 ) SE_THUNKS( 2 ) SE_THUNKS( 3 ) SE_THUNKS( 4 )
	SE_THUNKS( 5 ) SE_THUNKS( 6 ) SE_THUNKS( 7 ) SE_THUNKS( 8 ) SE_THUNKS( 9 )
	".data\n.balign 4\n.globl se_ref_thunks\nse_ref_thunks:\n"
	SE_WORDS( ) SE_WORDS( 1 ) SE_WORDS( 2 ) SE_WORDS( 3 ) SE_WORDS( 4 )
	SE_WORDS( 5 ) SE_WORDS( 6 ) SE_WORDS( 7 ) SE_WORDS( 8 ) SE_WORDS( 9 )
	".text\n" );

// What this core calls of the renderer while the frame before is drawn: the scene's additions go
// to its own list, the light styles to the next drawing, and anything else waits for it first.
enum { REF_SETTLE, REF_THROUGH, REF_LATER, REF_STYLES };
static uint8_t ref_kind[REF_THUNKS];	// (SE_RefGuard's)

int se_ref_guard( int i, void *a0, const uint32_t *frame )
{
	int kind = ref_kind[i];

	if( !piped || kind == REF_THROUGH )
	{
		if( ref_window && !SE_OffCore0( ) && !mcw_on_worker( ))
			ref_calls[i]++;
		return 1;
	}
	if( SE_OffCore0( ) || mcw_on_worker( ))
		return 1;
	if( ref_window )
		ref_calls[i]++;
	if( kind == REF_STYLES )
	{
		next.styles = a0;
		return 0;
	}
	if( !out )
		return 1;
	if( kind == REF_LATER && rec->count < LATER )
	{
		// a render mode or colour the same as the last recorded, with no other state call between, is left out
		static int mode_at = -1, colour_at = -1;
		if( !rec->count )
			mode_at = colour_at = -1;
		if( i == REF_INDEX( GL_SetRenderMode ))
		{
			if( mode_at >= 0 && rec->at[mode_at].w[0] == frame[0] )
				return 0;
			mode_at = rec->count, colour_at = -1;
		}
		else if( i == REF_INDEX( Color4ub ))
		{
			const uint32_t *c = colour_at >= 0 ? rec->at[colour_at].w : NULL;
			if( c && c[0] == frame[0] && c[1] == frame[1] && c[2] == frame[2] && c[3] == frame[3] )
				return 0;
			colour_at = rec->count;
		}
		else if( i != REF_INDEX( R_DrawStretchPic ))
			mode_at = colour_at = -1;
		uint32_t *w = rec->at[rec->count].w;
		w[0] = frame[0], w[1] = frame[1], w[2] = frame[2], w[3] = frame[3], w[4] = frame[4], w[5] = frame[5];
		w[6] = frame[6], w[7] = frame[7], w[8] = frame[12], w[9] = frame[13], w[10] = frame[14], w[11] = frame[15];
		rec->at[rec->count++].fn = i;
		return 0;
	}
	settles++, settled_by[i]++;
	if( i == REF_INDEX( R_EndFrame ) && next.have )
	{
		if( play_2d )
			hand_2d = 1;	// (left for the frame's own drawing, given next)
		SE_SceneAhead();	// (the next frame's copies while this one still draws: 17k less between its end and the give)
	}
	SE_RenderSettle();
	return 1;
}

// The renderer's table, each function called through its thunk.
void SE_RefGuard( void *table, size_t bytes )
{
	void **fn = table;
	int n = (int)( bytes / sizeof( void * ));

	if( n > REF_THUNKS )
	{
		Con_Printf( "the renderer's table has %d functions, %d thunks: not guarded\n", n, REF_THUNKS );
		return;
	}
	for( int i = 0; i < n; i++ )
		if(( se_ref_real[i] = fn[i] ))
			fn[i] = se_ref_thunks[i];
	// what of it is recorded while the drawing is out, or read, or made the next drawing's; the rest settles
	int later_2d = !getenv( "HL_LATER" ) || atoi( getenv( "HL_LATER" ));	// HL_LATER=0: the 2D drawn after a settle
	static const int later[] = { REF_INDEX( R_BeginFrame ), REF_INDEX( R_Set2DMode ), REF_INDEX( GL_BackendStartFrame ),
		REF_INDEX( GL_BackendEndFrame ), REF_INDEX( R_ClearScreen ) };
	static const int later2d[] = { REF_INDEX( R_AllowFog ), REF_INDEX( GL_SetRenderMode ), REF_INDEX( R_Set2DOffset ),
		REF_INDEX( R_DrawStretchPic ), REF_INDEX( FillRGBA ), REF_INDEX( Color4ub ), REF_INDEX( R_ShowTextures ) };
	static const int through[] = { REF_INDEX( RefGetParm ), REF_INDEX( R_AddEntity ), REF_INDEX( R_ClearScene ), REF_INDEX( Mod_GetCurrentVis ) };
	memset( ref_kind, REF_SETTLE, sizeof( ref_kind ));
	for( int k = 0; k < (int)ARRAYSIZE( later ); k++ ) ref_kind[later[k]] = REF_LATER;
	for( int k = 0; later_2d && k < (int)ARRAYSIZE( later2d ); k++ ) ref_kind[later2d[k]] = REF_LATER;
	for( int k = 0; k < (int)ARRAYSIZE( through ); k++ ) ref_kind[through[k]] = REF_THROUGH;
	ref_kind[REF_INDEX( CL_RunLightStyles )] = REF_STYLES;
	if( !getenv( "HL_RENDER_CALLS" ))	// (a call that goes through needs no thunk but to be counted: R_AddEntity, 50 a frame, 80 instructions each)
		for( int k = 0; k < (int)ARRAYSIZE( through ); k++ )
			if( se_ref_real[through[k]] )
				fn[through[k]] = se_ref_real[through[k]];
}

// Between a frame's end and the next's drawing: HL_RENDER_CALLS=1 counts what this core calls of
// the renderer there, and how often it waited for the drawing, every 256 frames.
void SE_RenderWindow( int open )
{
	if( ref_count < 0 )
		ref_count = getenv( "HL_RENDER_CALLS" ) != NULL;
	if( !ref_count || cls.state != ca_active )
		return;
	ref_window = open;
	if( open && ++ref_frames % 32 == 0 )
	{
		fprintf( stderr, "ref calls in 256 frames' windows (%u waits):", settles );
		for( int i = 0; i < REF_THUNKS; i++ )
			if( ref_calls[i] )
				fprintf( stderr, " %d:%u", i, ref_calls[i] ), ref_calls[i] = 0;
		fprintf( stderr, " settled by" );
		for( int i = 0; i < REF_THUNKS; i++ )
			if( settled_by[i] )
				fprintf( stderr, " %d:%u", i, settled_by[i] ), settled_by[i] = 0;
		fprintf( stderr, "\n" );
		settles = 0;
		if( tl.n )
			fprintf( stderr, "render timeline (in the clock's units after the give): start %u end %u; settle %u to %u (waiting for the job %u, after its end %u; the longest job %u, its ring waits %u most for %d; %u asks, the last of %x; %u faults, %u calls served; %u instructions; frames of %u)\n",
				tl.sum[0] / tl.n, tl.sum[1] / tl.n, tl.sum[2] / tl.n, tl.sum[3] / tl.n, tl.sum[4] / tl.n, tl.sum[5] / tl.n, tl.longest, tl.lw, tl.lm, ask.seq - tl.asks, (unsigned)ask.fn, mcw_faults - tl.faults, mcw_calls - tl.calls, tl.sum[6] / tl.n, tl.sum[7] / tl.n ), tl.longest = 0, tl.asks = ask.seq, tl.faults = mcw_faults, tl.calls = mcw_calls;
		memset( tl.sum, 0, sizeof( tl.sum )), tl.n = 0;
	}
}
