/*
 * Home: a small place to walk around in, in the spirit of a VRChat home world. Compile with
 *     cc /usr/share/example-home.c -o home
 * Arrows or W A S D walk and turn; Q and E step sideways. The world is a list of boxes, made
 * into quads once; a frame is those quads handed to the GPU, and it is only drawn again when
 * you move, so standing still costs nothing.
 */
#include <stdlib.h>
#define MWINCLUDECOLORS
#include <nano-X.h>
#include <GLES/segl.h>

#define F(x) ((GLfixed)((x) * 65536))
#define MAX_BOXES 96

/* a box: its centre, its size, its colour. Metres; y is up. */
static const struct box { GLfixed x, y, z, w, h, d; unsigned char r, g, b; } boxes[] = {
	/* ground, and the terrace */
	{ F(0), F(-0.1), F(0), F(60), F(0.2), F(60), 74, 122, 74 },
	{ F(0), F(0.05), F(0), F(14), F(0.1), F(12), 176, 164, 150 },
	{ F(0), F(0.12), F(1), F(5), F(0.04), F(4), 150, 60, 60 },		/* rug */
	/* the house: back wall, sides, a roof on four posts */
	{ F(0), F(1.6), F(-6), F(14), F(3.2), F(0.3), 226, 220, 208 },
	{ F(-7), F(1.6), F(-3), F(0.3), F(3.2), F(6), 226, 220, 208 },
	{ F(7), F(1.6), F(-3), F(0.3), F(3.2), F(6), 226, 220, 208 },
	{ F(0), F(3.3), F(-2), F(15), F(0.25), F(9), 120, 84, 60 },
	{ F(-6.5), F(1.6), F(2.2), F(0.3), F(3.2), F(0.3), 120, 84, 60 },
	{ F(6.5), F(1.6), F(2.2), F(0.3), F(3.2), F(0.3), 120, 84, 60 },
	/* a mirror on the back wall, a screen beside it */
	{ F(-3), F(1.7), F(-5.8), F(3.2), F(2.2), F(0.08), 170, 205, 225 },
	{ F(-3), F(1.7), F(-5.82), F(3.4), F(2.4), F(0.06), 60, 60, 66 },
	{ F(3.2), F(1.9), F(-5.8), F(2.6), F(1.5), F(0.08), 30, 40, 70 },
	/* a sofa: seat, back, two arms; a low table */
	{ F(0), F(0.45), F(-1.2), F(3.2), F(0.5), F(1.1), 70, 90, 140 },
	{ F(0), F(0.95), F(-1.65), F(3.2), F(0.6), F(0.3), 62, 80, 126 },
	{ F(-1.75), F(0.6), F(-1.2), F(0.3), F(0.8), F(1.1), 62, 80, 126 },
	{ F(1.75), F(0.6), F(-1.2), F(0.3), F(0.8), F(1.1), 62, 80, 126 },
	{ F(0), F(0.4), F(0.9), F(1.6), F(0.08), F(0.8), 110, 76, 50 },
	{ F(0), F(0.2), F(0.9), F(0.2), F(0.4), F(0.2), 60, 60, 66 },
	/* lamps */
	{ F(-5.5), F(1.0), F(1.5), F(0.1), F(2.0), F(0.1), 60, 60, 66 },
	{ F(-5.5), F(2.1), F(1.5), F(0.5), F(0.4), F(0.5), 255, 236, 180 },
	{ F(5.5), F(1.0), F(1.5), F(0.1), F(2.0), F(0.1), 60, 60, 66 },
	{ F(5.5), F(2.1), F(1.5), F(0.5), F(0.4), F(0.5), 255, 236, 180 },
	/* trees: a trunk and two crowns each */
	{ F(-12), F(1.2), F(6), F(0.5), F(2.4), F(0.5), 96, 66, 44 },
	{ F(-12), F(3.2), F(6), F(2.6), F(1.8), F(2.6), 46, 110, 60 },
	{ F(-12), F(4.6), F(6), F(1.6), F(1.2), F(1.6), 56, 128, 70 },
	{ F(11), F(1.2), F(9), F(0.5), F(2.4), F(0.5), 96, 66, 44 },
	{ F(11), F(3.2), F(9), F(2.6), F(1.8), F(2.6), 46, 110, 60 },
	{ F(11), F(4.6), F(9), F(1.6), F(1.2), F(1.6), 56, 128, 70 },
	{ F(3), F(1.2), F(16), F(0.5), F(2.4), F(0.5), 96, 66, 44 },
	{ F(3), F(3.4), F(16), F(3.0), F(2.2), F(3.0), 46, 110, 60 },
	/* a path, a pond, hills at the horizon */
	{ F(0), F(0.02), F(12), F(2), F(0.04), F(14), 190, 176, 150 },
	{ F(-9), F(0.02), F(13), F(6), F(0.04), F(4), 70, 130, 190 },
	{ F(-20), F(2), F(-24), F(24), F(6), F(8), 88, 120, 96 },
	{ F(14), F(3), F(-26), F(30), F(8), F(8), 80, 112, 90 },
	{ F(28), F(2.5), F(4), F(8), F(7), F(30), 86, 118, 94 },
	{ F(-28), F(2.5), F(6), F(8), F(7), F(34), 84, 116, 92 },
};
#define BOXES ((int)(sizeof boxes / sizeof boxes[0]))

static GLfixed quads[MAX_BOXES * 24 * 3];	/* six faces a box, four corners a face */

/* A box's faces, in the order sides along x, sides along z, top and bottom: each pair gets its
 * own shade of the colour, which is all the lighting there is. */
static void
build(void)
{
	static const signed char face[6][4][3] = {
		{ { -1, -1, -1 }, { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 } }, { { 1, -1, 1 }, { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 } },
		{ { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } },     { { 1, -1, -1 }, { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 } },
		{ { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 }, { -1, 1, -1 } },     { { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 } },
	};
	GLfixed *q = quads;
	int i, f, c;

	for (i = 0; i < BOXES; i++)
		for (f = 0; f < 6; f++)
			for (c = 0; c < 4; c++) {
				*q++ = boxes[i].x + face[f][c][0] * (boxes[i].w / 2);
				*q++ = boxes[i].y + face[f][c][1] * (boxes[i].h / 2);
				*q++ = boxes[i].z + face[f][c][2] * (boxes[i].d / 2);
			}
}

/* The sine of an angle in degrees, 16.16 both: a few terms of its series are enough here. */
static GLfixed
sine(int degrees)
{
	long long d = ((degrees % 360) + 360) % 360, x, x2, s;
	int negative = d >= 180;

	if (negative)
		d -= 180;
	if (d > 90)
		d = 180 - d;
	x = d * 1144;				/* radians: pi / 180 is 1144 / 65536 */
	x2 = x * x >> 16;
	s = x * (65536 - (x2 * (10923 - (x2 * 546 >> 16)) >> 16)) >> 16;
	return (GLfixed)(negative ? -s : s);
}

static void
draw(GLfixed x, GLfixed z, int yaw)
{
	static const int shade[3] = { 205, 160, 255 };	/* sides along x, along z, top */
	int width, height, i, pair;

	seglSize(&width, &height);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustumx(-F(0.1), F(0.1), -F(0.1) * height / width, F(0.1) * height / width, F(0.1), F(120));
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glRotatex(yaw * F(1), 0, F(1), 0);
	glTranslatex(-x, -F(1.6), -z);		/* eyes 1.6 m up */

	glClearColorx(F(0.55), F(0.74), F(0.93), F(1));
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	for (i = 0; i < BOXES; i++)
		for (pair = 0; pair < 3; pair++) {
			glColor4ub(boxes[i].r * shade[pair] / 255, boxes[i].g * shade[pair] / 255, boxes[i].b * shade[pair] / 255, 255);
			glDrawArrays(GL_QUADS, i * 24 + pair * 8, 8);
		}
	seglSwap();
}

int
main(void)
{
	GR_WINDOW_ID window;
	GR_EVENT event;
	GLfixed x = 0, z = F(7), step = F(0.25);
	int yaw = 0, moved = 1;

	if (GrOpen() < 0)
		return 1;
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Home", GR_ROOT_WINDOW_ID, -1, -1, 400, 280, BLACK);
	GrSelectEvents(window, GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_UPDATE);
	GrMapWindow(window);
	GrSetFocus(window);
	do
		GrGetNextEvent(&event);
	while (event.type != GR_EVENT_TYPE_EXPOSURE);
	if (seglInit(window) < 0)
		return 1;
	build();
	glEnable(GL_DEPTH_TEST);
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FIXED, 0, quads);

	for (;;) {
		if (moved)
			draw(x, z, yaw);
		moved = 0;
		GrGetNextEvent(&event);
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ)
			break;
		if (event.type == GR_EVENT_TYPE_EXPOSURE || event.type == GR_EVENT_TYPE_UPDATE)
			moved = 1;
		if (event.type == GR_EVENT_TYPE_KEY_DOWN) {
			/* facing yaw, forward is (sin yaw, -cos yaw) on the ground */
			GLfixed fx = (GLfixed)((long long)sine(yaw) * step >> 16), fz = -(GLfixed)((long long)sine(yaw + 90) * step >> 16);

			moved = 1;
			switch (event.keystroke.ch) {
			case MWKEY_UP: case 'w': x += fx, z += fz; break;
			case MWKEY_DOWN: case 's': x -= fx, z -= fz; break;
			case MWKEY_LEFT: case 'a': yaw = (yaw + 354) % 360; break;
			case MWKEY_RIGHT: case 'd': yaw = (yaw + 6) % 360; break;
			case 'q': x += fz, z -= fx; break;
			case 'e': x -= fz, z += fx; break;
			default: moved = 0;
			}
		}
	}
	GrClose();
	return 0;
}
