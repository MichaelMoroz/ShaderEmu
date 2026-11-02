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
static int se_win_w = 640, se_win_h = 480;

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
	GrMapWindow( se_window );
	GrSetFocus( se_window );
	if ( jk2glOpen( se_window ) < 0 ) {
		Com_Printf( "jk2: this machine has no GPU: no picture\n" );
		return;
	}
	seglSize( &se_win_w, &se_win_h );
}
#endif

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
static unsigned long long se_window_instructions;

void WIN_Present( window_t *window )
{
	(void)window;
#ifdef SE_WINDOW
	jk2glSwap();
	se_input();
#endif
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
#ifdef SE_WINDOW
		if ( jk2glOn() ) {     // the GPU's memory: textures in it, and how many found no room; vertices drawn since
			unsigned in_gpu, size, left_out, vertices;
			jk2glStats( &in_gpu, &size, &left_out, &vertices );
			Com_Printf( "jk2gpu: textures %u of %u KB, %u left out, %u vertices a frame\n", in_gpu >> 10, size >> 10, left_out,
				vertices / ( se_stats_every > 0 ? se_stats_every : 1 ) );
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
	// mouse1, mouse2, tab, shift, ctrl, a letter or digit)
	static const char *keys = (const char *)-1;
	static int let_go[8], let_go_count;
	if ( keys == (const char *)-1 )
		keys = getenv( "JK2_KEYS" );
	for ( int i = 0; i < let_go_count; i++ )
		Sys_QueEvent( 0, SE_KEY, let_go[i], qfalse, 0, NULL );
	let_go_count = 0;
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
			int key = len == 1 ? name[0] : 0;
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

// The window's keys and buttons as the game's events, and the pointer's travel as its mouse: in a menu its move, in
// the game while the left button is held (the view dragged round, a finger on glass), the right button the fire
// (Quake III's way, linux/quake3/tr_shaderemu.c). The window system is asked when the machine's input words moved
// and a few times a second.
static void se_input( void )
{
	static unsigned seen[4], asked_ms;
	static int ask = 8, mouse_x, mouse_y, mouse_known, mouse_inside = 1, dragging;
	if ( !se_window || !jk2glOn() )
		return;
	const volatile unsigned *input = gpuControl( REG_INPUT ), *clock = gpuControl( REG_CLOCK );
	int menu = ( Key_GetCatcher() & ( KEYCATCH_UI | KEYCATCH_CONSOLE ) ) != 0 || cls.state != CA_ACTIVE;
	if ( mouse_inside || dragging ) {
		int x = (int)input[0], y = (int)input[1];
		if ( mouse_known && ( x != mouse_x || y != mouse_y ) ) {
			if ( menu )
				Sys_QueEvent( 0, SE_MOUSE, ( x - mouse_x ) * 640 / se_win_w, ( y - mouse_y ) * 480 / se_win_h, 0, NULL );
			else if ( dragging )
				Sys_QueEvent( 0, SE_MOUSE, ( x - mouse_x ) * 2, ( y - mouse_y ) * 2, 0, NULL );
		}
		mouse_x = x, mouse_y = y, mouse_known = 1;
	}
	for ( int i = 2; i < 4; i++ )
		if ( seen[i] != input[i] )
			seen[i] = input[i], ask = 8;
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
				seglWindowChanged();
				seglSize( &se_win_w, &se_win_h );
				if ( se_win_w < 1 || se_win_h < 1 )
					se_win_w = 640, se_win_h = 480;
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_BUTTON_UP:
			down = e.type == GR_EVENT_TYPE_BUTTON_DOWN;
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
