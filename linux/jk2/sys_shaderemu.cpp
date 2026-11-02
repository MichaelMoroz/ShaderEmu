// The machine's side of OpenJK's Jedi Outcast (docs/games/jk2.md): what shared/sdl/ did, and the two libraries the
// engine loaded (the game with its client's game, and the renderer with Ghoul2), which build.sh links in.
//
// For now it is the "null" platform of the plan's steps 0 and 1: no window, no picture (the renderer's GL calls are
// nullgl.c's), no input but the terminal's console lines, and sound mixed into a buffer nobody hears, so that what the
// game does with sounds (their lengths) is as with sound. The same file builds for the build computer (JK2_HOST=1)
// and for the machine; the Nano-X window, the GPU and the sound card come at the plan's steps 2 and 4.
//
//   JK2_FRAMES=N      quit after N frames, with a jk2stat: line (frames, the time they took, the zone's bytes)
//   JK2_STATS=N       a jk2stat: line every N frames (0: none, the default)
//   JK2_LAST=CMD      a console command at the end, before quitting (zone_details: the zone's bytes by kind)
//   JK2_AT=F:CMD;...  console commands at frames F ("900:load auto")
//   (the engine's own +set fixedtime N makes every frame N ms of the game's time: two runs, and the build computer's
//   and the machine's, then play the same game)
//   JK2_SOUND=off     no sound at all (the game then loads none); otherwise mixed into nothing at 22 kHz
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "qcommon/qcommon.h"
#include "rd-common/tr_types.h"
#include "client/snd_local.h"
#include "client/client.h"
#include "sys/sys_local.h"
#include "sys/sys_public.h"

// ---- the linked-in libraries (sys_loadlib.h's Sys_LoadLibrary under SHADEREMU) ----
extern "C" void *se_GetGameAPI( void *import );
extern "C" void *se_GetRefAPI( int apiVersion, void *rimp );
extern "C" void se_dllEntry( void *syscalls );
extern "C" intptr_t se_vmMain( int command, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t );

static int se_handles[2];

// The game's variables (build.sh's jk2game_data and jk2game_bss, the linker's __start_ and __stop_ round them): a copy
// at its first load, put back at every later one, as a library's are fresh when it is loaded again. Its C++ objects'
// constructors ran at the program's start, so the copy has them made; what they held since is left, not freed.
extern "C" char __start_jk2game_data[], __stop_jk2game_data[], __start_jk2game_bss[], __stop_jk2game_bss[];
static char *se_game_image;

#if defined(__SANITIZE_ADDRESS__)
#define SE_WHOLE __attribute__((no_sanitize("address"), noinline))   // (the sections hold the sanitizer's own red zones too)
static SE_WHOLE void se_copy( char *to, const char *from, size_t n )
{
	for ( volatile size_t i = 0; i < n; i++ )
		to[i] = from[i];
}
#else
#define se_copy( to, from, n ) memcpy( to, from, n )
#endif

static void se_game_fresh( void )
{
	size_t data = __stop_jk2game_data - __start_jk2game_data, bss = __stop_jk2game_bss - __start_jk2game_bss;
	if ( !se_game_image ) {
		se_game_image = (char *)malloc( data + bss );
		se_copy( se_game_image, __start_jk2game_data, data );
		se_copy( se_game_image + data, __start_jk2game_bss, bss );
		Com_Printf( "jk2: the game's variables are %d bytes and %d bytes, kept as they began\n", (int)data, (int)bss );
		return;
	}
	se_copy( __start_jk2game_data, se_game_image, data );
	se_copy( __start_jk2game_bss, se_game_image + data, bss );
}

void *SE_LoadLibrary( const char *name )
{
	if ( strstr( name, "jospgame" ) ) {
		se_game_fresh();
		return &se_handles[0];
	}
	if ( strstr( name, "rdjosp-vanilla" ) )
		return &se_handles[1];
	return NULL;
}

void SE_UnloadLibrary( void *handle )
{
	(void)handle;
}

void *SE_LoadFunction( void *handle, const char *name )
{
	if ( handle == &se_handles[0] && !strcmp( name, "GetGameAPI" ) )
		return (void *)se_GetGameAPI;
	if ( handle == &se_handles[0] && !strcmp( name, "dllEntry" ) )     // (the client's game is in the same library)
		return (void *)se_dllEntry;
	if ( handle == &se_handles[0] && !strcmp( name, "vmMain" ) )
		return (void *)se_vmMain;
	if ( handle == &se_handles[1] && !strcmp( name, "GetRefAPI" ) )
		return (void *)se_GetRefAPI;
	return NULL;
}

static int env_int( const char *name, int otherwise )
{
	const char *v = getenv( name );
	return v && *v ? atoi( v ) : otherwise;
}

// ---- the end (sys_main.cpp's Sys_Exit) ----
#ifdef SE_GPROF
extern "C" void _mcleanup( void );     // (glibc's: gmon.out, for a -pg build on the build computer)
#endif

void NORETURN SE_Exit( int code )
{
	fflush( stdout );
	fflush( stderr );
#ifdef SE_GPROF
	_mcleanup();
#endif
	_exit( code );
}

// ---- the window: none ----
cvar_t *r_mode;
cvar_t *r_customwidth;
cvar_t *r_customheight;
cvar_t *r_fullscreen;

qboolean R_GetModeInfo( int *width, int *height, int mode )
{
	static const int modes[][2] = { {320, 240}, {400, 300}, {512, 384}, {640, 480}, {800, 600}, {960, 720},
		{1024, 768}, {1152, 864}, {1280, 1024}, {1600, 1200}, {2048, 1536}, {856, 480}, {2400, 600} };
	if ( mode == -1 ) {
		*width = r_customwidth->integer;
		*height = r_customheight->integer;
		return qtrue;
	}
	if ( mode < 0 || mode >= (int)ARRAY_LEN( modes ) )
		return qfalse;
	*width = modes[mode][0];
	*height = modes[mode][1];
	return qtrue;
}

window_t WIN_Init( const windowDesc_t *desc, glconfig_t *glConfig )
{
	r_mode = Cvar_Get( "r_mode", "3", CVAR_ARCHIVE | CVAR_LATCH );
	r_customwidth = Cvar_Get( "r_customwidth", "640", CVAR_ARCHIVE | CVAR_LATCH );
	r_customheight = Cvar_Get( "r_customheight", "480", CVAR_ARCHIVE | CVAR_LATCH );
	r_fullscreen = Cvar_Get( "r_fullscreen", "0", CVAR_ARCHIVE | CVAR_LATCH );
	Cvar_Get( "r_availableModes", "", CVAR_ROM );
	int w = 640, h = 480;
	if ( !R_GetModeInfo( &w, &h, r_mode->integer ) )
		w = 640, h = 480;
	glConfig->vidWidth = w;
	glConfig->vidHeight = h;
	glConfig->colorBits = 32;
	glConfig->depthBits = 24;
	glConfig->stencilBits = 0;
	glConfig->displayFrequency = 60;
	glConfig->deviceSupportsGamma = qfalse;
	glConfig->isFullscreen = qfalse;
	glConfig->stereoEnabled = qfalse;
	IN_Init( NULL );
	window_t window = {};
	window.api = desc->api;
	return window;
}

static int se_frames, se_stop_at = -2, se_stats_every, se_window_start, se_window_frames;

// this core's instructions (the machine's rdcycle counts them on rvc_cpu; 0 on the card and on the build computer)
static unsigned long long se_instructions( void )
{
#ifdef __riscv
	unsigned hi, lo, again;
	do {
		asm volatile( "rdcycleh %0" : "=r"( hi ) );
		asm volatile( "rdcycle %0" : "=r"( lo ) );
		asm volatile( "rdcycleh %0" : "=r"( again ) );
	} while ( hi != again );
	return (unsigned long long)hi << 32 | lo;
#else
	return 0;
#endif
}
static unsigned long long se_window_instructions;

void WIN_Present( window_t *window )
{
	(void)window;
	if ( se_stop_at == -2 ) {
		se_stop_at = env_int( "JK2_FRAMES", 0 );
		se_stats_every = env_int( "JK2_STATS", 0 );
		se_window_start = Sys_Milliseconds();
	}
	se_frames++;
	se_window_frames++;
	bool stop = se_stop_at > 0 && se_frames >= se_stop_at;
	if ( stop || ( se_stats_every > 0 && se_window_frames >= se_stats_every ) ) {
		int now = Sys_Milliseconds();
		extern int SE_ZoneBytes( int *peak, int *blocks );
		int peak, blocks, bytes = SE_ZoneBytes( &peak, &blocks );
		int free_kb = -1, cached_kb = -1;     // the system's free memory (on the machine all a program takes is taken from it)
		if ( FILE *m = fopen( "/proc/meminfo", "r" ) ) {
			char line[128];
			while ( fgets( line, sizeof( line ), m ) ) {
				sscanf( line, "MemFree: %d", &free_kb );
				sscanf( line, "Cached: %d", &cached_kb );
			}
			fclose( m );
		}
		unsigned long long ins = se_instructions();
		Com_Printf( "jk2stat: frame %d, %d frames in %d ms of the computer's time and %u thousand instructions of this core,"
			" zone %d bytes in %d blocks, at most %d, free %d KB, cached %d KB, client state %d%s%s\n", se_frames, se_window_frames,
			now - se_window_start, (unsigned)( ( ins - se_window_instructions ) / 1000 ), bytes, blocks, peak, free_kb, cached_kb, cls.state,
			Cvar_VariableIntegerValue( "cl_paused" ) ? ", paused" : "", Cvar_VariableIntegerValue( "sv_paused" ) ? ", server paused" : "" );
		se_window_start = now;
		se_window_instructions = ins;
		se_window_frames = 0;
	}
	// JK2_AT="FRAME:COMMAND;FRAME:COMMAND": console commands at those frames (a scripted run: "900:load auto")
	const char *at = getenv( "JK2_AT" );
	for ( ; at && *at; at = strchr( at, ';' ) ? strchr( at, ';' ) + 1 : NULL ) {
		const char *colon = strchr( at, ':' );
		if ( colon && atoi( at ) == se_frames ) {
			const char *end = strchr( colon, ';' );
			int len = end ? (int)( end - colon - 1 ) : (int)strlen( colon + 1 );
			Com_Printf( "jk2: frame %d: %.*s\n", se_frames, len, colon + 1 );
			Cbuf_AddText( va( "%.*s\n", len, colon + 1 ) );
		}
	}
	if ( stop ) {
		const char *last = getenv( "JK2_LAST" );    // console commands before the end (zone_details)
		if ( last && *last )
			Cbuf_ExecuteText( EXEC_NOW, va( "%s\n", last ) );
		Com_Printf( "jk2: %d frames, quitting\n", se_frames );
		Cbuf_ExecuteText( EXEC_NOW, "quit\n" );
	}
}

void WIN_SetGamma( glconfig_t *glConfig, byte red[256], byte green[256], byte blue[256] )
{
}

void WIN_Shutdown( void )
{
	IN_Shutdown();
}

void *WIN_GL_GetProcAddress( const char *proc )
{
	return NULL;    // (no extension: the renderer takes the plain paths)
}

qboolean WIN_GL_ExtensionSupported( const char *extension )
{
	return qfalse;
}

void GLimp_Minimize( void ) {}
void GLimp_EnableLogging( qboolean enable ) {}
void GLimp_LogComment( char *comment ) {}

// ---- input: the terminal's lines are console commands (con_tty.cpp); nothing else yet ----
cvar_t *in_joystick;

void IN_Init( void *windowData )
{
	in_joystick = Cvar_Get( "in_joystick", "0", CVAR_ARCHIVE_ND | CVAR_LATCH );
}
void IN_Frame( void ) {}
void IN_Shutdown( void ) {}
void IN_Restart( void ) {}

uint8_t ConvertUTF32ToExpectedCharset( uint32_t utf32 )
{
	return utf32 < 256 ? (uint8_t)utf32 : '?';
}

// ---- sound: mixed into a buffer, at the pace of the client's time ----
static int se_dma_start, se_sound_on = -1;

qboolean SNDDMA_Init( int sampleFrequencyInKHz )
{
	if ( se_sound_on < 0 ) {
		const char *s = getenv( "JK2_SOUND" );
		se_sound_on = !( s && !strcmp( s, "off" ) );
	}
	if ( !se_sound_on )
		return qfalse;
	dma.samplebits = 16;
	dma.channels = 2;
	dma.samples = 16384;
	dma.submission_chunk = 1;
	dma.speed = 22050;
	if ( !dma.buffer )
		dma.buffer = (byte *)calloc( 1, dma.samples * 2 );
	se_dma_start = cls.realtime;
	return qtrue;
}

int SNDDMA_GetDMAPos( void )
{
	int ms = cls.realtime - se_dma_start;    // (the client's time: under fixedtime it is the game's, and a sound ends at the same frame every run)
	int frames = ms / 1000 * dma.speed + ms % 1000 * dma.speed / 1000;     // (no 64-bit division on the machine)
	return frames * dma.channels & ( dma.samples - 1 );
}

void SNDDMA_Shutdown( void )
{
	free( dma.buffer );
	dma.buffer = NULL;
}

void SNDDMA_Submit( void ) {}
void SNDDMA_BeginPainting( void ) {}
void SNDDMA_Activate( qboolean activate ) {}
