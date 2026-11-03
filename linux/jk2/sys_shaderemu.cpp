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
	se_music_workers = mcw_open( env_int( "JK2_WORKERS", 16 ) );
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
#else
int SE_MixWorker( void ) { return 0; }
void SE_MixGive( void ( *fn )( void ) ) { fn(); }
void SE_MixSettle( void ) {}
int SE_MusicOn( void ) { return 0; }
int SE_MusicGive( const void *track, int bytes, int pos, int want, float volume, int raw_time ) { return -1; }
void SE_MusicForget( const void *track ) {}
void SE_MusicRound( int sound_time, int muted ) {}
#endif
