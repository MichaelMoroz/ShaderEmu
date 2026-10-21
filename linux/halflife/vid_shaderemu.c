/*
vid_shaderemu.c - the window, its frames and its input on the machine (docs/games/halflife.md)

A Nano-X window drawn by the machine's GPU through gl_shaderemu.c, which ref/gl finds its
OpenGL in. Keys come from the window system's events; the pointer is read from the machine's
input words, as Quake's is (linux/quake/vid_shaderemu.c). Played from a holodeck's seat, the
game reads the host's words (docs/world/holodeck.md, "Played from the seat").
*/
#include "platform/platform.h"
#if XASH_VIDEO == VIDEO_SHADEREMU
#include "input.h"
#include "client.h"
#include "vid_common.h"
#include "mod_local.h"
#include "studio.h"
#include <nano-X.h>
#include <GLES/segl.h>

#define REG_INPUT 0x20		// the machine's input counters (docs/machine/input.md)
#define GAME_HALFLIFE 9		// linux/apps/gameid.h
#define SPLASH "/usr/share/halflife/logo.ppm"

int hlglOpen( unsigned int nano_x_window );
void hlglSwap( void );
void hlglWindowChanged( int *width, int *height );
void *hlglProcAddress( const char *name );
const volatile unsigned *gpuControl( int offset );
void gpuWindowOrigin( int *x, int *y );

static GR_WINDOW_ID window;
static int opened, win_w = 640, win_h = 480;
static int mouse_x, mouse_y, mouse_dx, mouse_dy, mouse_known, mouse_inside = 1, text_input;
static int origin_x, origin_y, origin_known;	// the window's first pixel on the display
static int dragging;	// the left button is held on the game: the pointer's travel turns the view
/* A sitter in a holodeck's seat: 1 with tracked hands, 2 with a mouse. The room is turned by
   `body`; the aim is the hand's from there. */
static int seated;
static float body, aimed;
static float seat_unit;	// the game's units in 4 mm of the room (SE_SeatView)
int se_seat_aiming;	// the right hand aims: the gun is se_seat_gun from the eye (hlsdk.patch's EV_GetGunPosition)
float se_seat_gun[3];
static unsigned mouse_count;

void Platform_Minimize_f( void ) { }

void GL_SwapBuffers( void )
{
	if( opened )
		hlglSwap();
}

// The window, made at once with Uplink's splash in it (the launcher's logo.bmp), while the engine loads.
void SE_Splash( void )
{
	GR_SCREEN_INFO info;
	string size;

	if( window || Sys_CheckParm( "-dedicated" ) || GrOpen() < 0 )
		return;
	GrGetScreenInfo( &info );
	if( Sys_GetParmFromCmdLine( "-width", size ))
		win_w = Q_atoi( size );
	if( Sys_GetParmFromCmdLine( "-height", size ))
		win_h = Q_atoi( size );
	if( win_w > info.cols ) win_w = info.cols;
	if( win_h > info.rows ) win_h = info.rows;
	window = GrNewWindowEx( GR_WM_PROPS_APPWINDOW, "Half-Life: Uplink", GR_ROOT_WINDOW_ID, -1, -1, win_w, win_h, 0 );
	GrSetWindowBackgroundColor( window, GR_RGB( 0, 0, 0 ));
	GrMapWindow( window );
	GrSetFocus( window );
	GrClearWindow( window, 0 );
	if( !Sys_CheckParm( "-nosplash" ))
	{
		GR_GC_ID gc = GrNewGC();
		GrDrawImageFromFile( window, gc, ( win_w - 480 ) / 2, ( win_h - 200 ) / 2, 480, 200, (char *)SPLASH, 0 );
		GrDestroyGC( gc );
	}
	GrFlush();
}

static qboolean open_window( void )
{
	if( opened )
		return true;
	if( !window )
		SE_Splash();
	if( !window )
	{
		Con_Printf( S_ERROR "no Nano-X server (start one: nano-X -p &)\n" );
		return false;
	}
	GrSelectEvents( window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN |
		GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_ENTER | GR_EVENT_MASK_MOUSE_EXIT |
		GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE );
	if( hlglOpen( window ) < 0 )
	{
		Con_Printf( S_ERROR "this machine has no GPU\n" );
		return false;
	}
	opened = 1;
	return true;
}

qboolean R_Init_Video( ref_graphic_apis_t type )
{
	if( type != REF_GL || !open_window( ))
		return false;
	if( !VID_SetMode( ))
		return false;
	ref.dllFuncs.GL_InitExtensions();
	host.renderinfo_changed = false;
	return true;
}

void R_Free_Video( void )
{
	ref.dllFuncs.GL_ClearExtensions();
}

qboolean VID_SetMode( void )
{
	R_ChangeDisplaySettings( win_w, win_h, WINDOW_MODE_WINDOWED );
	return true;
}

rserr_t R_ChangeDisplaySettings( int width, int height, window_mode_t window_mode )
{
	int render_w = win_w, render_h = win_h;

	VID_SetDisplayTransform( &render_w, &render_h );
	R_SaveVideoMode( win_w, win_h, render_w, render_h, false );
	return rserr_ok;
}

int GL_SetAttribute( int attr, int val ) { return 0; }
int GL_GetAttribute( int attr, int *val )
{
	*val = attr == REF_GL_RED_SIZE || attr == REF_GL_GREEN_SIZE || attr == REF_GL_BLUE_SIZE ? 8 : attr == REF_GL_CONTEXT_MAJOR_VERSION ? 1 : 0;
	return 0;
}
int R_MaxVideoModes( void ) { return 0; }
vidmode_t *R_GetVideoMode( int num ) { return NULL; }
void *GL_GetProcAddress( const char *name ) { return hlglProcAddress( name ); }
void GL_UpdateSwapInterval( void ) { ClearBits( gl_vsync.flags, FCVAR_CHANGED ); }
void *SW_LockBuffer( void ) { return NULL; }
void SW_UnlockBuffer( void ) { }
qboolean SW_CreateBuffer( int width, int height, uint *stride, uint *bpp, uint *r, uint *g, uint *b ) { return false; }
ref_window_type_t R_GetWindowHandle( void **handle, ref_window_type_t type ) { return REF_WINDOW_TYPE_NULL; }
void VID_Info_f( void ) { Con_Printf( "Nano-X window %dx%d, the machine's GPU\n", win_w, win_h ); }
platform_orientation_t Platform_GetDisplayOrientation( void ) { return ORIENTATION_LANDSCAPE; }

/* ---- input ---- */

static int engine_key( int ch )
{
	static const struct { int ch, key; } other[] = {
		{ MWKEY_LEFT, K_LEFTARROW }, { MWKEY_RIGHT, K_RIGHTARROW }, { MWKEY_UP, K_UPARROW }, { MWKEY_DOWN, K_DOWNARROW },
		{ MWKEY_ENTER, K_ENTER }, { '\n', K_ENTER }, { MWKEY_KP_ENTER, K_KP_ENTER }, { MWKEY_ESCAPE, K_ESCAPE },
		{ MWKEY_BACKSPACE, K_BACKSPACE }, { MWKEY_TAB, K_TAB }, { MWKEY_INSERT, K_INS }, { MWKEY_DELETE, K_DEL },
		{ MWKEY_HOME, K_HOME }, { MWKEY_END, K_END }, { MWKEY_PAGEUP, K_PGUP }, { MWKEY_PAGEDOWN, K_PGDN },
		{ MWKEY_LSHIFT, K_SHIFT }, { MWKEY_RSHIFT, K_SHIFT }, { MWKEY_LCTRL, K_CTRL }, { MWKEY_RCTRL, K_CTRL },
		{ MWKEY_LALT, K_ALT }, { MWKEY_RALT, K_ALT }, { MWKEY_PAUSE, K_PAUSE },
	};
	static const char shifted[] = ")!@#$%^&*(";
	const char *at;
	size_t i;

	if( ch >= 'A' && ch <= 'Z' )
		return ch - 'A' + 'a';
	if( ch >= MWKEY_F1 && ch <= MWKEY_F12 )
		return K_F1 + ch - MWKEY_F1;
	for( i = 0; i < ARRAYSIZE( other ); i++ )
		if( other[i].ch == ch )
			return other[i].key;
	if( ch > 0 && ch < 128 && ( at = Q_strchr( shifted, ch )) != NULL )
		return '0' + (int)( at - shifted );
	return ch >= 32 && ch < 127 ? ch : 0;
}

static qboolean playing( void )
{
	return cls.state == ca_active && cls.signon == SIGNONS && cls.key_dest == key_game && !UI_IsVisible() && !cl.intermission;
}

/*
 * The window system is asked for events only when the machine's input counters have moved, for
 * a few calls after, and a few times a second: every move of the pointer was an event, two system
 * calls and two task switches. Its place is read from the input words themselves.
 */
void Platform_RunEvents( void )
{
	static unsigned seen[4];
	static double asked;
	static int ask = 8;
	const volatile unsigned *input;
	GR_EVENT e;
	int i, key, down;

	if( !opened )
		return;
	// a menu, the console, a level loading, a film: a picture for a screen, not a world for a room
	seglWorldFlat( !playing( ));
	input = gpuControl( REG_INPUT );
	if( input && ( mouse_inside || dragging ))	// (a drag goes on outside the window)
	{
		int x = (int)input[0], y = (int)input[1];

		if( !origin_known )
			gpuWindowOrigin( &origin_x, &origin_y ), origin_known = 1;
		if( mouse_known && dragging )
			mouse_dx += x - origin_x - mouse_x, mouse_dy += y - origin_y - mouse_y;
		mouse_x = x - origin_x, mouse_y = y - origin_y, mouse_known = 1;
	}
	for( i = 2; input && i < 4; i++ )	// the buttons, and the count of keys
		if( seen[i] != input[i] )
			seen[i] = input[i], ask = 8;
	if( !ask && Sys_DoubleTime() - asked >= 0.25 )
		ask = 1;
	while( ask )
	{
		GrCheckNextEvent( &e );
		switch( e.type )
		{
		case GR_EVENT_TYPE_NONE:
			ask--;
			asked = Sys_DoubleTime();
			return;
		case GR_EVENT_TYPE_MOUSE_ENTER:
			mouse_inside = 1;
			mouse_known = 0;
			break;
		case GR_EVENT_TYPE_MOUSE_EXIT:
			mouse_inside = 0;
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_BUTTON_UP:
			down = e.type == GR_EVENT_TYPE_BUTTON_DOWN;
			origin_x = e.button.rootx - e.button.x, origin_y = e.button.rooty - e.button.y, origin_known = 1;
			mouse_x = e.button.x, mouse_y = e.button.y;
			/* In the game the left button drags the view round, as a finger on glass does, the right
			   one is what the left was (fire) and the middle one the secondary fire. */
			if( playing( ) && !seated )
			{
				if( e.button.changebuttons & GR_BUTTON_L )
					dragging = down, mouse_dx = mouse_dy = 0, mouse_known = 0;	// (measured from the input words alone)
				if( e.button.changebuttons & GR_BUTTON_R ) IN_MouseEvent( 0, down );
				if( e.button.changebuttons & GR_BUTTON_M ) IN_MouseEvent( 1, down );
				break;
			}
			// a sitter fires by the trigger: a click on the room's screen is no shot (their release still counts)
			if( playing( ) && seated && down )
				break;
			dragging = 0;
			if( UI_IsVisible( ))	// the menu's focus where the click is, though no move came before it
				UI_MouseMove( mouse_x * refState.scale_x, mouse_y * refState.scale_y );
			if( e.button.changebuttons & GR_BUTTON_L ) IN_MouseEvent( 0, down );
			if( e.button.changebuttons & GR_BUTTON_R ) IN_MouseEvent( 1, down );
			if( e.button.changebuttons & GR_BUTTON_M ) IN_MouseEvent( 2, down );
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
		case GR_EVENT_TYPE_KEY_UP:
			down = e.type == GR_EVENT_TYPE_KEY_DOWN;
			key = engine_key( e.keystroke.ch );
			if( key )
				Key_Event( key, down );
			if( down && text_input && e.keystroke.ch >= 32 && e.keystroke.ch < 127 )
				CL_CharEvent( e.keystroke.ch );
			break;
		case GR_EVENT_TYPE_UPDATE:
			origin_known = 0;
			if( e.update.utype == GR_UPDATE_SIZE )
			{
				int w = 0, h = 0;
				hlglWindowChanged( &w, &h );
				if( w > 0 && h > 0 && ( w != win_w || h != win_h ))
					win_w = w, win_h = h, R_ChangeDisplaySettings( w, h, WINDOW_MODE_WINDOWED );
			}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			Sys_Quit( "window closed" );
			break;
		}
	}
}

// The view's turn by a drag: a quarter of a degree a pixel, as Quake's (the engine multiplies by m_yaw and sensitivity).
void Platform_MouseMove( float *x, float *y )
{
	*x = *y = 0.0f;
	if( dragging && playing( ))
	{
		float scale = m_yaw.value * Cvar_VariableValue( "sensitivity" );

		scale = scale > 0.0f ? 0.25f / scale : 0.0f;
		*x = mouse_dx * scale;
		*y = mouse_dy * scale * ( m_yaw.value / ( m_pitch.value != 0.0f ? fabs( m_pitch.value ) : m_yaw.value ));
	}
	mouse_dx = mouse_dy = 0;
}

void Platform_EnableTextInput( qboolean enable, int x, int y, int w, int h ) { text_input = enable; }
void Platform_PreCreateMove( void ) { }
void GAME_EXPORT Platform_GetMousePos( int *x, int *y )
{
	if( x ) *x = mouse_x * refState.scale_x;
	if( y ) *y = mouse_y * refState.scale_y;
}
void GAME_EXPORT Platform_SetMousePos( int x, int y ) { }
qboolean Platform_GetMouseGrab( void ) { return false; }
void Platform_SetMouseGrab( qboolean enable ) { }
void Platform_SetCursorType( VGUI_DefaultCursor type ) { }
int Platform_GetClipboardText( char *buffer, size_t size ) { return 0; }
void Platform_SetClipboardText( const char *buffer ) { }

/* ---- the holodeck's seat (docs/world/holodeck.md, "Played from the seat") ---- */

static void pitch_to( float pitch )
{
	cl.viewangles[PITCH] = pitch > 80 ? 80 : pitch < -80 ? -80 : pitch;
}

/* After the client made its move: from a seat with tracked hands the right hand aims and the walk
 * stays the body's; with a mouse its travel turns and the head's tilt is the aim's rise. */
// HL_SCRIPT="T:command|T:command": console commands at the level's seconds T; "look P Y" sets the view's angles
static void script( usercmd_t *cmd )
{
	static char *s;
	static int read;
	static float turning;	// "turn D": D degrees a second from then on

	if( !read++ && getenv( "HL_SCRIPT" ))
		s = strdup( getenv( "HL_SCRIPT" ));
	if( !s || cls.state != ca_active )
		return;
	if( turning )
		cl.viewangles[YAW] = anglemod( cl.viewangles[YAW] + turning * host.frametime ), VectorCopy( cl.viewangles, cmd->viewangles );
	for( char *p = s; p && *p; )
	{
		char *bar = strchr( p, '|' ), *colon = strchr( p, ':' );
		double t = atof( p );
		if( bar ) *bar = 0;
		if( colon && *p != '#' && t <= cl.time )
		{
			*p = '#';	// (done)
			float pitch, yaw;
			fprintf( stderr, "script %.2f: %s\n", cl.time, colon + 1 );
			int n = sscanf( colon + 1, "look %f %f", &pitch, &yaw );
			if( sscanf( colon + 1, "turn %f", &turning ) == 1 )
				;
			else if( n >= 1 )
				cl.viewangles[PITCH] = pitch, cl.viewangles[YAW] = n == 2 ? yaw : cl.viewangles[YAW], VectorCopy( cl.viewangles, cmd->viewangles );
			else
				Cbuf_AddText( colon + 1 ), Cbuf_AddText( "\n" );
		}
		if( bar ) *bar = '|';
		p = bar ? bar + 1 : NULL;
	}
}

void SE_SeatMove( usercmd_t *cmd, qboolean active )
{
	const volatile unsigned *hw = seglHostState();
	script( cmd );
	int now = !hw || !SEGL_HOST_HOLODECK( hw ) ? 0 : SEGL_HOST_HANDS( hw ) ? 1 : SEGL_HOST_KIND( hw ) == 2 ? 2 : 0;
	int play = active && playing( );

	if( now != seated )
	{
		seated = now;
		dragging = 0;
		body = aimed = cl.viewangles[YAW];
		mouse_count = hw ? SEGL_HOST_MOUSE_X( hw ) : 0;
		Cvar_SetValue( "r_nocull", seated ? 1.0f : 0.0f );	// (the room shows what is beside and behind the camera too)
	}
	if( !seated )
	{
		se_seat_aiming = 0;
		return;
	}
	if( seated == 1 )
	{
		float turn = SEGL_HOST_HAND_YAW( hw, 1 ) * ( 360.0f / 65536.0f ), forward = cmd->forwardmove, side = cmd->sidemove;
		float look = SEGL_HOST_STICK( hw, 2 ) / 127.0f, walk = ( turn - SEGL_HOST_HEAD_YAW( hw ) * ( 360.0f / 65536.0f )) * ( M_PI_F / 180 );
		float s = sin( walk ), c = cos( walk );	// the walk goes the way the head looks: so far to the left of the aim

		body += cl.viewangles[YAW] - aimed;	// (what the game turned the aim by since, the body turned)
		if( play )
		{
			body -= ( fabs( look ) < 0.15f ? 0 : look * fabs( look )) * 140 * host.frametime;
			forward += SEGL_HOST_STICK( hw, 1 ) * ( 400 / 127.0f );
			side += SEGL_HOST_STICK( hw, 0 ) * ( 400 / 127.0f );
		}
		body = anglemod( body );
		cl.viewangles[YAW] = aimed = anglemod( body - turn );
		pitch_to( -SEGL_HOST_HAND_PITCH( hw, 1 ) * ( 360.0f / 65536.0f ));
		cmd->forwardmove = forward * c + side * s;
		cmd->sidemove = side * c - forward * s;
	}
	else
	{
		unsigned count = SEGL_HOST_MOUSE_X( hw );

		if( play )
		{
			cl.viewangles[YAW] = anglemod( cl.viewangles[YAW] - 0.066f * (short)( count - mouse_count ));
			// (the keys they walk by come as the left stick: no window need have the keyboard)
			cmd->forwardmove += SEGL_HOST_STICK( hw, 1 ) * ( 400 / 127.0f );
			cmd->sidemove += SEGL_HOST_STICK( hw, 0 ) * ( 400 / 127.0f );
		}
		mouse_count = count;
		pitch_to( -SEGL_HOST_HEAD_PITCH( hw ) * ( 360.0f / 65536.0f ));
	}
	VectorCopy( cl.viewangles, cmd->viewangles );
	// with tracked hands the shot leaves the hand along its beam: where the hand is from the eye, to the
	// server in impact_index 1 and impact_position (hlsdk.patch's CmdStart), to the client's events here
	se_seat_aiming = seated == 1 && seat_unit > 0;
	if( se_seat_aiming )
	{
		vec3_t level = { 0, body, 0 }, f, r, u;
		AngleVectors( level, f, r, u );
		for( int i = 0; i < 3; i++ )
			se_seat_gun[i] = ( f[i] * SEGL_HOST_HAND_Z( hw, 1 ) + r[i] * SEGL_HOST_HAND_X( hw, 1 ) + u[i] * SEGL_HOST_HAND_Y( hw, 1 )) * seat_unit;
		cmd->reserved[0] = 1;	// (the game's impact_index and impact_position)
		memcpy( &cmd->reserved[1], se_seat_gun, sizeof( se_seat_gun ));
	}
	// fire and the secondary fire by the triggers, use by the right grip, jump by the jump button
	// (a desktop's keys for them)
	if( play && SEGL_HOST_HAND_TRIGGER( hw, 1 )) SetBits( cmd->buttons, IN_ATTACK );
	if( play && SEGL_HOST_HAND_TRIGGER( hw, 0 )) SetBits( cmd->buttons, IN_ATTACK2 );
	if( play && SEGL_HOST_HAND_GRIP( hw, 1 )) SetBits( cmd->buttons, IN_USE );
	if( play && SEGL_HOST_JUMP( hw )) SetBits( cmd->buttons, IN_JUMP );
}

// Units a metre of the room's: its plane, 1.28 by 0.8 m, is `at` from the camera (GpuVolumePass.cginc).
static float seat_metre( const volatile unsigned *hw, float znear, float zfar, float fov_x, float fov_y )
{
	float at = znear * pow( zfar / znear, SEGL_HOST_PLANE( hw ) / 255.0f );
	float x = 1.28f / tan( fov_x * ( M_PI_F / 360 )), y = 0.8f / tan( fov_y * ( M_PI_F / 360 ));

	return 2 * at / ( x < y ? x : y );
}

/* After the client worked out the view: the eye for the room's default scale, and for a sitter
 * with tracked hands a level picture turned as the body is, the weapon in the right hand. */
void SE_SeatView( ref_viewpass_t *rvp )
{
	const volatile unsigned *hw = seglHostState();
	cl_entity_t *view = &clgame.viewent;
	float znear = 4, zfar = Q_max( 256.0f, clgame.movevars.zmax * 1.73f ), metre, unit;
	vec3_t level = { 0, body, 0 }, forward, right, up, hand;
	static int said = -1;
	static float fov_x, fov_y, scale_x, scale_y;	// (the tangents made again when the view's angles change)
	static float mark_from = -1, mark_to;	// HL_MARK=FROM,TO: the level's seconds a timing counts (rvc_cpu --count-from)
	static int marked;

	if( mark_from < 0 )
		mark_from = getenv( "HL_MARK" ) && sscanf( getenv( "HL_MARK" ), "%f,%f", &mark_from, &mark_to ) == 2 ? mark_from : 1e9f;
	if( marked >= 0 && cl.time >= mark_from )
	{
		if( !marked++ ) fprintf( stderr, "hlmark: begins at %.2f\n", cl.time );
		else if( cl.time >= mark_to ) fprintf( stderr, "hlmark: ends, %d frames to %.2f\n", marked - 1, cl.time ), marked = -1;
	}
	if( !hw || rvp->fov_x <= 0 || rvp->fov_y <= 0 )
		return;
	if( rvp->fov_x != fov_x || rvp->fov_y != fov_y )
	{
		fov_x = rvp->fov_x, fov_y = rvp->fov_y;
		scale_x = 1 / tan( fov_x * ( M_PI_F / 360 )), scale_y = 1 / tan( fov_y * ( M_PI_F / 360 ));
	}
	// the eye 64 units over the floor (28 over the player's origin, 36 over its feet), as ref/gl's planes;
	// its logarithms only when the planes change (and every 64th frame, should the host clear the word)
	static float eye_far, eye_x, eye_y;
	static unsigned eye_frames;
	if( zfar != eye_far || scale_x != eye_x || scale_y != eye_y || !( eye_frames++ & 63 ))
	{
		eye_far = zfar, eye_x = scale_x, eye_y = scale_y;
		seglWorldEye( GAME_HALFLIFE, 64, znear, zfar, scale_x, scale_y );
	}
	if( said < 0 )
		said = getenv( "HALFLIFE_SEAT" ) != NULL;	// a line every 20 frames: how it is seated, the aim, the body
	if( said && ++said % 20 == 0 )
		Con_Printf( "seat: %d aim %.1f %.1f body %.1f drag %d at %.0f %.0f %.0f eye %08x\n", seated, cl.viewangles[YAW], cl.viewangles[PITCH],
			body, dragging, rvp->vieworigin[0], rvp->vieworigin[1], rvp->vieworigin[2], hw[( 0x6c - 0x360 ) / 4] );
	if( seated != 1 )
		return;
	VectorCopy( level, rvp->viewangles );
	metre = seat_metre( hw, znear, zfar, rvp->fov_x, rvp->fov_y );
	unit = seat_unit = 0.004f * metre;
	AngleVectors( level, forward, right, up );
	for( int i = 0; i < 3; i++ )
		hand[i] = rvp->vieworigin[i] + ( forward[i] * SEGL_HOST_HAND_Z( hw, 1 ) + right[i] * SEGL_HOST_HAND_X( hw, 1 )
			+ up[i] * SEGL_HOST_HAND_Y( hw, 1 )) * unit;
	VectorCopy( hand, view->origin );
	if( view->model && view->model->type == mod_studio )
	{
		// pointing the aim's way (none of the client's bob, sway or autoaim), held a third of its length
		// from its back end with its muzzle on the hand's beam (else the middle of its first sequence's box)
		static model_t *of;
		static vec3_t lo, hi, muzzle;
		static int has_muzzle;
		studiohdr_t *hdr = (studiohdr_t *)Mod_StudioExtradata( view->model );
		vec3_t aim = { cl.viewangles[PITCH], cl.viewangles[YAW], 0 };
		qboolean SE_StudioAttachmentRest( model_t *mod, int sequence, int att, vec3_t out );

		if( of != view->model )
		{
			of = view->model;
			if( hdr && hdr->numseq > 0 )
			{
				mstudioseqdesc_t *seq = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex );
				VectorCopy( seq->bbmin, lo ), VectorCopy( seq->bbmax, hi );
			}
			else VectorClear( lo ), VectorClear( hi );
			has_muzzle = SE_StudioAttachmentRest( view->model, 0, 0, muzzle );
		}
		float gx = lo[0] + ( hi[0] - lo[0] ) * 0.33f, gy = has_muzzle ? muzzle[1] : ( lo[1] + hi[1] ) * 0.5f;
		float gz = has_muzzle ? muzzle[2] : ( lo[2] + hi[2] ) * 0.5f;
		AngleVectors( aim, forward, right, up );
		for( int i = 0; i < 3; i++ )
			view->origin[i] -= forward[i] * gx - right[i] * gy + up[i] * gz;
		VectorSet( view->angles, -aim[PITCH], aim[YAW], 0 );	// (a studio model's pitch is the other way)
		VectorCopy( view->angles, view->curstate.angles ), VectorCopy( view->angles, view->latched.prevangles );
		if( said && said % 20 == 0 )
		{
			vec3_t m, d;	// the muzzle in the world, and how far it is from the beam's line
			for( int i = 0; i < 3; i++ )
				m[i] = view->origin[i] + forward[i] * muzzle[0] - right[i] * muzzle[1] + up[i] * muzzle[2], d[i] = m[i] - hand[i];
			float along = DotProduct( d, forward );
			for( int i = 0; i < 3; i++ )
				d[i] -= forward[i] * along;
			Con_Printf( "seat: gun %.1f %.1f %.1f from the eye, a metre %.1f, muzzle %d %.1f %.1f %.1f; hand %.1f %.1f %.1f way %.3f %.3f %.3f, muzzle %.2f off the beam\n",
				view->origin[0] - rvp->vieworigin[0], view->origin[1] - rvp->vieworigin[1], view->origin[2] - rvp->vieworigin[2], metre,
				has_muzzle, muzzle[0], muzzle[1], muzzle[2], hand[0], hand[1], hand[2], forward[0], forward[1], forward[2], VectorLength( d ));
		}
	}
}
#endif
