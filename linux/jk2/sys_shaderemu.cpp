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

// The game's variables (build.sh's jk2game_data and jk2game_bss, the linker's __start_ and __stop_ round them) and its
// constructors (jk2game_init, which the program's start does not run): at every load the data as it began (a copy
// of 30 KB, made at the first), the rest zero, and the constructors run, as a library's are when it is loaded again.
// What the objects of the load before held is left, not freed. (A copy of both, kept before, was 5.5 MB more.)
extern "C" char __start_jk2game_data[], __stop_jk2game_data[], __start_jk2game_bss[], __stop_jk2game_bss[];
typedef void ( *se_constructor_t )( void );
extern "C" se_constructor_t __start_jk2game_init[], __stop_jk2game_init[];
static char *se_game_image;

#if defined(__SANITIZE_ADDRESS__)
#define SE_WHOLE __attribute__((no_sanitize("address"), noinline))   // (the sections hold the sanitizer's own red zones too)
static SE_WHOLE void se_copy( char *to, const char *from, size_t n )
{
	for ( volatile size_t i = 0; i < n; i++ )
		to[i] = from ? from[i] : 0;
}
#else
static void se_copy( char *to, const char *from, size_t n )
{
	if ( from )
		memcpy( to, from, n );
	else
		memset( to, 0, n );
}
#endif

static void se_game_fresh( void )
{
	size_t data = __stop_jk2game_data - __start_jk2game_data, bss = __stop_jk2game_bss - __start_jk2game_bss;
	if ( !se_game_image ) {
		se_game_image = (char *)malloc( data );
		se_copy( se_game_image, __start_jk2game_data, data );
		Com_Printf( "jk2: the game's variables are %d bytes and %d bytes, %d constructors\n", (int)data, (int)bss,
			(int)( __stop_jk2game_init - __start_jk2game_init ) );
	} else {
		se_copy( __start_jk2game_data, se_game_image, data );
		se_copy( __start_jk2game_bss, NULL, bss );
	}
	for ( se_constructor_t *c = __start_jk2game_init; c < __stop_jk2game_init; c++ )
		( *c )();
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

// ---- the window (the machine's build, SE_WINDOW): a Nano-X window the GPU draws in, through gl_jk2.c ----
#ifdef SE_WINDOW
extern "C" {
#include <nano-X.h>
int jk2glOpen( unsigned window );
int jk2glOn( void );
void jk2glSwap( void );
void jk2RenderSettle( void );
void jk2RenderServe( void );
void jk2RenderStats( unsigned *gives, unsigned *settles, unsigned *gl_settles, unsigned *waits );
void jk2glStats( unsigned *in_gpu, unsigned *gpu_size, unsigned *left_out, unsigned *vertices );
const volatile unsigned *gpuControl( int offset );
void gpuWindowOrigin( int *x, int *y );
void seglWindowChanged( void );
void seglSize( int *width, int *height );
}
#define REG_INPUT 0x20      // the machine's input words: the pointer, its buttons, the keys' count (docs/machine/input.md)
#define REG_CLOCK 0x34      // its clock in milliseconds

struct se_asks_t { unsigned fn[64], n, unused[3]; } se_asks;   // (what gl_shaderemu.c reads of Half-Life's server: never written here)
static GR_WINDOW_ID se_window;
static void se_input( void );
static void se_world_tell( void );
static int se_win_w = 640, se_win_h = 480;
static int se_vid_w = 640, se_vid_h = 480;     // what the renderer draws (glConfig's size)
static unsigned se_resized_ms;                 // when the window's size last changed (0: settled)

// A window when there is a window system (else none, and the picture is nullgl's: a run for the sums)
static void se_window_open( int w, int h )
{
	GR_SCREEN_INFO info;
	const char *picture = getenv( "JK2_PICTURE" );
	if ( se_window || ( picture && !strcmp( picture, "none" ) ) || GrOpen() < 0 )
		return;
	GrGetScreenInfo( &info );
	se_win_w = w, se_win_h = h;
	if ( se_win_w > info.cols )
		se_win_w = info.cols;
	if ( se_win_h > info.rows - 55 )      // all of it above the bar, under a caption (29 and 26)
		se_win_h = info.rows - 55;
	se_window = GrNewWindowEx( GR_WM_PROPS_APPWINDOW, (char *)"Jedi Outcast", GR_ROOT_WINDOW_ID, -1, -1, se_win_w, se_win_h, 0 );
	GrSelectEvents( se_window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN |
		GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_ENTER | GR_EVENT_MASK_MOUSE_EXIT | GR_EVENT_MASK_CLOSE_REQ |
		GR_EVENT_MASK_UPDATE );
	GR_WM_PROPERTIES props;     // the wheel tilted too (se_input: the weapons)
	props.flags = GR_WM_FLAGS_COMMAND, props.props = GR_WMC_BUTTONS, props.background = 1;
	GrSetWMProperties( se_window, &props );
	GrMapWindow( se_window );
	GrSetFocus( se_window );
	if ( jk2glOpen( se_window ) < 0 ) {
		Com_Printf( "jk2: this machine has no GPU: no picture\n" );
		return;
	}
	seglSize( &se_win_w, &se_win_h );
}
#endif

static glconfig_t *se_glconfig;     // (WIN_Init's: the renderer's own, WIN_Shutdown)

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
	se_glconfig = glConfig;
	glConfig->vidWidth = w;
	glConfig->vidHeight = h;
	se_vid_w = w, se_vid_h = h;
	glConfig->colorBits = 32;
	glConfig->depthBits = 24;
	glConfig->stencilBits = 0;
	glConfig->displayFrequency = 60;
	glConfig->deviceSupportsGamma = qfalse;
	glConfig->isFullscreen = qfalse;
	glConfig->stereoEnabled = qfalse;
#ifdef SE_WINDOW
	se_window_open( w, h );
#endif
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
unsigned long long SE_Instructions( void ) { return se_instructions(); }   // (this core's, for the files' log of files.cpp)
static unsigned long long se_window_instructions;

void WIN_Present( window_t *window )
{
	(void)window;
#ifdef SE_WINDOW
	jk2glSwap();
	se_world_tell();
	se_input();
#endif
	if ( se_stop_at == -2 ) {
		se_stop_at = env_int( "JK2_FRAMES", 0 );
		se_stats_every = env_int( "JK2_STATS", 0 );
		se_window_start = Sys_Milliseconds();
	}
	se_frames++;
	se_window_frames++;
	// JK2_MARK=F0,F1: a measured window (tools/game_par.py's jk2 entries): a line at frame F0 and one at F1 with its
	// frames and milliseconds of the machine's time, then the end
	static int mark_from = -2, mark_to, mark_ms;
	if ( mark_from == -2 ) {
		const char *m = getenv( "JK2_MARK" );
		mark_from = -1;
		if ( m && sscanf( m, "%d,%d", &mark_from, &mark_to ) != 2 )
			mark_from = -1;
	}
	if ( mark_from > 0 && se_frames == mark_from ) {
		mark_ms = Sys_Milliseconds();
		Com_Printf( "jk2mark: begins at frame %d\n", se_frames );
	}
	if ( mark_from > 0 && se_frames == mark_to ) {
		Com_Printf( "jk2mark: ends, %d frames in %d ms\n", mark_to - mark_from, Sys_Milliseconds() - mark_ms );
		se_stop_at = se_frames;
	}
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
#ifdef SE_WINDOW
		if ( jk2glOn() ) {     // the GPU's memory: textures in it, and how many found no room; vertices drawn since
			unsigned in_gpu, size, left_out, vertices, gives, settles, gl_settles, waits;
			jk2glStats( &in_gpu, &size, &left_out, &vertices );
			Com_Printf( "jk2gpu: textures %u of %u KB, %u left out, %u vertices a frame\n", in_gpu >> 10, size >> 10, left_out,
				vertices / ( se_stats_every > 0 ? se_stats_every : 1 ) );
			jk2RenderStats( &gives, &settles, &gl_settles, &waits );
			Com_Printf( "jk2render: %u frames given to the drawing's worker, %u settled (%u by a GL call of this core's), "
				"%u passes waited for it\n", gives, settles, gl_settles, waits );
		}
#endif
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
	// JK2_KEYS="FRAME:KEY FRAME:KEY+ FRAME:KEY-": keys pressed and let go (KEY), held (KEY+) or let go (KEY-) at those
	// frames, as the window's would be: a played start for a test (enter, escape, space, up, down, left, right,
	// mouse1, mouse2, tab, shift, ctrl, a letter or digit), or atX,Y: the menus' pointer put at X, Y of their 640 x 480
	static const char *keys = (const char *)-1;
	static int let_go[8], let_go_count, point_x = -1, point_y;
	if ( keys == (const char *)-1 )
		keys = getenv( "JK2_KEYS" );
	for ( int i = 0; i < let_go_count; i++ )
		Sys_QueEvent( 0, SE_KEY, let_go[i], qfalse, 0, NULL );
	let_go_count = 0;
	if ( point_x >= 0 ) {    // (the second half of atX,Y, a frame after the first: moves queued together are summed)
		Sys_QueEvent( 0, SE_MOUSE, point_x, point_y, 0, NULL );
		point_x = -1;
	}
	for ( const char *k = keys; k && *k; ) {
		while ( *k == ' ' )
			k++;
		int frame = atoi( k ), len = 0;
		const char *name = strchr( k, ':' );
		if ( !name )
			break;
		name++;
		while ( name[len] && name[len] != ' ' && name[len] != '+' && name[len] != '-' )
			len++;
		char how = name[len] == '+' || name[len] == '-' ? name[len] : 0;
		if ( frame == se_frames ) {
			static const struct { const char *name; int key; } named[] = { {"enter", A_ENTER}, {"escape", A_ESCAPE},
				{"space", A_SPACE}, {"up", A_CURSOR_UP}, {"down", A_CURSOR_DOWN}, {"left", A_CURSOR_LEFT},
				{"right", A_CURSOR_RIGHT}, {"mouse1", A_MOUSE1}, {"mouse2", A_MOUSE2}, {"tab", A_TAB}, {"shift", A_SHIFT},
				{"ctrl", A_CTRL} };
			int key = len == 1 ? name[0] : 0, x, y;
			if ( sscanf( name, "at%d,%d", &x, &y ) == 2 ) {     // atX,Y: the menu's pointer to X, Y of 640 x 480
				Com_Printf( "jk2: frame %d: pointer at %d, %d\n", se_frames, x, y );
				Sys_QueEvent( 0, SE_MOUSE, -10000, -10000, 0, NULL );
				point_x = x, point_y = y;
			}
			for ( unsigned i = 0; i < ARRAY_LEN( named ); i++ )
				if ( (int)strlen( named[i].name ) == len && !strncmp( name, named[i].name, len ) )
					key = named[i].key;
			if ( key ) {
				Com_Printf( "jk2: frame %d: key %.*s%c\n", se_frames, len, name, how ? how : ' ' );
				Sys_QueEvent( 0, SE_KEY, key, how != '-', 0, NULL );
				if ( !how && let_go_count < (int)ARRAY_LEN( let_go ) )
					let_go[let_go_count++] = key;
			}
		}
		k = name + len + ( how ? 1 : 0 );
	}
	if ( stop ) {
		const char *last = getenv( "JK2_LAST" );    // console commands before the end (zone_details)
		if ( last && *last )
			Cbuf_ExecuteText( EXEC_NOW, va( "%s\n", last ) );
		Com_Printf( "jk2: %d frames, quitting\n", se_frames );
		Cbuf_ExecuteText( EXEC_NOW, "quit\n" );
	}
}

#ifdef SE_WINDOW
// Whether the game is being played, no menu over it (the drawing on a worker then: gl_jk2.c)
extern "C" int SE_InPlay( void )
{
	return cls.state == CA_ACTIVE && !( Key_GetCatcher() & ( KEYCATCH_UI | KEYCATCH_CONSOLE ) );
}

// ---- the holodeck's seat (docs/world/holodeck.md; game 10 of linux/apps/gameid.h) ----
// GLES/segl.h's words of the host, here without the header (this file is the engine's, built without the GL headers)
extern "C" {
const volatile unsigned int *seglHostState( void );
void seglWorldUp( int axis );
void seglWorldFlat( int flat );
}
#define SE_HOST_HOLODECK( s )   ( ( ( s )[0] >> 4 ) & 1 )
#define SE_HOST_HANDS( s )      ( ( ( s )[0] >> 5 ) & 1 )
#define SE_HOST_KIND( s )       ( ( s )[0] & 15 )
#define SE_HOST_JUMP( s )       ( ( ( s )[0] >> 6 ) & 1 )
#define SE_HOST_HEAD_PITCH( s ) ( (short)( ( s )[1] >> 16 ) )
#define SE_HOST_HAND_PITCH( s, h ) ( (short)( ( s )[5 + 2 * ( h ) - ( h )] >> 16 ) )
#define SE_HOST_TRIGGER( s, h ) ( ( ( s )[4 + 2 * ( h )] >> 30 ) & 1 )
#define SE_HOST_GRIP( s, h )    ( ( ( s )[4 + 2 * ( h )] >> 31 ) & 1 )
#define SE_HOST_STICK( s, n )   ( (signed char)( ( s )[5] >> ( 8 * ( n ) ) ) )
#define SE_HOST_MOUSE_X( s )    ( ( s )[4] & 0xffff )
static int se_seated;           // 0, 1 with tracked hands, 2 a desktop's sitter

// segl.h's seglWorldEye: the player's eye over its floor in the game's units, its near and far planes and its projection's scales
static void se_world_eye( int game, float eye, float znear, float zfar, float fx, float fy )
{
	volatile unsigned int *word = (volatile unsigned int *)seglHostState();
	float x = 1.28f * fx, y = 0.8f * fy, unit = 2 * znear / ( x < y ? x : y ), spread = logf( zfar / znear );
	int a = (int)( logf( eye / unit ) / spread * 4096 ), b = (int)( 4096 / spread );
	unsigned said;
	if ( !word || spread <= 0 )
		return;
	a = a < -32768 ? -32768 : a > 32767 ? 32767 : a;
	b = b > 4095 ? 4095 : b;
	said = (unsigned)( a & 0xffff ) | (unsigned)b << 16 | 10u << 28;
	word += ( 0x6c - 0x360 ) / 4;
	if ( *word != said )
		*word = said;
}

// What the room is told each frame: a menu, the console or a load is a flat screen; the world's up is z; the eye 48 units
// over the feet (the game's own view height), its planes 4 to 4,096 and an 80-degree picture of 4:3
static void se_world_tell( void )
{
	static int told_up;
	if ( !seglHostState() )
		return;
	if ( !told_up ) {
		seglWorldUp( 3 );
		told_up = 1;
	}
	seglWorldFlat( !SE_InPlay() );
	se_world_eye( 10, 48, 4, 4096, 1 / tanf( 40 * ( M_PI / 180 ) ), 1 / ( tanf( 40 * ( M_PI / 180 ) ) * 0.75f ) );
}
#endif

// A move as it is made (jk2.patch's CL_CreateCmd): from a holodeck's seat the sticks walk and turn the body, the right
// trigger fires, the left the other fire, the right grip uses and the jump button jumps; a desktop's sitter turns by their
// mouse's counts and the head's tilt is the aim's rise. JK2_SEAT=1 says how it is seated every 20 moves.
void SE_SeatMove( usercmd_t *cmd )
{
#ifdef SE_WINDOW
	static unsigned mouse_count;
	static int said = -1;
	const volatile unsigned *host = seglHostState();
	int now = !host || !SE_HOST_HOLODECK( host ) ? 0 : SE_HOST_HANDS( host ) ? 1 : SE_HOST_KIND( host ) == 2 ? 2 : 0;
	if ( said < 0 )
		said = getenv( "JK2_SEAT" ) != NULL;
	if ( now != se_seated ) {
		se_seated = now;
		mouse_count = host ? SE_HOST_MOUSE_X( host ) : 0;
	}
	if ( !se_seated || Key_GetCatcher() || cls.state != CA_ACTIVE ) {
		mouse_count = host ? SE_HOST_MOUSE_X( host ) : 0;
		return;
	}
	if ( se_seated == 1 ) {
		float look = SE_HOST_STICK( host, 2 ) / 127.0f;
		cl.viewangles[YAW] -= ( fabsf( look ) < 0.15f ? 0 : look * fabsf( look ) ) * 140 * cls.frametime * 0.001f;
		cl.viewangles[PITCH] = -( (short)( host[7] >> 16 ) ) * ( 360.0f / 65536.0f );   // the right hand's rise
	} else {
		unsigned count = SE_HOST_MOUSE_X( host );
		cl.viewangles[YAW] -= 0.066f * (short)( count - mouse_count );
		mouse_count = count;
		cl.viewangles[PITCH] = -SE_HOST_HEAD_PITCH( host ) * ( 360.0f / 65536.0f );
	}
	int forward = cmd->forwardmove + SE_HOST_STICK( host, 1 ), right = cmd->rightmove + SE_HOST_STICK( host, 0 );
	cmd->forwardmove = forward > 127 ? 127 : forward < -127 ? -127 : forward;
	cmd->rightmove = right > 127 ? 127 : right < -127 ? -127 : right;
	if ( SE_HOST_TRIGGER( host, 1 ) )
		cmd->buttons |= BUTTON_ATTACK;
	if ( SE_HOST_TRIGGER( host, 0 ) )
		cmd->buttons |= BUTTON_ALT_ATTACK;
	if ( SE_HOST_GRIP( host, 1 ) )
		cmd->buttons |= BUTTON_USE;
	if ( SE_HOST_JUMP( host ) )
		cmd->upmove = 127;
	if ( said && ++said % 20 == 0 )
		Com_Printf( "seat: %d aim %.1f %.1f move %d %d %d buttons %x\n", se_seated, cl.viewangles[YAW], cl.viewangles[PITCH], cmd->forwardmove,
			cmd->rightmove, cmd->upmove, cmd->buttons );
#else
	(void)cmd;
#endif
}

#ifdef SE_WINDOW

// A frame given to the drawing's worker (gl_jk2.c): the window's part of the swap, its list shown when the job is done
extern "C" void SE_WindowFrame( void )
{
	WIN_Present( NULL );
}
#endif

void WIN_SetGamma( glconfig_t *glConfig, byte red[256], byte green[256], byte blue[256] )
{
}

void WIN_Shutdown( void )
{
	IN_Shutdown();
	// rd-vanilla calls WIN_Init again only while glConfig's width is 0 (R_Init), and its RE_Shutdown leaves it as it was: a
	// vid_restart at the window's size kept 640 x 480, drawn in the corner of the larger window (seen in a capture, 3 November)
	if ( se_glconfig )
		se_glconfig->vidWidth = 0;
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
#ifdef SE_WINDOW
static int se_key( int ch )
{
	static const struct { int ch, key; } other[] = {
		{MWKEY_LEFT, A_CURSOR_LEFT}, {MWKEY_RIGHT, A_CURSOR_RIGHT}, {MWKEY_UP, A_CURSOR_UP}, {MWKEY_DOWN, A_CURSOR_DOWN},
		{MWKEY_ENTER, A_ENTER}, {'\n', A_ENTER}, {'\r', A_ENTER}, {MWKEY_KP_ENTER, A_KP_ENTER}, {MWKEY_ESCAPE, A_ESCAPE},
		{MWKEY_BACKSPACE, A_BACKSPACE}, {MWKEY_TAB, A_TAB}, {MWKEY_INSERT, A_INSERT}, {MWKEY_DELETE, A_DELETE},
		{MWKEY_HOME, A_HOME}, {MWKEY_END, A_END}, {MWKEY_PAGEUP, A_PAGE_UP}, {MWKEY_PAGEDOWN, A_PAGE_DOWN},
		{MWKEY_LSHIFT, A_SHIFT}, {MWKEY_RSHIFT, A_SHIFT}, {MWKEY_LCTRL, A_CTRL}, {MWKEY_RCTRL, A_CTRL},
		{MWKEY_LALT, A_ALT}, {MWKEY_RALT, A_ALT}, {MWKEY_PAUSE, A_PAUSE}, {'`', A_CONSOLE}, {'~', A_CONSOLE},
	};
	if ( ch >= 'A' && ch <= 'Z' )
		return ch - 'A' + 'a';
	static const int f[12] = { A_F1, A_F2, A_F3, A_F4, A_F5, A_F6, A_F7, A_F8, A_F9, A_F10, A_F11, A_F12 };
	if ( ch >= MWKEY_F1 && ch <= MWKEY_F12 )
		return f[ch - MWKEY_F1];
	for ( unsigned i = 0; i < ARRAY_LEN( other ); i++ )
		if ( other[i].ch == ch )
			return other[i].key;
	return ch > 32 && ch < 127 ? ch : ch == ' ' ? A_SPACE : 0;   // (the printable keys' codes are their characters)
}

// The window's keys and buttons as the game's events, and the pointer as its mouse: in a menu the menus' cursor is
// put where the pointer is, in the game the pointer's travel while the left button is held (the view dragged round, a
// finger on glass), the right button the fire (Quake III's way, linux/quake3/tr_shaderemu.c). The window system is
// asked when the machine's input words moved and a few times a second.
void _UI_MouseTo( int x, int y );     // (jk2.patch, ui_main.cpp)
static int se_origin_x, se_origin_y, se_origin_known;   // the window's first pixel on the display
static int se_menu_x = -1, se_menu_y = -1;              // where the menus' cursor was put last (640 x 480)

// The pointer's place in the menus' 640 x 480: the window's picture is the whole window, stretched (seglPicturePoint).
// Until 3 November 2026 a menu was told how far the pointer went, a pixel of the window for 640 / its width of the
// menu's; the menu's cursor stops at its edges and the pointer does not, a move outside the window was not told, and a
// window that had been moved or resized went on from the cursor's old place: the saber and the desktop's pointer
// went apart and a click landed where the saber was, not where the pointer was (the owner's photo of the main menu
// in the world). Now the cursor is put where the pointer is, at any size of the window.
static void se_menu_pointer( int x, int y )
{
	if ( !se_origin_known )
		gpuWindowOrigin( &se_origin_x, &se_origin_y ), se_origin_known = 1;
	int mx = ( x - se_origin_x ) * 640 / se_win_w, my = ( y - se_origin_y ) * 480 / se_win_h;
	mx = mx < 0 ? 0 : mx > 640 ? 640 : mx;
	my = my < 0 ? 0 : my > 480 ? 480 : my;
	if ( mx == se_menu_x && my == se_menu_y )
		return;
	se_menu_x = mx, se_menu_y = my;
	_UI_MouseTo( mx, my );
	if ( getenv( "JK2_POINTER_LOG" ) )
		Com_Printf( "jk2pointer: %d,%d of the display to %d,%d of the menu (window at %d,%d, %dx%d)\n", x, y, mx, my,
			se_origin_x, se_origin_y, se_win_w, se_win_h );
}

// A controller (docs/machine/input.md, "Game controls"; the bits are linux/apps/input_words.h's): the jump button
// (A) jumps, B uses, the left stick pressed crouches, Y is Escape; the wheel (the right stick, tilted too) is the next
// weapon up or right, the one before down or left, and the menus' own wheel in a menu.
static void se_pad_buttons( unsigned buttons )
{
	static unsigned held;
	unsigned down = buttons & ~held, up = held & ~buttons;
	held = buttons;
	if ( ( down | up ) & 0x10000 )
		Sys_QueEvent( 0, SE_KEY, A_SPACE, ( down & 0x10000 ) != 0, 0, NULL );
	if ( ( down | up ) & 0x8000 )
		Sys_QueEvent( 0, SE_KEY, 'e', ( down & 0x8000 ) != 0, 0, NULL );
	if ( ( down | up ) & 0x1000 )
		Sys_QueEvent( 0, SE_KEY, 'c', ( down & 0x1000 ) != 0, 0, NULL );
	if ( down & 0x4000 ) {
		Sys_QueEvent( 0, SE_KEY, A_ESCAPE, qtrue, 0, NULL );
		Sys_QueEvent( 0, SE_KEY, A_ESCAPE, qfalse, 0, NULL );
	}
}

static void se_input( void )
{
	static unsigned seen[4], asked_ms;
	static int ask = 8, mouse_x, mouse_y, mouse_known, mouse_inside = 1, dragging;
	if ( !se_window || !jk2glOn() )
		return;
	const volatile unsigned *input = gpuControl( REG_INPUT ), *clock = gpuControl( REG_CLOCK );
	int menu = ( Key_GetCatcher() & ( KEYCATCH_UI | KEYCATCH_CONSOLE ) ) != 0 || cls.state != CA_ACTIVE;
	int ui = ( Key_GetCatcher() & KEYCATCH_UI ) != 0;
	if ( mouse_inside || dragging ) {
		int x = (int)input[0], y = (int)input[1];
		if ( ui )
			se_menu_pointer( x, y );
		else if ( !menu && dragging && mouse_known && ( x != mouse_x || y != mouse_y ) )
			Sys_QueEvent( 0, SE_MOUSE, ( x - mouse_x ) * 2, ( y - mouse_y ) * 2, 0, NULL );
		mouse_x = x, mouse_y = y, mouse_known = 1;
	}
	if ( !ui )
		se_menu_x = -1;     // (a menu that comes is told where the pointer is at once)
	// The renderer at the window's size (docs/games/jk2.md, "The window's size"): a second after the last change of it, a
	// restart of the renderer at that size, of 320 x 240 to 1024 x 768 in whole 8s; JK2_RESIZE=stretch draws 640 x 480 stretched
	static int resize_stretch = -1;
	if ( resize_stretch < 0 ) {
		const char *how = getenv( "JK2_RESIZE" );
		resize_stretch = how && !strcmp( how, "stretch" );
	}
	if ( se_resized_ms && *clock - se_resized_ms >= 1000 ) {
		int w = se_win_w > 1024 ? 1024 : se_win_w < 320 ? 320 : se_win_w & ~7, h = se_win_h > 768 ? 768 : se_win_h < 240 ? 240 : se_win_h & ~7;
		se_resized_ms = 0;
		if ( !resize_stretch && ( w != se_vid_w || h != se_vid_h ) ) {
			Com_Printf( "jk2: the window is %dx%d: the renderer restarted at %dx%d\n", se_win_w, se_win_h, w, h );
			Cvar_Set( "r_mode", "-1" );
			Cvar_SetValue( "r_customwidth", w );
			Cvar_SetValue( "r_customheight", h );
			Cbuf_AddText( "vid_restart\n" );
		}
	}
	for ( int i = 2; i < 4; i++ )
		if ( seen[i] != input[i] ) {
			seen[i] = input[i], ask = 8;
			if ( i == 2 )
				se_pad_buttons( seen[2] );
		}
	if ( !ask && *clock - asked_ms >= 250 )
		ask = 1;
	while ( ask ) {
		GR_EVENT e;
		int down, key;
		GrCheckNextEvent( &e );
		switch ( e.type ) {
		case GR_EVENT_TYPE_NONE:
			ask--;
			asked_ms = *clock;
			return;
		case GR_EVENT_TYPE_MOUSE_ENTER:
			mouse_inside = 1, mouse_known = 0;
			break;
		case GR_EVENT_TYPE_MOUSE_EXIT:
			mouse_inside = 0;
			break;
		case GR_EVENT_TYPE_UPDATE:
			if ( e.update.utype == GR_UPDATE_SIZE ) {
				jk2RenderSettle();     // (the drawing's worker done first: the window's GL state changes)
				seglWindowChanged();
				seglSize( &se_win_w, &se_win_h );
				if ( se_win_w < 1 || se_win_h < 1 )
					se_win_w = 640, se_win_h = 480;
				se_resized_ms = *clock | 1;
			}
			se_origin_known = 0, se_menu_x = -1;   // (moved or resized: asked again)
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_BUTTON_UP:
			down = e.type == GR_EVENT_TYPE_BUTTON_DOWN;
			if ( e.button.changebuttons & ( GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLDN | GR_BUTTON_SCROLLLEFT | GR_BUTTON_SCROLLRIGHT ) ) {
				int ahead = ( e.button.changebuttons & ( GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLRIGHT ) ) != 0;
				if ( !menu && down )
					Cbuf_AddText( ahead ? "weapnext\n" : "weapprev\n" );
				else if ( menu )
					Sys_QueEvent( 0, SE_KEY, ahead ? A_MWHEELUP : A_MWHEELDOWN, down, 0, NULL );
				break;
			}
			if ( e.button.changebuttons & GR_BUTTON_EXTRA )
				break;
			// (where the window is, for nothing: the press's place in it and on the display)
			se_origin_x = e.button.rootx - e.button.x, se_origin_y = e.button.rooty - e.button.y, se_origin_known = 1;
			// a click in a menu lands where it was pressed, whatever the words said meanwhile (nxkey's presses are not in them)
			if ( ui )
				se_menu_pointer( e.button.rootx, e.button.rooty );
			if ( !menu ) {
				if ( e.button.changebuttons & GR_BUTTON_L )
					dragging = down, mouse_known = 0;
				if ( e.button.changebuttons & GR_BUTTON_R )
					Sys_QueEvent( 0, SE_KEY, A_MOUSE1, down, 0, NULL );
				break;
			}
			dragging = 0;
			if ( e.button.changebuttons & GR_BUTTON_L )
				Sys_QueEvent( 0, SE_KEY, A_MOUSE1, down, 0, NULL );
			if ( e.button.changebuttons & GR_BUTTON_R )
				Sys_QueEvent( 0, SE_KEY, A_MOUSE2, down, 0, NULL );
			if ( e.button.changebuttons & GR_BUTTON_M )
				Sys_QueEvent( 0, SE_KEY, A_MOUSE3, down, 0, NULL );
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
		case GR_EVENT_TYPE_KEY_UP:
			down = e.type == GR_EVENT_TYPE_KEY_DOWN;
			key = se_key( e.keystroke.ch );
			if ( key )
				Sys_QueEvent( 0, SE_KEY, key, down, 0, NULL );
			if ( down && key == A_BACKSPACE )
				Sys_QueEvent( 0, SE_CHAR, 'h' - 'a' + 1, 0, 0, NULL );
			else if ( down && key != A_CONSOLE && e.keystroke.ch >= 32 && e.keystroke.ch < 127 )
				Sys_QueEvent( 0, SE_CHAR, e.keystroke.ch, 0, 0, NULL );
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			Cbuf_AddText( "quit\n" );
			break;
		}
	}
}
#endif
void IN_Frame( void ) {}      // (the window's input is taken once a frame, in WIN_Present)
void IN_Shutdown( void ) {}
void IN_Restart( void ) {}

uint8_t ConvertUTF32ToExpectedCharset( uint32_t utf32 )
{
	return utf32 < 256 ? (uint8_t)utf32 : '?';
}

// ---- sound: mixed into a buffer, at the pace of the client's time; with a sound card (the machine's build) the
// buffer is a stream voice's ring, played at the card's pace ----
static int se_dma_start, se_sound_on = -1;
#ifdef SE_WINDOW
#include "../userland/shaderemu_sound.h"
static int se_voice = -1;           // the stream voice the engine's mix is played on, when there is a card
extern int s_paintedtime;

// The card's ring as the engine's DMA buffer: JK2_SOUND=mix keeps the buffer nobody hears (a run for the sums: the
// card's pace is not the client's time, and a voice that ends in another frame moves them)
static qboolean se_card_init( void )
{
	const char *s = getenv( "JK2_SOUND" );
	uint32_t at;
	if ( ( s && !strcmp( s, "mix" ) ) || snd_open() < 0 || !SND_RATE || ( se_voice = snd_claim() ) < 0 )
		return qfalse;
	dma.samplebits = 16;
	dma.channels = 2;
	dma.samples = 16384;                // (8,192 frames: 0.37 s at 22,050 a second)
	dma.submission_chunk = 1;
	dma.speed = 22050;
	dma.buffer = (byte *)snd_memory( dma.samples * 2, &at );
	if ( !dma.buffer )
		return qfalse;
	memset( dma.buffer, 0, dma.samples * 2 );
	snd_play( se_voice, SND_STREAM | SND_STEREO, at, dma.samples / 2, 0, snd_step( dma.speed ), SND_VOLUME( 256, 256 ) );
	Com_Printf( "jk2: sound on the card's voice %d, its ring %d frames\n", se_voice, dma.samples / 2 );
	return qtrue;
}
#endif

qboolean SNDDMA_Init( int sampleFrequencyInKHz )
{
	if ( se_sound_on < 0 ) {
		const char *s = getenv( "JK2_SOUND" );
		se_sound_on = !( s && !strcmp( s, "off" ) );
	}
	if ( !se_sound_on )
		return qfalse;
#ifdef SE_WINDOW
	if ( se_voice >= 0 || se_card_init() )
		return qtrue;
	se_voice = -1;
#endif
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
#ifdef SE_WINDOW
	if ( se_voice >= 0 )                // the frame the card is at (its count from the start)
		return SND_STATE[se_voice].seen != SND_VOICE[se_voice].key ? 0 : (int)( SND_STATE[se_voice].position * 2 & ( dma.samples - 1 ) );
#endif
	int ms = cls.realtime - se_dma_start;    // (the client's time: under fixedtime it is the game's, and a sound ends at the same frame every run)
	int frames = ms / 1000 * dma.speed + ms % 1000 * dma.speed / 1000;     // (no 64-bit division on the machine)
	return frames * dma.channels & ( dma.samples - 1 );
}

void SNDDMA_Shutdown( void )
{
#ifdef SE_WINDOW
	if ( se_voice >= 0 )                // (the card's ring and voice are kept for the next init)
		return;
#endif
	free( dma.buffer );
	dma.buffer = NULL;
}

void SNDDMA_Submit( void )
{
#ifdef SE_WINDOW
	if ( se_voice >= 0 )                // how far the mix is written: the card plays up to there and waits
		SND_VOICE[se_voice].loop = (uint32_t)s_paintedtime;
#endif
}
void SNDDMA_BeginPainting( void ) {}
void SNDDMA_Activate( qboolean activate ) {}

// ---- the music on the card (docs/games/jk2.md, "Music on the card") ----
// The engine's music (snd_dma.cpp's background tracks: an intro and its loop, or the level's dynamic explore, action and
// boss tracks with their cross-fades) decides what plays, when and how loud, as it did; what it gave its mixer, the
// samples it decoded on this core (2.8 to 3.0 million instructions a frame: two thirds of the menu's), it now gives
// here: each track's file, as the engine holds it in memory, is decoded by libmedia on the worker cores, at half its
// rate (22,050 a second, the mix's), into a stream voice's ring of the card. A stream is a track played from a place
// on: the engine's next give at the frame where the last ended is the same stream, any other a new one (a rewind, a
// seek, the fader's copy going on with the old). A stream's voice starts when the card's clock reaches the time the
// engine meant its first frames for (its raw buffer's end), so an intro's tail and its loop do not overlap; and it is
// let go once what was given is heard. JK2_MUSIC=engine is the way before; with no card (the sums' runs) or no workers
// the engine decodes and mixes as before.
#ifdef SE_WINDOW
#include "media.h"
#include <zlib.h>
#include "mcw.h"
#include "mcwx.h"

#define SE_STREAMS 3            // the current track, the fader's and one going quiet
#define SE_RING 32768           // a stream's ring, frames (1.49 s at 22,050)
#define SE_JOBS 4               // pieces decoded at once, a stream
#define SE_PIECE 5512           // frames a piece (a quarter of a second)

struct se_stream_t {
	const void *track;
	uint32_t bytes;
	struct media_audio *a;
	struct media_audio_info info;
	int voice, open, playing, start_at, given_round;
	unsigned volume;
	int16_t *ring;
	uint32_t ring_at;
	int16_t *stage[SE_JOBS];
	mcw_handle job[SE_JOBS];
	uint32_t job_piece[SE_JOBS];
	uint8_t *done;
	uint32_t base, pos, next, ordered, written;
};
static se_stream_t se_streams[SE_STREAMS];
static int se_music_state = -1, se_round, se_music_workers;
extern "C" char _start[], etext[];

static uint32_t se_music_job( uint32_t sj, uint32_t piece )
{
	se_stream_t *s = &se_streams[sj >> 8];
	int j = sj & 255;
	uint32_t n = media_audio_piece_frames( s->a, piece ), ch = (uint32_t)s->info.channels;
	uint32_t at = ( media_audio_piece_first( s->a, piece ) - s->base ) & ( SE_RING - 1 ), first = SE_RING - at < n ? SE_RING - at : n;
	media_audio_piece( s->a, j, piece, s->stage[j] );
	memcpy( s->ring + at * ch, s->stage[j], first * ch * 2 );
	if ( n > first )
		memcpy( s->ring, s->stage[j] + first * ch, ( n - first ) * ch * 2 );
	return n;
}

int SE_MusicOn( void )
{
	if ( se_music_state >= 0 )
		return se_music_state;
	const char *m = getenv( "JK2_MUSIC" );
	if ( se_voice < 0 )
		return 0;     // (no card yet, or none: asked again)
	se_music_state = 0;
	if ( m && !strcmp( m, "engine" ) )
		return 0;
	se_music_workers = mcw_open( env_int( "JK2_WORKERS", 32 ) );
	if ( se_music_workers <= 0 ) {
		Com_Printf( "jk2: no workers: the music decoded by this core\n" );
		return 0;
	}
	mcw_touch_code( _start, (unsigned)( etext - _start ) );   // (a worker stops at a page of code no core has run)
	for ( int i = 0; i < SE_STREAMS; i++ ) {
		se_stream_t *s = &se_streams[i];
		if ( ( s->voice = snd_claim() ) < 0 || !( s->ring = (int16_t *)snd_memory( SE_RING * 4, &s->ring_at ) ) ) {
			Com_Printf( "jk2: no voice or sound memory for the music: decoded by this core\n" );
			return 0;
		}
		mcw_touch( s->ring, SE_RING * 4 );
		for ( int j = 0; j < SE_JOBS; j++ ) {
			s->stage[j] = (int16_t *)mcw_alloc( ( SE_PIECE + 4096 ) * 4 );
			mcw_touch( s->stage[j], ( SE_PIECE + 4096 ) * 4 );
		}
	}
	Com_Printf( "jk2: music on the card, decoded by %d workers\n", se_music_workers );
	return se_music_state = 1;
}

static void se_stream_close( se_stream_t *s )
{
	for ( int j = 0; j < SE_JOBS; j++ )
		if ( s->job[j] )
			mcw_join( s->job[j] ), s->job[j] = 0;
	snd_stop( s->voice );
	if ( s->a )
		media_audio_close( s->a ), s->a = NULL;
	free( s->done ), s->done = NULL;
	s->open = s->playing = 0;
}

static uint32_t se_stream_played( se_stream_t *s )
{
	int v = s->voice;
	return !s->playing || SND_STATE[v].seen != SND_VOICE[v].key ? 0 : SND_STATE[v].position;
}

static se_stream_t *se_stream_open( const void *track, uint32_t bytes, uint32_t pos, int start_at )
{
	se_stream_t *s = NULL;
	const char *why = "";
	for ( int i = 0; i < SE_STREAMS && !s; i++ )
		if ( !se_streams[i].open )
			s = &se_streams[i];
	for ( int i = 0; i < SE_STREAMS && !s; i++ )     // (none free: the one given to longest ago)
		if ( !s || se_streams[i].given_round < s->given_round )
			s = &se_streams[i];
	if ( !s )
		s = &se_streams[0];
	if ( s->open )
		se_stream_close( s );
	if ( !( s->a = media_audio_open( track, bytes, &why ) ) ) {
		Com_Printf( "jk2: music: %s\n", why );
		return NULL;
	}
	media_audio_half( s->a, 1 );
	media_audio_get_info( s->a, &s->info );
	if ( s->info.rate != dma.speed ) {
		media_audio_half( s->a, 0 );
		media_audio_get_info( s->a, &s->info );
	}
	if ( s->info.rate != dma.speed || s->info.channels < 1 || s->info.channels > 2 || !media_audio_set_piece( s->a, SE_PIECE ) ||
	     media_audio_decoders( s->a, SE_JOBS ) < SE_JOBS ) {
		Com_Printf( "jk2: music: a track of %d Hz, %d channels, %u frames: not played\n", s->info.rate, s->info.channels,
			(unsigned)s->info.frames );
		media_audio_close( s->a ), s->a = NULL;
		return NULL;
	}
	media_audio_get_info( s->a, &s->info );
	uint32_t p0 = media_audio_piece_of( s->a, pos < s->info.frames ? pos : s->info.frames - 1 );
	s->track = track, s->bytes = bytes, s->pos = pos, s->start_at = start_at;
	s->base = media_audio_piece_first( s->a, p0 );
	s->next = s->ordered = p0, s->written = 0;
	s->done = (uint8_t *)calloc( s->info.pieces + 1, 1 );
	s->open = 1, s->playing = 0, s->given_round = se_round;
	if ( getenv( "JK2_MUSIC_LOG" ) )
		Com_Printf( "jk2music: stream %d: a track of %u frames at %d Hz, %d channels, from frame %u (piece %u of %u), at %d\n",
			(int)( s - se_streams ), (unsigned)s->info.frames, s->info.rate, s->info.channels, (unsigned)pos, (unsigned)p0,
			(unsigned)s->info.pieces, start_at );
	return s;
}

// What the engine gives of a track: `want` frames from `pos`, meant to sound at `raw_time` of the card's clock (its raw
// buffer's end); how many the track has from there (fewer at its end), or -1 when it cannot be played here
int SE_MusicGive( const void *track, int bytes, int pos, int want, float volume, int raw_time )
{
	se_stream_t *s = NULL;
	for ( int i = 0; i < SE_STREAMS && !s; i++ )
		if ( se_streams[i].open && se_streams[i].track == track && se_streams[i].pos == (uint32_t)pos )
			s = &se_streams[i];
	if ( !s && !( s = se_stream_open( track, (uint32_t)bytes, (uint32_t)pos, raw_time ) ) )
		return -1;
	int left = (int)s->info.frames - pos, got = want < left ? want : left > 0 ? left : 0;
	s->pos += got;
	s->given_round = se_round;
	int v = (int)( volume * 256.0f );
	s->volume = v < 0 ? 0 : v > 256 ? 256 : v;
	return got;
}

// A track's file freed by the engine (an intro's when its loop begins): its streams decode no more, and play out what
// is decoded of what was given
void SE_MusicForget( const void *track )
{
	for ( int i = 0; i < SE_STREAMS; i++ ) {
		se_stream_t *s = &se_streams[i];
		if ( !s->open || s->track != track || !s->a )
			continue;
		for ( int j = 0; j < SE_JOBS; j++ )
			if ( s->job[j] ) {
				mcw_join( s->job[j] );
				s->job[j] = 0;
				s->done[s->job_piece[j]] = 1;
			}
		while ( s->ordered < s->info.pieces && s->done[s->ordered] )
			s->written += media_audio_piece_frames( s->a, s->ordered++ );
		media_audio_close( s->a ), s->a = NULL;
		s->track = NULL;
		if ( s->pos - s->base > s->written )
			s->pos = s->base + s->written;
	}
}

// Once a frame, before the engine's gives: the pieces decoded are taken into the streams in order, more given out to the
// workers up to a ring ahead of what is heard, a voice started when its time comes, a stream let go once heard
void SE_MusicRound( int sound_time, int muted )
{
	jk2RenderServe();     // (the drawing's worker, a page or a system call it stopped at)
	if ( se_music_state != 1 )
		return;
	for ( int i = 0; i < SE_STREAMS; i++ ) {
		se_stream_t *s = &se_streams[i];
		if ( !s->open )
			continue;
		for ( int j = 0; j < SE_JOBS && s->a; j++ )
			if ( s->job[j] && mcw_ready( s->job[j] ) ) {
				mcw_join( s->job[j] );
				s->job[j] = 0;
				s->done[s->job_piece[j]] = 1;
			}
		while ( s->a && s->ordered < s->info.pieces && s->done[s->ordered] )
			s->written += media_audio_piece_frames( s->a, s->ordered++ );
		uint32_t played = se_stream_played( s ), given_end = s->pos > s->base ? s->pos - s->base : 0;
		if ( s->given_round != se_round && ( !s->playing || played >= given_end ) ) {
			if ( getenv( "JK2_MUSIC_LOG" ) )
				Com_Printf( "jk2music: stream %d ends: %u frames heard, %u written, %u given\n", i, (unsigned)played,
					(unsigned)s->written, (unsigned)given_end );
			se_stream_close( s );     // (not given in the last frame, and all of it heard)
			continue;
		}
		if ( s->playing && played >= s->written && s->written < given_end && getenv( "JK2_MUSIC_LOG" ) )
			Com_Printf( "jk2music: stream %d dry at %u frames\n", i, (unsigned)played );
		if ( !s->playing && s->written && sound_time - s->start_at >= 0 ) {
			snd_play( s->voice, SND_STREAM | ( s->info.channels == 2 ? SND_STEREO : 0 ) | SND_SMOOTH, s->ring_at, SE_RING, s->written,
				snd_step( (uint32_t)s->info.rate ), 0 );
			s->playing = 1;
		}
		if ( s->playing ) {
			SND_VOICE[s->voice].loop = s->written;
			SND_VOICE[s->voice].volume = muted ? 0 : SND_VOLUME( s->volume, s->volume );
		}
		for ( int j = 0; j < SE_JOBS && s->a && s->next < s->info.pieces; j++ ) {
			if ( s->job[j] )
				continue;
			uint32_t end = media_audio_piece_first( s->a, s->next ) + media_audio_piece_frames( s->a, s->next ) - s->base;
			if ( end - played > SE_RING || !( s->job[j] = mcw_try_call( se_music_job, (uint32_t)( i << 8 | j ), s->next ) ) )
				break;
			s->job_piece[j] = s->next++;
		}
	}
	se_round++;
}

// The mix painted on a worker (docs/games/jk2.md, "The mix on a worker"): S_Update_'s painting, the card's submit and
// the lip syncs, given at the end of S_Update and done before the client's next frame (CL_Frame), while the server's
// frame runs, which reads of the sound only the voices' loudness (gi.VoiceVolume). Only with the card (the sums' runs
// mix on this core) and the drawing's workers; JK2_MIX=core is the way before.
extern "C" int jk2MixOn( void );
extern "C" void jk2MixGive( void ( *fn )( void ) );
extern "C" void jk2MixSettle( void );
int SE_MixWorker( void )
{
	return se_voice >= 0 && jk2MixOn();
}
void SE_MixGive( void ( *fn )( void ) )
{
	jk2MixGive( fn );
}
void SE_MixSettle( void )
{
	jk2MixSettle();
}

// The workers opened for the load (JK2_WORKERS, 32), at the first; 0 for none. A team of libmedia's takes at most
// JK2_TEAM of them (16: a team over more costs this core more than it saves it), a large pak entry's split inflate all
// (se_wide_workers), the pictures and sounds side by side any that has nothing to do.
static int se_opened = -1;
static int se_wide_workers( void )
{
	if ( se_opened < 0 ) {
		se_opened = mcw_open( env_int( "JK2_WORKERS", 32 ) );
		if ( se_opened > 0 )
			mcw_touch_code( _start, (unsigned)( etext - _start ) );   // (a worker stops at a page of code no core has run)
		else
			se_opened = 0;
	}
	return mcw_on_worker() ? 0 : se_opened;
}
static int se_load_workers( void )
{
	static int team = -1;
	if ( team < 0 )
		team = env_int( "JK2_TEAM", 16 );
	int workers = se_wide_workers();
	return workers < team ? workers : team;
}

// Whether JK2_<what> asks for the engine's own decoder (the way before).
static int se_engine_decodes( const char *what )
{
	const char *how = getenv( what );
	return how && !strcmp( how, "engine" );
}

// A sound effect's MP3 decoded by libmedia on the workers, for S_LoadSound (docs/games/jk2.md, "The load"): the level's
// load decoded about 220 of them on this core, one after another, 1.65 billion instructions. Only where libmedia has the
// same rate and channels and the engine's length to a frame, or -1 and the engine decodes it.
// JK2_SOUNDS=engine is the way before.
int SE_SoundMP3( const void *data, int size, void *out, int bytes, int rate, int channels )
{
	static int engine = -1;
	if ( engine < 0 )
		engine = se_engine_decodes( "JK2_SOUNDS" );
	int workers = engine ? 0 : se_load_workers();
	if ( workers <= 0 || bytes <= 0 || channels < 1 )
		return -1;
	const char *why;
	struct media_audio *a = media_audio_open( data, (uint32_t)size, &why );
	if ( !a )
		return -1;
	struct media_audio_info info;
	media_audio_get_info( a, &info );
	// the engine counts a frame libmedia's table leaves out (one that decodes to nothing) or the other way round: the
	// length stays the engine's (ICARUS waits for a sound's end), a missing frame silence, a frame more cut off
	uint32_t frames = (uint32_t)bytes / ( 2u * (uint32_t)channels ), some = info.frames < frames ? info.frames : frames;
	int16_t *pcm = NULL;
	int done = -1;
	if ( info.rate == rate && info.channels == channels && info.frames + 1152u >= frames && frames + 1152u >= info.frames &&
	     frames * 2u * (uint32_t)channels == (uint32_t)bytes && !posix_memalign( (void **)&pcm, 16, (size_t)bytes ) ) {
		if ( media_audio_decode_all( a, pcm, some, workers ) == some ) {
			memcpy( out, pcm, (size_t)some * 2u * (uint32_t)channels );
			memset( (char *)out + (size_t)some * 2u * (uint32_t)channels, 0, (size_t)( frames - some ) * 2u * (uint32_t)channels );
			done = bytes;
		}
		free( pcm );
	}
	media_audio_close( a );
	return done;
}

// A pak's large entry inflated by libmedia over the workers, for minizip's unzReadCurrentFile when the whole of it is read
// at once (docs/games/jk2.md, "The load"): the level's animations are 9.6 MB, 202 million instructions of zlib's on this
// core. As a PNG's stream (m_inflate_stream_plain: the output's size known, a part's values kept, no second inflate, no
// CRC-32); with JK2_PAK_CRC=1 (`crc` given) media_inflate_stream, which makes the CRC. Under JK2_PAK_MIN packed bytes
// (65,536) zlib, which is faster on this core than libmedia's inflate. 0, or -1 and zlib inflates it; `in` NULL asks. JK2_PAK=engine is
// the way before.
extern "C" const char *m_inflate_stream_plain( const void *in, uint32_t in_size, void *out, uint32_t out_size, uint32_t *made,
					       int workers );
extern "C" const char *m_inflate_stream_rest( const void *in, uint32_t in_size, void *out, uint32_t out_size, uint32_t *made,
					      uint32_t *crc, int workers );

static void se_files_reap( int all );	// (files read ahead: below)

// A call waited for in passes this core rests (mcw_rest_on: a pass of idle work, some hundreds of instructions), not by
// mcw_join's polling, which served and counted instructions every turn: 15 million of the load's (docs/games/jk2.md, "The load")
static uint32_t se_join( int h )
{
	while ( !mcw_ready( h ) )
		mcw_rest_on( &h, 1 );
	return mcw_join( h );
}

// ---- pictures decoded side by side (rd-vanilla's R_FindImageFile, tr_image.cpp; docs/games/jk2.md, "The load") ----
// A picture's job: its file's bytes as they lie in the pak (deflated or stored), inflated into `plain` and decoded into
// `rgba` on a worker that has nothing to do (mcw_try_call: libmedia's teams leave such a worker out), with rooms of that
// worker's own. What a worker stores to is the picture's memory and its rooms alone; the record is read only.
extern "C" const char *m_jpeg_decode_alone_shrunk( const uint8_t *f, uint32_t size, struct media_image *out, void *room, int shrink );
extern "C" uint32_t m_jpeg_room( void );
static void ( *se_tga_pixels )( const void *file, unsigned char *rgba );	// (rd-common's SE_TGAPixels, which the renderer gives)
// (rd-vanilla's se_prepare, what Upload32 makes of the pixels before their upload: the samples, 3 or 4, + 8 made the
// GPU's words and + 16 with holes; NULL for none)
static int ( *se_prepare )( unsigned char *rgba, int width, int height, int halvings, int lightscale );

#define SE_PICTURES 256
struct __attribute__((aligned(16))) se_picture_t {
	const void *raw;
	uint32_t raw_len;
	int method;		// 8 deflated, 0 stored
	void *plain;		// the inflated file's room (deflated)
	uint32_t len;
	int kind, shrink;	// 1 JPEG (2^shrink times smaller), 2 TGA
	int width, height;	// the decoded picture's
	void *rgba;
	int halvings, lightscale;	// se_prepare's
	int handle;		// this core's: the call, 0 for a free record
};
static se_picture_t se_pictures[SE_PICTURES];
static struct se_rooms_t { void *inflate, *jpeg, *mp3; } se_rooms[256];
static void *se_head_room;
static int se_rooms_made;	// (every worker's rooms: SE_PicturesOn)
static void *se_head_room_mp3( void )
{
	static void *room;
	if ( !room && posix_memalign( &room, 16, media_mp3_room() ) )
		room = NULL;
	return room;
}

static uint32_t se_picture_job( uint32_t a0, uint32_t a1 )
{
	(void)a1;
	const se_picture_t *p = (const se_picture_t *)(uintptr_t)a0;
	const se_rooms_t *room = &se_rooms[mcw_self()];
	const uint8_t *data = (const uint8_t *)p->raw;
	uint32_t n = p->raw_len;
	if ( p->method == 8 ) {
		uint32_t made = 0;
		if ( media_inflate_one( p->raw, p->raw_len, p->plain, p->len, &made, NULL, NULL, room->inflate ) || made != p->len )
			return 2;
		data = (const uint8_t *)p->plain, n = p->len;
	}
	if ( p->kind == 1 ) {
		struct media_image out;
		memset( &out, 0, sizeof out );
		out.width = p->width, out.height = p->height, out.channels = 4, out.stride = p->width * 4, out.pixels = (uint8_t *)p->rgba;
		if ( m_jpeg_decode_alone_shrunk( data, n, &out, room->jpeg, 1 << p->shrink ) )
			return 2;
	} else {
		se_tga_pixels( data, (unsigned char *)p->rgba );
	}
	// (1 decoded; 32 + se_prepare's answer where it is prepared for its upload too)
	return se_prepare ? 32 + se_prepare( (unsigned char *)p->rgba, p->width, p->height, p->halvings, p->lightscale ) : 1;
}

// Whether pictures are decoded side by side now: off on a worker and in play, where the drawing's and the mix's workers
// are posted to directly. JK2_PICTURES_AHEAD=0 (or JK2_PICTURES=engine) is the way before. `tga` decodes a TGA's pixels
// (the renderer's own functions are its alone at the link).
int SE_PicturesOn( void ( *tga )( const void *file, unsigned char *rgba ),
		   int ( *prepare )( unsigned char *rgba, int width, int height, int halvings, int lightscale ) )
{
	static int on = -1;
	se_tga_pixels = tga, se_prepare = prepare;
	if ( on < 0 ) {
		on = 0;
		int workers = !se_engine_decodes( "JK2_PICTURES" ) && env_int( "JK2_PICTURES_AHEAD", 1 ) ? se_wide_workers() : 0;
		if ( workers > 0 && !posix_memalign( &se_head_room, 16, media_inflate_room() ) ) {
			on = 1;
			for ( int k = 1; k <= workers && k < 256 && on; k++ ) {
				if ( posix_memalign( &se_rooms[k].inflate, 16, media_inflate_room() ) ||
				     posix_memalign( &se_rooms[k].jpeg, 16, m_jpeg_room() ) ||	// (m_jpeg_room makes the colours' tables, here)
				     posix_memalign( &se_rooms[k].mp3, 16, media_mp3_room() ) )
					on = 0;
				else
					mcw_touch( se_rooms[k].inflate, media_inflate_room() ), mcw_touch( se_rooms[k].jpeg, m_jpeg_room() ),
					mcw_touch( se_rooms[k].mp3, media_mp3_room() );
			}
			// (a fixed block inflated here first: the tables inflate makes at its first call are this core's)
			static const uint8_t empty[2] = { 3, 0 };
			uint8_t none[16];
			uint32_t made;
			media_inflate_one( empty, 2, none, sizeof none, &made, NULL, NULL, se_head_room );
			se_rooms_made = on;
		}
	}
	return on && !mcw_on_worker() && !SE_InPlay();
}

// A file's first bytes, to `room` of them, inflated when it is deflated: how many. By zlib, which stops where the room
// is full (media_inflate_one made the whole first block: 139 thousand instructions a head; JK2_HEAD=media the way before),
// its state in malloc's memory, not the engine's zone.
static voidpf se_zalloc( voidpf, uInt items, uInt size ) { return malloc( (size_t)items * size ); }
static void se_zfree( voidpf, voidpf at ) { free( at ); }
int SE_PictureHead( const void *raw, int raw_len, int method, int len, void *head, int room )
{
	static int by_media = -1;
	if ( by_media < 0 ) {
		const char *e = getenv( "JK2_HEAD" );
		by_media = e && !strcmp( e, "media" );
	}
	uint32_t made = 0, want = (uint32_t)( len < room ? len : room );
	if ( method != 8 ) {
		made = (uint32_t)( raw_len < room ? raw_len : room );
		memcpy( head, raw, made );
	} else if ( by_media ) {
		media_inflate_one( raw, (uint32_t)raw_len, head, want, &made, NULL, NULL, se_head_room );
	} else {
		z_stream z;
		memset( &z, 0, sizeof( z ) );
		z.zalloc = se_zalloc, z.zfree = se_zfree;
		if ( inflateInit2( &z, -MAX_WBITS ) != Z_OK )
			return 0;
		z.next_in = (Bytef *)raw, z.avail_in = (uInt)raw_len;
		z.next_out = (Bytef *)head, z.avail_out = want;
		int r = inflate( &z, Z_SYNC_FLUSH );
		made = r == Z_OK || r == Z_STREAM_END || r == Z_BUF_ERROR ? want - z.avail_out : 0;
		inflateEnd( &z );
	}
	return (int)made;
}

// A deflated file's first bytes inflated in steps, 512 bytes and then twice as many each time, until `known` says they are
// enough or `room` of them (or the file) are made: how many (rd-vanilla's JPEG heads, whose frame is mostly in their first
// few hundred bytes: 80 thousand instructions a head to 4 KB)
int SE_PictureHeadUntil( const void *raw, int raw_len, int len, void *head, int room, int ( *known )( const unsigned char *head, int got ) )
{
	uint32_t made = 0, want = (uint32_t)( len < room ? len : room ), step = 512;
	z_stream z;
	memset( &z, 0, sizeof( z ) );
	z.zalloc = se_zalloc, z.zfree = se_zfree;
	if ( inflateInit2( &z, -MAX_WBITS ) != Z_OK )
		return 0;
	z.next_in = (Bytef *)raw, z.avail_in = (uInt)raw_len;
	z.next_out = (Bytef *)head;
	while ( made < want ) {
		uint32_t to = want - made > step ? made + step : want;
		z.avail_out = to - made;
		int r = inflate( &z, Z_SYNC_FLUSH );
		made = to - z.avail_out;
		if ( r != Z_OK || known( (const unsigned char *)head, (int)made ) )
			break;
		step *= 2;
	}
	inflateEnd( &z );
	return (int)made;
}

// A picture given to a worker with nothing to do: its handle, 0 when none has nothing to do (or no record is free)
int SE_PictureGive( const void *raw, int raw_len, int method, void *plain, int len, int kind, int shrink, void *rgba, int width,
		    int height, int halvings, int lightscale )
{
	se_files_reap( 0 );
	int i;
	for ( i = 0; i < SE_PICTURES && se_pictures[i].handle; i++ ) {}
	if ( i == SE_PICTURES )
		return 0;
	se_picture_t *p = &se_pictures[i];
	p->raw = raw, p->raw_len = (uint32_t)raw_len, p->method = method, p->plain = plain, p->len = (uint32_t)len;
	p->kind = kind, p->shrink = shrink, p->width = width, p->height = height, p->rgba = rgba;
	p->halvings = halvings, p->lightscale = lightscale;
	int h = mcw_try_call( se_picture_job, (uint32_t)(uintptr_t)p, 0 );
	p->handle = h;
	return h ? i + 1 : 0;
}

// The rest of this pass given up, a picture waiting for a worker with nothing to do (mcw_rest: the kernel's idle work)
void SE_PicturesRest( void ) { mcw_rest(); }

// Whether a given picture is through; its answer (1 decoded, 32 + se_prepare's decoded and prepared, 2 not), waited for
int SE_PictureReady( int given ) { return mcw_ready( se_pictures[given - 1].handle ); }
int SE_PictureJoin( int given )
{
	se_picture_t *p = &se_pictures[given - 1];
	int answer = (int)se_join( p->handle );
	p->handle = 0;
	return answer;
}

// A file of the pak read ahead (files.cpp's FS_ReadFile, "files read ahead"): its deflated bytes where they lie inflated
// into `out` on a worker with nothing to do, with that worker's room. Its handle, 0 when none has nothing to do, no record
// is free or the rooms are not made (SE_PicturesOn); SE_FileJoin waits: 1 made whole. What a worker stores to is `out`.
#define SE_FILES 64
struct __attribute__((aligned(16))) se_file_t { const void *raw; uint32_t raw_len; void *out; uint32_t len; int handle, state, answer; };
static se_file_t se_files[SE_FILES];	// state 0 free, 1 out on a worker, 2 through (its worker let go: se_files_reap)
static int se_files_out;
static uint32_t se_file_job( uint32_t a0, uint32_t a1 )
{
	(void)a1;
	const se_file_t *f = (const se_file_t *)(uintptr_t)a0;
	uint32_t made = 0;
	return !media_inflate_one( f->raw, f->raw_len, f->out, f->len, &made, NULL, NULL, se_rooms[mcw_self()].inflate ) &&
		made == f->len ? 1 : 2;
}

void S_SE_SoundsReady( void );	// (snd_mem.cpp)
extern "C" int SE_PakInflate( const void *in, unsigned in_size, void *out, unsigned out_size, unsigned *crc )
{
	static int engine = -1;
	static unsigned least;
	if ( engine < 0 ) {
		engine = se_engine_decodes( "JK2_PAK" );
		least = (unsigned)env_int( "JK2_PAK_MIN", 65536 );
	}
	if ( engine || se_load_workers() <= 0 )
		return -1;
	if ( in_size < least ) {
		// A smaller entry by one worker with nothing to do (libmedia's inflate, se_file_job), this core resting until it is
		// through: zlib's inflate of the small files read one at a time was some 100 million instructions of this core's
		// over the load (docs/games/jk2.md, "The load"). None free, no rooms, a CRC asked or JK2_PAK_ONE=0: zlib's, here.
		static unsigned one = (unsigned)env_int( "JK2_PAK_ONE", 2048 );
		if ( !one || in_size < one || crc || !se_rooms_made || mcw_on_worker() )
			return -1;
		if ( !in )
			return 0;
		static se_file_t job __attribute__(( aligned( 16 ) ));
		job.raw = in, job.raw_len = in_size, job.out = out, job.len = out_size;
		mcw_touch( out, out_size );
		// (none free: a pass rested and asked again, up to JK2_PAK_WAIT passes, 64, as a sound is: the sounds' decodes
		// keep every worker busy at times, and zlib here was most of what was left)
		static int wait = env_int( "JK2_PAK_WAIT", 64 );
		int h = 0;
		for ( int tries = 0; !( h = mcw_try_call( se_file_job, (uint32_t)(uintptr_t)&job, 0 ) ) && tries < wait; tries++ ) {
			se_files_reap( 0 );
			S_SE_SoundsReady();	// (the sounds through joined: their workers free)
			mcw_rest();
		}
		if ( !h )
			return -1;
		return se_join( h ) == 1 ? 0 : -1;
	}
	if ( !in )
		return 0;	// (asked whether it would take it, before the bytes are read)
	uint32_t made = 0;
	se_files_reap( 1 );	// (every worker for the team)
	mcw_touch( out, out_size );   // (stored to by this core first: the workers write it)
	// (JK2_PAK_REST=0: this core takes a part of each as the workers do, where it rests until they are through)
	static int rest = env_int( "JK2_PAK_REST", 1 );
	const char *why = rest ? m_inflate_stream_rest( in, in_size, out, out_size, &made, crc, se_wide_workers() )
		: crc ? media_inflate_stream( in, in_size, out, out_size, &made, NULL, crc, se_wide_workers() )
		      : m_inflate_stream_plain( in, in_size, out, out_size, &made, se_wide_workers() );
	if ( getenv( "JK2_PAK_LOG" ) )
		printf( "jk2: pak entry of %u packed bytes, %u made: %s\n", in_size, out_size,
			why ? why : made != out_size ? "short: zlib's" : "libmedia's" );
	return !why && made == out_size ? 0 : -1;
}
// Files out on workers: those through joined, so that their workers are free for libmedia's teams and the pictures;
// with `all` every one waited for
static void se_files_reap( int all )
{
	for ( int i = 0; i < SE_FILES && se_files_out; i++ ) {
		se_file_t *f = &se_files[i];
		if ( f->state == 1 && ( all || mcw_ready( f->handle ) ) ) {
			f->answer = (int)se_join( f->handle );
			f->handle = 0, f->state = 2;
			se_files_out--;
		}
	}
}
int SE_FileGive( const void *raw, int raw_len, void *out, int len )
{
	if ( !se_rooms_made || mcw_on_worker() || SE_InPlay() || raw_len <= 0 || len <= 0 )
		return 0;
	se_files_reap( 0 );
	int i;
	for ( i = 0; i < SE_FILES && se_files[i].state; i++ ) {}
	if ( i == SE_FILES )
		return 0;
	se_file_t *f = &se_files[i];
	f->raw = raw, f->raw_len = (uint32_t)raw_len, f->out = out, f->len = (uint32_t)len;
	mcw_touch( out, (unsigned)len );   // (stored to by this core first: the worker writes it)
	int h = mcw_try_call( se_file_job, (uint32_t)(uintptr_t)f, 0 );
	if ( !h )
		return 0;
	f->handle = h, f->state = 1;
	se_files_out++;
	return i + 1;
}
// A file given waited for and its record let go: 1 made whole
int SE_FileJoin( int given )
{
	if ( given <= 0 || given > SE_FILES || !se_files[given - 1].state )
		return 0;
	se_file_t *f = &se_files[given - 1];
	if ( f->state == 1 ) {
		f->answer = (int)se_join( f->handle );
		f->handle = 0;
		se_files_out--;
	}
	f->state = 0;
	return f->answer;
}

// The map's MD4 made on a worker while this core loads its lumps (cm_load.cpp's CM_LoadMap_Actual; docs/games/jk2.md,
// "The load"): Com_BlockChecksum's value, its digest's four words XORed, 112 million instructions of this core's. The
// engine's md4.cpp keeps its state in a static; this one keeps it on the worker's stack. JK2_MD4_CHECK=1 makes both.
#define SE_MD4_F( x, y, z ) ( ( (x) & (y) ) | ( ~(x) & (z) ) )
#define SE_MD4_G( x, y, z ) ( ( (x) & (y) ) | ( (x) & (z) ) | ( (y) & (z) ) )
#define SE_MD4_H( x, y, z ) ( (x) ^ (y) ^ (z) )
#define SE_MD4_ROT( x, s ) ( ( (x) << (s) ) | ( (x) >> ( 32 - (s) ) ) )
#define SE_MD4_R1( a, b, c, d, k, s ) a = SE_MD4_ROT( a + SE_MD4_F( b, c, d ) + X[k], s )
#define SE_MD4_R2( a, b, c, d, k, s ) a = SE_MD4_ROT( a + SE_MD4_G( b, c, d ) + X[k] + 0x5A827999u, s )
#define SE_MD4_R3( a, b, c, d, k, s ) a = SE_MD4_ROT( a + SE_MD4_H( b, c, d ) + X[k] + 0x6ED9EBA1u, s )
static void se_md4_block( uint32_t h[4], const uint8_t *in )
{
	uint32_t copy[16], a = h[0], b = h[1], c = h[2], d = h[3];
	const uint32_t *X = (const uint32_t *)in;	// (the machine's words are little-endian, as MD4's)
	if ( (uintptr_t)in & 3 )
		memcpy( copy, in, 64 ), X = copy;
	SE_MD4_R1( a, b, c, d, 0, 3 ); SE_MD4_R1( d, a, b, c, 1, 7 ); SE_MD4_R1( c, d, a, b, 2, 11 ); SE_MD4_R1( b, c, d, a, 3, 19 );
	SE_MD4_R1( a, b, c, d, 4, 3 ); SE_MD4_R1( d, a, b, c, 5, 7 ); SE_MD4_R1( c, d, a, b, 6, 11 ); SE_MD4_R1( b, c, d, a, 7, 19 );
	SE_MD4_R1( a, b, c, d, 8, 3 ); SE_MD4_R1( d, a, b, c, 9, 7 ); SE_MD4_R1( c, d, a, b, 10, 11 ); SE_MD4_R1( b, c, d, a, 11, 19 );
	SE_MD4_R1( a, b, c, d, 12, 3 ); SE_MD4_R1( d, a, b, c, 13, 7 ); SE_MD4_R1( c, d, a, b, 14, 11 ); SE_MD4_R1( b, c, d, a, 15, 19 );
	SE_MD4_R2( a, b, c, d, 0, 3 ); SE_MD4_R2( d, a, b, c, 4, 5 ); SE_MD4_R2( c, d, a, b, 8, 9 ); SE_MD4_R2( b, c, d, a, 12, 13 );
	SE_MD4_R2( a, b, c, d, 1, 3 ); SE_MD4_R2( d, a, b, c, 5, 5 ); SE_MD4_R2( c, d, a, b, 9, 9 ); SE_MD4_R2( b, c, d, a, 13, 13 );
	SE_MD4_R2( a, b, c, d, 2, 3 ); SE_MD4_R2( d, a, b, c, 6, 5 ); SE_MD4_R2( c, d, a, b, 10, 9 ); SE_MD4_R2( b, c, d, a, 14, 13 );
	SE_MD4_R2( a, b, c, d, 3, 3 ); SE_MD4_R2( d, a, b, c, 7, 5 ); SE_MD4_R2( c, d, a, b, 11, 9 ); SE_MD4_R2( b, c, d, a, 15, 13 );
	SE_MD4_R3( a, b, c, d, 0, 3 ); SE_MD4_R3( d, a, b, c, 8, 9 ); SE_MD4_R3( c, d, a, b, 4, 11 ); SE_MD4_R3( b, c, d, a, 12, 15 );
	SE_MD4_R3( a, b, c, d, 2, 3 ); SE_MD4_R3( d, a, b, c, 10, 9 ); SE_MD4_R3( c, d, a, b, 6, 11 ); SE_MD4_R3( b, c, d, a, 14, 15 );
	SE_MD4_R3( a, b, c, d, 1, 3 ); SE_MD4_R3( d, a, b, c, 9, 9 ); SE_MD4_R3( c, d, a, b, 5, 11 ); SE_MD4_R3( b, c, d, a, 13, 15 );
	SE_MD4_R3( a, b, c, d, 3, 3 ); SE_MD4_R3( d, a, b, c, 11, 9 ); SE_MD4_R3( c, d, a, b, 7, 11 ); SE_MD4_R3( b, c, d, a, 15, 15 );
	h[0] += a, h[1] += b, h[2] += c, h[3] += d;
}

static uint32_t se_md4_job( uint32_t a0, uint32_t a1 )
{
	const uint8_t *in = (const uint8_t *)(uintptr_t)a0;
	uint32_t n = a1, h[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u }, at;
	uint8_t tail[128];
	for ( at = 0; at + 64 <= n; at += 64 )
		se_md4_block( h, in + at );
	uint32_t left = n - at, bits = n * 8, blocks = left <= 55 ? 1 : 2;
	memset( tail, 0, sizeof tail );
	memcpy( tail, in + at, left );
	tail[left] = 0x80;
	memcpy( tail + blocks * 64 - 8, &bits, 4 );	// (md4.cpp's: the length's low word, its high one 0)
	for ( uint32_t k = 0; k < blocks; k++ )
		se_md4_block( h, tail + k * 64 );
	return h[0] ^ h[1] ^ h[2] ^ h[3];
}

// The checksum begun on a worker that has nothing to do: its handle, 0 for none (Com_BlockChecksum here then)
int SE_ChecksumGive( const void *buffer, int length )
{
	if ( !SE_PicturesOn( se_tga_pixels, se_prepare ) || length < 65536 )	// (the load's workers, as the pictures')
		return 0;
	return mcw_try_call( se_md4_job, (uint32_t)(uintptr_t)buffer, (uint32_t)length );
}
uint32_t SE_ChecksumJoin( int given, const void *buffer, int length )
{
	uint32_t value = se_join( given );
	if ( getenv( "JK2_MD4_CHECK" ) ) {
		extern uint32_t Com_BlockChecksum( const void *buffer, int length );
		uint32_t engine = Com_BlockChecksum( buffer, length );
		Com_Printf( "jk2: the map's checksum %08x on a worker, %08x by md4.cpp: %s\n", value, engine, value == engine ? "the same" : "NOT THE SAME" );
	}
	return value;
}

// ---- sounds made side by side (snd_mem.cpp's S_LoadSound_Actual; docs/games/jk2.md, "The load") ----
// A sound's job: an MP3 decoded from its first frame into `pcm` (media_mp3_decode_alone), fitted to the engine's length
// (`pcm_frames`; what it lacks silence), and ResampleSfx's loop from `src` into `out`, on a worker with nothing to do. Its
// answer is 1 + the loudness ResampleSfx finds (sfx_t's fVolRange), 0 when the decode made under half the engine's length
// (a file of a few frames). It stores to pcm and out alone; the record is read only.
#define SE_SOUNDS 256
struct __attribute__((aligned(16))) se_sound_t {
	const void *mp3;	// NULL: src is the file's samples
	uint32_t mp3_size;
	int16_t *pcm;
	uint32_t pcm_frames;
	int channels;
	int downmix;		// a stereo file the engine makes mono: decoded in stereo, its channels' mean (pcm holds both)
	const uint8_t *src;
	int width;		// 1 or 2 bytes a sample
	uint32_t step;		// ResampleSfx's uiFracStep
	int count;		// samples out
	int16_t *out;
	const void *packed;	// a WAV's bytes deflated as they lie in the pak, inflated into `plain` first (src lies in it)
	uint32_t packed_size;
	void *plain;
	uint32_t plain_size;
	int handle;		// this core's: the call, 0 for a free record
};
static se_sound_t se_sounds[SE_SOUNDS];

static uint32_t se_sound_run( const se_sound_t *p, void *room, void *inflate_room )
{
	if ( p->packed ) {
		uint32_t made = 0;
		if ( media_inflate_one( p->packed, p->packed_size, p->plain, p->plain_size, &made, NULL, NULL, inflate_room ) ||
		     made != p->plain_size )
			return 0;	// (made by the first core then: snd_mem.cpp)
	}
	if ( p->mp3 ) {
		const uint32_t ch = p->downmix ? 2 : (uint32_t)p->channels;
		uint32_t made = media_mp3_decode_alone( p->mp3, p->mp3_size, p->pcm, p->pcm_frames, (int)ch, room );
		if ( made * 2 < p->pcm_frames )
			return 0;	// (under half the engine's length: the engine's decode, on the first core)
		if ( made < p->pcm_frames )
			memset( p->pcm + made * ch, 0, ( p->pcm_frames - made ) * ch * 2 );
		if ( p->downmix )
			for ( uint32_t i = 0; i < p->pcm_frames; i++ )
				p->pcm[i] = (int16_t)( ( (int)p->pcm[2 * i] + (int)p->pcm[2 * i + 1] ) / 2 );
	}
	// (ResampleSfx's loop)
	uint32_t frac = 0;
	int most = 0;
	for ( int i = 0; i < p->count; i++ ) {
		int at = (int)( frac >> 8 ), sample;
		frac += p->step;
		if ( p->width == 2 ) {
			int16_t v;
			memcpy( &v, p->src + at * 2, 2 );
			sample = v;
		} else {
			sample = (int)( (unsigned)( (unsigned char)p->src[at] - 128 ) << 8 );
		}
		p->out[i] = (int16_t)sample;
		if ( sample < 0 )
			sample = -sample;
		if ( most < ( sample >> 8 ) )
			most = sample >> 8;
	}
	return 1 + (uint32_t)most;
}
static uint32_t se_sound_job( uint32_t a0, uint32_t a1 )
{
	(void)a1;
	return se_sound_run( (const se_sound_t *)(uintptr_t)a0, se_rooms[mcw_self()].mp3, se_rooms[mcw_self()].inflate );
}

// ResampleSfx's loop alone, on this core (a sound the engine decoded after all): 1 + its loudness
int SE_SoundResample( const void *src, int width, unsigned step, int count, void *out )
{
	se_sound_t p;
	memset( &p, 0, sizeof p );
	p.src = (const uint8_t *)src, p.width = width, p.step = step, p.count = count, p.out = (int16_t *)out;
	return (int)se_sound_run( &p, NULL, NULL );
}

// A deflate stream inflated on this core (a WAV whose worker's inflate failed): whether it made its length
int SE_InflateHere( const void *in, int in_size, void *out, int out_size )
{
	uint32_t made = 0;
	return se_head_room && !media_inflate_one( in, (uint32_t)in_size, out, (uint32_t)out_size, &made, NULL, NULL, se_head_room ) &&
	       made == (uint32_t)out_size;
}

// Whether sounds are made side by side now (as the pictures; JK2_SOUNDS_AHEAD=0 or JK2_SOUNDS=engine the way before)
int SE_SoundsOn( void )
{
	static int on = -1;
	if ( on < 0 )
		on = !se_engine_decodes( "JK2_SOUNDS" ) && env_int( "JK2_SOUNDS_AHEAD", 1 );
	return on && SE_PicturesOn( se_tga_pixels, se_prepare );
}

// An MP3's channels as its first frame says them (after an ID3v2 tag), 0 when none is found in its first 4 KB: the engine
// makes a stereo file mono, which media_mp3_decode_alone does not
int SE_MP3Channels( const void *data, int size )
{
	const uint8_t *f = (const uint8_t *)data;
	uint32_t at = 0, n = (uint32_t)size;
	if ( n > 10 && !memcmp( f, "ID3", 3 ) )
		at = 10 + ( (f[6] & 127u) << 21 | (f[7] & 127u) << 14 | (f[8] & 127u) << 7 | (f[9] & 127u) );
	for ( uint32_t end = at + 4096 < n ? at + 4096 : n; at + 4 <= end; at++ )
		if ( f[at] == 0xff && ( f[at + 1] & 0xe0 ) == 0xe0 && ( f[at + 1] & 0x18 ) != 0x08 && ( f[at + 1] & 0x06 ) != 0 &&
		     ( f[at + 2] & 0xf0 ) != 0xf0 && ( f[at + 2] & 0x0c ) != 0x0c )
			return ( f[at + 3] >> 6 ) == 3 ? 1 : 2;
	return 0;
}

// A sound given to a worker with nothing to do (its handle), or made here at once when none has (0, and *answer)
int SE_SoundGive( const void *mp3, int mp3_size, void *pcm, int pcm_frames, int channels, const void *src, int width,
		  unsigned step, int count, void *out, int *answer, const void *packed, int packed_size, void *plain, int plain_size )
{
	int i;
	for ( i = 0; i < SE_SOUNDS && se_sounds[i].handle; i++ ) {}
	se_sound_t here, *p = i < SE_SOUNDS ? &se_sounds[i] : &here;
	p->mp3 = mp3, p->mp3_size = (uint32_t)mp3_size, p->pcm = (int16_t *)pcm, p->pcm_frames = (uint32_t)pcm_frames;
	// (channels | 0x100: a stereo file made mono, snd_mem.cpp's se_sound_ahead)
	p->channels = channels & 0xff, p->downmix = channels >> 8, p->src = (const uint8_t *)src, p->width = width, p->step = step, p->count = count;
	p->out = (int16_t *)out;
	p->packed = packed, p->packed_size = (uint32_t)packed_size, p->plain = plain, p->plain_size = (uint32_t)plain_size;
	if ( plain )
		mcw_touch( plain, (unsigned)plain_size );
	if ( pcm )
		mcw_touch( pcm, (unsigned)pcm_frames * ( channels >> 8 ? 2u : (unsigned)channels ) * 2 );	// (stored to by this core first: a worker writes them)
	mcw_touch( out, (unsigned)count * 2 );
	int h = p != &here ? mcw_try_call( se_sound_job, (uint32_t)(uintptr_t)p, 0 ) : 0;
	if ( !h && *answer == -1 && p != &here ) {
		return -1;	// (asked not to make it here: the caller joins the sounds through and asks again, snd_mem.cpp)
	}
	if ( !h ) {
		*answer = (int)se_sound_run( p, se_head_room_mp3(), se_head_room );
		return 0;
	}
	p->handle = h;
	return i + 1;
}
int SE_SoundReady( int given ) { return mcw_ready( se_sounds[given - 1].handle ); }
int SE_SoundJoin( int given )
{
	se_sound_t *p = &se_sounds[given - 1];
	int answer = (int)se_join( p->handle );
	p->handle = 0;
	return answer;
}

// A picture saved as a JPEG by libmedia on the workers, for rd-common's RE_SaveJPGToBuffer (the autosave's screenshot;
// docs/games/jk2.md, "The load"): 3 bytes a pixel, rows width * 3 + padding apart, the last first unless `flip`. The
// bytes are libmedia's, in restart intervals, not libjpeg's; the picture no game state reads. Its size, or -1 and libjpeg
// saves it. JK2_PICTURES=engine is the way before.
int SE_JPGEncode( unsigned char *out, int room, int quality, int width, int height, const unsigned char *rgb, int padding, int flip )
{
	if ( se_engine_decodes( "JK2_PICTURES" ) || se_load_workers() <= 0 || width <= 0 || height <= 0 )
		return -1;
	struct media_image image;
	memset( &image, 0, sizeof image );
	image.width = width, image.height = height, image.channels = 3, image.stride = ( width * 3 + 15 ) & ~15;
	if ( posix_memalign( (void **)&image.pixels, 16, (size_t)image.stride * height ) )
		return -1;
	for ( int y = 0; y < height; y++ )
		memcpy( image.pixels + (size_t)y * image.stride, rgb + (size_t)( flip ? y : height - 1 - y ) * ( width * 3 + padding ), (size_t)width * 3 );
	uint32_t size = 0;
	uint8_t *jpeg = media_jpeg_encode( &image, quality, &size, se_load_workers() );
	free( image.pixels );
	int done = jpeg && size <= (uint32_t)room ? (int)size : -1;
	if ( done >= 0 )
		memcpy( out, jpeg, size );
	if ( jpeg )
		media_free( jpeg, size );
	return done;
}

// strtod for every atof and strtod of the program (the link's --wrap=strtod, build.sh; docs/games/jk2.md, "The load"):
// musl's makes every number of the text it parses in long doubles, which this machine has in software. A decimal of a
// mantissa up to 2^53 and a power of ten up to 22 is one exact double times or over an exact power of ten, rounded once:
// strtod's value (Clinger's fast path). Any other text is musl's. JK2_STRTOD_CHECK=1 makes both and says where they differ.
extern "C" double __real_strtod( const char *s, char **end );
extern "C" double __wrap_strtod( const char *s, char **end )
{
	static const double tens[23] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
					 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
	const char *p = s;
	while ( *p == ' ' || ( *p >= '\t' && *p <= '\r' ) )
		p++;
	int negative = *p == '-';
	if ( *p == '-' || *p == '+' )
		p++;
	uint64_t m = 0;
	int digits = 0, exp10 = 0, any = 0;
	for ( ; *p >= '0' && *p <= '9'; p++, any = 1 ) {
		if ( m || *p != '0' )
			digits++;
		if ( digits > 18 )
			return __real_strtod( s, end );
		m = m * 10 + (uint64_t)( *p - '0' );
	}
	if ( *p == '.' ) {
		for ( p++; *p >= '0' && *p <= '9'; p++, any = 1 ) {
			if ( m || *p != '0' )
				digits++;
			if ( digits > 18 )
				return __real_strtod( s, end );
			m = m * 10 + (uint64_t)( *p - '0' ), exp10--;
		}
	}
	if ( !any || ( *p == 'x' || *p == 'X' ) )
		return __real_strtod( s, end );
	if ( *p == 'e' || *p == 'E' ) {
		const char *q = p + 1;
		int eneg = *q == '-', e = 0;
		if ( *q == '-' || *q == '+' )
			q++;
		if ( *q >= '0' && *q <= '9' ) {
			for ( ; *q >= '0' && *q <= '9'; q++ )
				if ( e < 10000 )
					e = e * 10 + ( *q - '0' );
			exp10 += eneg ? -e : e;
			p = q;
		}
	}
	if ( m > ( 1ull << 53 ) || exp10 > 22 || exp10 < -22 )
		return __real_strtod( s, end );
	double v = (double)m;
	v = exp10 >= 0 ? v * tens[exp10] : v / tens[-exp10];
	if ( negative )
		v = -v;
	if ( end )
		*end = (char *)p;
	static int check = -1;
	if ( check < 0 )
		check = getenv( "JK2_STRTOD_CHECK" ) != NULL;
	if ( check ) {
		char *e2;
		double w = __real_strtod( s, &e2 );
		if ( memcmp( &v, &w, sizeof v ) || e2 != p )
			printf( "jk2: strtod NOT THE SAME for \"%.40s\": %.17g against %.17g\n", s, v, w );
	}
	return v;
}
// atof as strtod (the link's --wrap=atof): musl's atof calls its own strtod inside the library, which --wrap=strtod
// does not reach: every atof of the game's spawns and scripts was musl's long doubles (docs/games/jk2.md, "The load")
extern "C" double __wrap_atof( const char *s )
{
	return __wrap_strtod( s, NULL );
}

extern "C" const char *m_jpeg_decode_shrunk( const uint8_t *f, uint32_t size, struct media_image *out, int workers, int shrink );

// A picture's JPEG decoded by libmedia on the workers, for rd-common's LoadJPG (docs/games/jk2.md, "The load"): JPEG was a
// fifth of core 0's load, libjpeg's. `shrink` halvings each way (r_picmip), as libjpeg's scaled IDCT makes them: a side
// (n + 2^shrink - 1) >> shrink. With `pic` NULL the size alone; then RGBA into `pic`, rows width * 4 apart. 0, or -1 and
// libjpeg decodes it. JK2_PICTURES=engine is the way before, JK2_PICTURES_CHECK=1 both and how far apart.
int SE_PictureJPG( const void *data, int size, int shrink, int *width, int *height, unsigned char *pic )
{
	static int engine = -1;
	if ( engine < 0 )
		engine = se_engine_decodes( "JK2_PICTURES" );
	struct media_info info;
	if ( engine || mcw_on_worker() || size <= 0 || shrink < 0 || shrink > 3 ||
	     media_probe( data, (uint32_t)size, &info ) || info.kind != MEDIA_JPEG || !info.width || !info.height )
		return -1;
	int s = 1 << shrink, w = ( info.width + s - 1 ) / s, h = ( info.height + s - 1 ) / s;
	if ( !pic ) {
		*width = w, *height = h;
		return 0;
	}
	if ( *width != w || *height != h )
		return -1;
	struct media_image out;
	memset( &out, 0, sizeof out );
	out.width = w, out.height = h, out.channels = 4, out.stride = w * 4, out.pixels = pic;
	mcw_touch( pic, (unsigned)( w * h * 4 ) );   // (stored to by this core first: the workers write it)
	return m_jpeg_decode_shrunk( (const uint8_t *)data, (uint32_t)size, &out, se_load_workers(), s ) ? -1 : 0;
}
#else
int SE_SoundMP3( const void *data, int size, void *out, int bytes, int rate, int channels ) { return -1; }
int SE_PictureJPG( const void *data, int size, int shrink, int *width, int *height, unsigned char *pic ) { return -1; }
int SE_PicturesOn( void ( *tga )( const void *file, unsigned char *rgba ),
		   int ( *prepare )( unsigned char *rgba, int width, int height, int halvings, int lightscale ) ) { return 0; }
int SE_ChecksumGive( const void *buffer, int length ) { return 0; }
int SE_SoundsOn( void ) { return 0; }
int SE_JPGEncode( unsigned char *out, int room, int quality, int width, int height, const unsigned char *rgb, int padding, int flip ) { return -1; }
int SE_MP3Channels( const void *data, int size ) { return 0; }
int SE_SoundResample( const void *src, int width, unsigned step, int count, void *out ) { return 1; }
int SE_InflateHere( const void *in, int in_size, void *out, int out_size ) { return 0; }
int SE_SoundGive( const void *mp3, int mp3_size, void *pcm, int pcm_frames, int channels, const void *src, int width,
		  unsigned step, int count, void *out, int *answer, const void *packed, int packed_size, void *plain, int plain_size )
{ *answer = 0; return 0; }
int SE_SoundReady( int given ) { return 1; }
int SE_SoundJoin( int given ) { return 1; }
uint32_t SE_ChecksumJoin( int given, const void *buffer, int length ) { return 0; }
int SE_PictureHead( const void *raw, int raw_len, int method, int len, void *head, int room ) { return 0; }
int SE_PictureHeadUntil( const void *raw, int raw_len, int len, void *head, int room, int ( *known )( const unsigned char *head, int got ) ) { return 0; }
int SE_PictureGive( const void *raw, int raw_len, int method, void *plain, int len, int kind, int shrink, void *rgba, int width,
		    int height, int halvings, int lightscale ) { return 0; }
int SE_PictureReady( int given ) { return 1; }
int SE_PictureJoin( int given ) { return 2; }
int SE_FileGive( const void *raw, int raw_len, void *out, int len ) { return 0; }
int SE_FileJoin( int given ) { return 0; }
void SE_PicturesRest( void ) {}
extern "C" int SE_PakInflate( const void *in, unsigned in_size, void *out, unsigned out_size, unsigned *crc ) { return -1; }
int SE_MixWorker( void ) { return 0; }
void SE_MixGive( void ( *fn )( void ) ) { fn(); }
void SE_MixSettle( void ) {}
int SE_MusicOn( void ) { return 0; }
int SE_MusicGive( const void *track, int bytes, int pos, int want, float volume, int raw_time ) { return -1; }
void SE_MusicForget( const void *track ) {}
void SE_MusicRound( int sound_time, int muted ) {}
#endif
