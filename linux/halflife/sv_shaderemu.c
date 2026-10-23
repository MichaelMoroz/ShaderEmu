/*
sv_shaderemu.c - the server's frame on a worker core (docs/games/halflife.md)

Host_ServerFrame of frame n runs on a worker while this core draws frame n from what the server
said in frame n - 1. The packets each side sends the other while the frame is out wait for its
end; what only this core may do (allocations, cvars, commands) the worker asks for and waits;
console lines are kept. It is this core's while a level loads or a client is not yet in.
*/
#include <setjmp.h>
#include <stdlib.h>
#include "common.h"
#include "server.h"
#include "mcw.h"
#include "se_server.h"

#define BOX_BYTES 0x20000

char se_server_stack[SE_SERVER_STACK] __attribute__(( aligned( 16 )));
int hlglServerWorker( void ), hlglServerHelper( void ), hlglServerPacker( int i );
extern int se_pack_helpers;
uint32_t SE_PackJob( uint32_t part, uint32_t unused );
int SE_Packers( void );
void SE_PackReset( void ), SE_PackTouch( void ), SE_PackFrameEnd( void );
void hlglJobsWait( void );

typedef struct { int sock, length; netadr_t to; } kept_packet_t;
typedef struct { uint used, unused[3]; byte data[BOX_BYTES]; } box_t;
static box_t inbox __attribute__(( aligned( 16 )));	// this core's: packets for the server while it is out
static box_t outbox __attribute__(( aligned( 16 )));	// the job's: packets the server sent
static box_t server_in __attribute__(( aligned( 16 )));	// the inbox at the settle, read by the server's next frame
static struct { uint at, unused[3]; } server_in_read __attribute__(( aligned( 16 )));	// the server's
static struct { uint used, lost, failed, unused; char error[1024]; char kept[8192]; } work __attribute__(( aligned( 16 )));
static struct { volatile uint seq; uint32_t ( *fn )( void * ); void *args; uint unused; } ask __attribute__(( aligned( 16 )));		// the worker's
static struct { volatile uint seq; uint32_t result; uint unused[2]; } answer __attribute__(( aligned( 16 )));	// this core's
static struct SE_APART { jmp_buf at; } abort_of;	// the worker's
#define abort_at abort_of.at
static int mode = -1, worker, helpers, helper[8], out, inbox_loopback;	// mode 2: a frame given only once the last is done (HL_SERVER=every: each host frame)
static double skipped, given_frametime;	// the host's time while none was given; what the frame out covers

uint32_t se_call_on_stack( uint32_t ( *fn )( uint32_t ), uint32_t arg, void *stack_top );
#ifdef __gpuvm__
// (gpu_vm: the same, callr for jalr; a bundle on 16 bytes)
__asm__( ".text\n.globl se_call_on_stack\n.type se_call_on_stack, @function\n.p2align 4\nse_call_on_stack:\n"
	"	addi a2, a2, -16\n	sw ra, 12(a2)\n	sw sp, 8(a2)\n	mv sp, a2\n	mv t0, a0\n	mv a0, a1\n	callr t0\n"
	"	lw ra, 12(sp)\n	lw sp, 8(sp)\n	ret\n" );
#else
__asm__( ".text\n.globl se_call_on_stack\n.type se_call_on_stack, @function\nse_call_on_stack:\n"
	"	addi a2, a2, -16\n	sw ra, 12(a2)\n	sw sp, 8(a2)\n	mv sp, a2\n	mv t0, a0\n	mv a0, a1\n	jalr t0\n"
	"	lw ra, 12(sp)\n	lw sp, 8(sp)\n	ret\n" );
#endif

static void setup( void )
{
	const char *how = getenv( "HL_SERVER" );

	mode = 0;
	inbox_loopback = getenv( "HL_INBOX" ) && !Q_strcmp( getenv( "HL_INBOX" ), "loopback" );
	if( how && !Q_strcmp( how, "inline" ))
		return;
	if(( worker = hlglServerWorker( )) <= 0 )
		return;
	mcw_touch( se_server_stack, SE_SERVER_STACK );
	mcw_touch( &work, sizeof( work ));
	mcw_touch( &outbox, sizeof( outbox ));
	mcw_touch( &server_in_read, sizeof( server_in_read ));
	mcw_touch( &ask, sizeof( ask ));
	for( int i = 0; i < SE_Packers( ) && i < 8 && ( helper[i] = i ? hlglServerPacker( i - 1 ) : hlglServerHelper( )) > 0; i++ )
		helpers = i + 1;
	if( helpers )
		SE_PackTouch();
	mode = how && !Q_strcmp( how, "every" ) ? 1 : 2;
	Con_Printf( "the server's frame is on worker core %d%s\n", worker, mode == 2 ? ", given when the last is done" : "" );
}

// ---- what the worker asks of this core ----

struct se_asks_t { uint fn[64], n, unused[3]; } se_asks __attribute__(( aligned( 16 )));	// the worker's: DEBUG
uint32_t SE_ServerOnCore0( uint32_t ( *fn )( void * ), void *args )
{
	if( SE_OnRender( ))
		return SE_RenderOnCore0( fn, args );
	if( !SE_OnServer( ))
		return fn( args );
	ask.fn = fn, ask.args = args;
	ask.seq = ask.seq + 1;
	se_asks.fn[se_asks.n++ & 63] = (uint)fn;	// DEBUG
	while( answer.seq != ask.seq )
		mc_end_pass();
	return answer.result;
}

static struct { int on, unused[3]; } serving __attribute__(( aligned( 16 )));	// this core's
static void serve( void )
{
	if( ask.seq != answer.seq )
	{
		serving.on = 1;
		answer.result = ask.fn( ask.args );
		answer.seq = ask.seq;
		serving.on = 0;
	}
}

int SE_ServerServing( void )
{
	return serving.on;
}

// Whether the server's frame is still being made on its worker.
int SE_ServerBusy( void )
{
	return out && !mcw_done( worker );
}

void SE_ServerServe( void )
{
	if( out )
		serve();
	SE_RenderServe();
}

void SE_ServerKeepPrint( const char *text )
{
	uint n = Q_strlen( text ) + 1;

	if( SE_OnRender( ))
	{
		SE_RenderKeepPrint( text );
		return;
	}
	if( work.used + n > sizeof( work.kept ))
	{
		work.lost++;
		return;
	}
	memcpy( work.kept + work.used, text, n );
	work.used += n;
}

void SE_ServerAbort( const char *text, int fatal )
{
	if( SE_OnRender( ))
		SE_RenderAbort( text, fatal );
	Q_strncpy( work.error, text, sizeof( work.error ));
	work.failed = fatal ? 2 : 1;
	longjmp( abort_at, 1 );
}

static int box_keep( box_t *box, int sock, size_t length, const void *data, const void *to )
{
	uint bytes = sizeof( kept_packet_t ) + (( length + 15 ) & ~15u );
	kept_packet_t *p;

	if( box->used + bytes > BOX_BYTES )
		return 1;	// (lost, as a network loses them)
	p = (kept_packet_t *)( box->data + box->used );
	p->sock = sock, p->length = (int)length, p->to = *(const netadr_t *)to;
	memcpy( p + 1, data, length );
	box->used += bytes;
	return 1;
}

int SE_ServerKeepPacket( int sock, size_t length, const void *data, const void *to )
{
	if( SE_OnServer( ))
		return box_keep( &outbox, sock, length, data, to );
	if( out && sock == NS_CLIENT )
		return box_keep( &inbox, sock, length, data, to );
	return 0;
}

static void box_send( box_t *box )
{
	for( uint at = 0; at < box->used; )
	{
		kept_packet_t *p = (kept_packet_t *)( box->data + at );
		at += sizeof( kept_packet_t ) + (( p->length + 15 ) & ~15u );
		NET_SendPacket( p->sock, p->length, p + 1, p->to );
	}
	box->used = 0;
}

// The client's packets of a frame out go to the server whole: the loopback keeps only the last 4.
static void box_move( box_t *box )
{
	if( server_in_read.at >= server_in.used )
		server_in.used = server_in_read.at = 0;
	uint n = box->used <= BOX_BYTES - server_in.used ? box->used : 0;	// (lost, as a network loses them)
	memcpy( server_in.data + server_in.used, box->data, n );
	server_in.used += n;
	box->used = 0;
}

int SE_ServerTakePacket( void *data, size_t *length )
{
	if( server_in_read.at >= server_in.used )
		return 0;
	kept_packet_t *p = (kept_packet_t *)( server_in.data + server_in_read.at );
	server_in_read.at += sizeof( kept_packet_t ) + (( p->length + 15 ) & ~15u );
	memcpy( data, p + 1, p->length );
	*length = p->length;
	return 1;
}

// ---- the job ----

#ifdef __gpuvm__
static inline uint32_t sv_now( void ) { uint32_t v; __asm__ volatile( "mfsr %0, time_lo" : "=r"( v )); return v; }	// DEBUG
#else
static inline uint32_t sv_now( void ) { uint32_t v; __asm__ volatile( "rdtime %0" : "=r"( v )); return v; }	// DEBUG
#endif
#ifdef __gpuvm__
static inline uint32_t sv_ins( void ) { uint32_t v; __asm__ volatile( "mfsr %0, cycle" : "=r"( v )); return v; }
#else
static inline uint32_t sv_ins( void ) { uint32_t v; __asm__ volatile( "rdinstret %0" : "=r"( v )); return v; }
#endif
static struct __attribute__(( aligned( 16 ))) { volatile uint32_t start, end, ins, unused; } sv_tl;
static struct { uint32_t give, settle, sum[6], n; unsigned short bins[40]; } sv_tl0;
static uint32_t job_body( uint32_t unused )
{
	sv_tl.start = sv_now(), sv_tl.ins = sv_ins();
	if( setjmp( abort_at ))
	{
		SE_PackFrameEnd();
		return 1;
	}
	Host_ServerFrame();
	sv_tl.ins = sv_ins() - sv_tl.ins, sv_tl.end = sv_now();
	SE_PackFrameEnd();	// (its helper let go, if it was not asked)
	return 0;
}

static uint32_t server_job( uint32_t a0, uint32_t a1 )
{
	return se_call_on_stack( job_body, 0, se_server_stack + SE_SERVER_STACK );
}

// Before anything on this core that reads or changes the server's data: its frame out ends here.
void SE_ServerSettle( void )
{
	if( !out )
		return;
	sv_tl0.settle = sv_now();
	extern unsigned se_settle_passes;	// DEBUG
	while( !mcw_done( worker ))
	{
		serve();
		mcw_rest_for( &worker, 1 );	// (gpu_vm: the server's worker has the whole pass)
		se_settle_passes++;
	}
	for( int i = 0; i < se_pack_helpers; i++ )
		while( !mcw_done( helper[i] ))
			mcw_rest_for( &helper[i], 1 );
	se_pack_helpers = 0;
	out = 0;
	for( uint at = 0; at < work.used; at += Q_strlen( work.kept + at ) + 1 )
		Sys_Print( work.kept + at );
	work.used = 0;
	if( work.lost )
		Con_Printf( "(%u of the server's lines lost)\n", work.lost ), work.lost = 0;
	box_send( &outbox );
	if( inbox_loopback ) box_send( &inbox );	// (HL_INBOX=loopback: as before 20 October 2026)
	else box_move( &inbox );
	if( work.failed )
	{
		int fatal = work.failed == 2;
		work.failed = 0;
		if( fatal ) Sys_Error( "%s", work.error );
		else Host_Error( "%s", work.error );
	}
}

// The server's frame given to its worker (1), or it is this core's to make now (0).
int SE_ServerGive( void )
{
	if( mode < 0 )
		setup();
	int can = mode > 0 && svs.initialized && sv.state == ss_active && !sv.paused && CL_IsInGame( );
	for( int i = 0; can && i < svs.maxclients; i++ )
		if( svs.clients[i].state != cs_free && svs.clients[i].state != cs_spawned )
			can = 0;
	if( !can )
	{
		SE_ServerSettle();	// (the frame made here comes after the one out)
		skipped = 0;
		return 0;
	}
	if( out && mode == 2 && !mcw_done( worker ))
	{
		serve();
		skipped += host.frametime;
		return 1;
	}
	SE_ServerSettle();
	given_frametime = Q_min( host.frametime + skipped, MAX_FRAMETIME ), skipped = 0;
	if( sv_tl0.give )
	{
		sv_tl0.sum[0] += sv_tl.start - sv_tl0.give, sv_tl0.sum[1] += sv_tl.end - sv_tl0.give, sv_tl0.sum[2] += sv_tl0.settle - sv_tl0.give;
		sv_tl0.sum[3] += sv_now() - sv_tl0.give, sv_tl0.sum[4] += sv_tl.ins;
		sv_tl0.bins[sv_tl.ins / 16384 < 39 ? sv_tl.ins / 16384 : 39]++;
		if( ++sv_tl0.n == 256 )
		{
			extern unsigned se_world_kept_hits, se_world_kept_misses;
			fprintf( stderr, "sv timeline (passes after the give): start %.1f end %.1f; settle %.1f, next give %.1f; %u instructions; world kept %u of %u\n", sv_tl0.sum[0] / 3136.0, sv_tl0.sum[1] / 3136.0, sv_tl0.sum[2] / 3136.0, sv_tl0.sum[3] / 3136.0, sv_tl0.sum[4] / 256, se_world_kept_hits, se_world_kept_hits + se_world_kept_misses );
			se_world_kept_hits = se_world_kept_misses = 0;
			fprintf( stderr, "sv frames by 16k:" );
			for( int b = 0; b < 40; b++ )
				if( sv_tl0.bins[b] )
					fprintf( stderr, " %d:%u", b * 16, sv_tl0.bins[b] );
			fprintf( stderr, "\n" );
			memset( &sv_tl0, 0, sizeof( sv_tl0 ));
		}
	}
	sv_tl0.give = sv_now();
	if( helpers )
	{
		SE_PackReset();
		for( int i = 0; i < helpers; i++ )
			mcw_post( helper[i], SE_PackJob, i, 0 );
		se_pack_helpers = helpers;
	}
	mcw_post( worker, server_job, 0, 0 );
	out = 1;
	return 1;
}

double SE_ServerFrametime( void )
{
	return SE_OnServer( ) ? given_frametime : host.frametime;
}

void SE_ServerFrameStart( void )
{
	if( out && mode == 2 && !mcw_done( worker ))
		return;
	SE_ServerSettle();
}

// ---- the C library's allocations (the link wraps them): the first core's ----

void *__real_malloc( size_t n );
void __real_free( void *p );
void *__real_calloc( size_t n, size_t size );
void *__real_realloc( void *p, size_t n );
int __real_posix_memalign( void **p, size_t align, size_t n );
void *__real_aligned_alloc( size_t align, size_t n );
void *__real_memalign( size_t align, size_t n );

typedef struct { void *p; size_t a, b; } alloc_args_t;
static uint32_t malloc_0( void *p ) { alloc_args_t *a = p; return (uint32_t)__real_malloc( a->a ); }
static uint32_t free_0( void *p ) { alloc_args_t *a = p; __real_free( a->p ); return 0; }
static uint32_t calloc_0( void *p ) { alloc_args_t *a = p; return (uint32_t)__real_calloc( a->a, a->b ); }
static uint32_t realloc_0( void *p ) { alloc_args_t *a = p; return (uint32_t)__real_realloc( a->p, a->a ); }
static uint32_t posix_memalign_0( void *p ) { alloc_args_t *a = p; return (uint32_t)__real_posix_memalign( (void **)a->p, a->a, a->b ); }
static uint32_t aligned_alloc_0( void *p ) { alloc_args_t *a = p; return (uint32_t)__real_aligned_alloc( a->a, a->b ); }
static uint32_t memalign_0( void *p ) { alloc_args_t *a = p; return (uint32_t)__real_memalign( a->a, a->b ); }

void *__wrap_malloc( size_t n )
{
	alloc_args_t a = { NULL, n, 0 };
	return SE_OffCore0() ? (void *)SE_ServerOnCore0( malloc_0, &a ) : __real_malloc( n );
}
void __wrap_free( void *p )
{
	alloc_args_t a = { p, 0, 0 };
	if( p && SE_OffCore0( )) SE_ServerOnCore0( free_0, &a );
	else __real_free( p );
}
void *__wrap_calloc( size_t n, size_t size )
{
	alloc_args_t a = { NULL, n, size };
	return SE_OffCore0() ? (void *)SE_ServerOnCore0( calloc_0, &a ) : __real_calloc( n, size );
}
void *__wrap_realloc( void *p, size_t n )
{
	alloc_args_t a = { p, n, 0 };
	return SE_OffCore0() ? (void *)SE_ServerOnCore0( realloc_0, &a ) : __real_realloc( p, n );
}
int __wrap_posix_memalign( void **p, size_t align, size_t n )
{
	alloc_args_t a = { p, align, n };
	return SE_OffCore0() ? (int)SE_ServerOnCore0( posix_memalign_0, &a ) : __real_posix_memalign( p, align, n );
}
void *__wrap_aligned_alloc( size_t align, size_t n )
{
	alloc_args_t a = { NULL, align, n };
	return SE_OffCore0() ? (void *)SE_ServerOnCore0( aligned_alloc_0, &a ) : __real_aligned_alloc( align, n );
}
void *__wrap_memalign( size_t align, size_t n )
{
	alloc_args_t a = { NULL, align, n };
	return SE_OffCore0() ? (void *)SE_ServerOnCore0( memalign_0, &a ) : __real_memalign( align, n );
}
