/*
 * Home: a small place to walk around in, in the spirit of a VRChat home world. Compile with
 *     cc /usr/share/example-home.c -o home
 * Arrows or W A S D walk and turn; Q and E step sideways. The world is a list of boxes, made
 * into quads once, and drawn again only when you move. Numbers are floats.
 */
#include <math.h>
#include <stdlib.h>
#define MWINCLUDECOLORS
#include <nano-X.h>
#include <GLES/segl.h>

#define MAX_BOXES 96

/* a box: its centre, its size, its colour. Metres; y is up. */
static const struct box { GLfloat x, y, z, w, h, d; unsigned char r, g, b; } boxes[] = {
	/* ground, and the terrace */
	{ 0.0f, -0.1f, 0.0f, 60.0f, 0.2f, 60.0f, 74, 122, 74 },
	{ 0.0f, 0.05f, 0.0f, 14.0f, 0.1f, 12.0f, 176, 164, 150 },
	{ 0.0f, 0.12f, 1.0f, 5.0f, 0.04f, 4.0f, 150, 60, 60 },		/* rug */
	/* the house: back wall, sides, a roof on four posts */
	{ 0.0f, 1.6f, -6.0f, 14.0f, 3.2f, 0.3f, 226, 220, 208 },
	{ -7.0f, 1.6f, -3.0f, 0.3f, 3.2f, 6.0f, 226, 220, 208 },
	{ 7.0f, 1.6f, -3.0f, 0.3f, 3.2f, 6.0f, 226, 220, 208 },
	{ 0.0f, 3.3f, -2.0f, 15.0f, 0.25f, 9.0f, 120, 84, 60 },
	{ -6.5f, 1.6f, 2.2f, 0.3f, 3.2f, 0.3f, 120, 84, 60 },
	{ 6.5f, 1.6f, 2.2f, 0.3f, 3.2f, 0.3f, 120, 84, 60 },
	/* a mirror on the back wall, a screen beside it */
	{ -3.0f, 1.7f, -5.8f, 3.2f, 2.2f, 0.08f, 170, 205, 225 },
	{ -3.0f, 1.7f, -5.82f, 3.4f, 2.4f, 0.06f, 60, 60, 66 },
	{ 3.2f, 1.9f, -5.8f, 2.6f, 1.5f, 0.08f, 30, 40, 70 },
	/* a sofa: seat, back, two arms; a low table */
	{ 0.0f, 0.45f, -1.2f, 3.2f, 0.5f, 1.1f, 70, 90, 140 },
	{ 0.0f, 0.95f, -1.65f, 3.2f, 0.6f, 0.3f, 62, 80, 126 },
	{ -1.75f, 0.6f, -1.2f, 0.3f, 0.8f, 1.1f, 62, 80, 126 },
	{ 1.75f, 0.6f, -1.2f, 0.3f, 0.8f, 1.1f, 62, 80, 126 },
	{ 0.0f, 0.4f, 0.9f, 1.6f, 0.08f, 0.8f, 110, 76, 50 },
	{ 0.0f, 0.2f, 0.9f, 0.2f, 0.4f, 0.2f, 60, 60, 66 },
	/* lamps */
	{ -5.5f, 1.0f, 1.5f, 0.1f, 2.0f, 0.1f, 60, 60, 66 },
	{ -5.5f, 2.1f, 1.5f, 0.5f, 0.4f, 0.5f, 255, 236, 180 },
	{ 5.5f, 1.0f, 1.5f, 0.1f, 2.0f, 0.1f, 60, 60, 66 },
	{ 5.5f, 2.1f, 1.5f, 0.5f, 0.4f, 0.5f, 255, 236, 180 },
	/* trees: a trunk and two crowns each */
	{ -12.0f, 1.2f, 6.0f, 0.5f, 2.4f, 0.5f, 96, 66, 44 },
	{ -12.0f, 3.2f, 6.0f, 2.6f, 1.8f, 2.6f, 46, 110, 60 },
	{ -12.0f, 4.6f, 6.0f, 1.6f, 1.2f, 1.6f, 56, 128, 70 },
	{ 11.0f, 1.2f, 9.0f, 0.5f, 2.4f, 0.5f, 96, 66, 44 },
	{ 11.0f, 3.2f, 9.0f, 2.6f, 1.8f, 2.6f, 46, 110, 60 },
	{ 11.0f, 4.6f, 9.0f, 1.6f, 1.2f, 1.6f, 56, 128, 70 },
	{ 3.0f, 1.2f, 16.0f, 0.5f, 2.4f, 0.5f, 96, 66, 44 },
	{ 3.0f, 3.4f, 16.0f, 3.0f, 2.2f, 3.0f, 46, 110, 60 },
	/* a path, a pond, hills at the horizon */
	{ 0.0f, 0.02f, 12.0f, 2.0f, 0.04f, 14.0f, 190, 176, 150 },
	{ -9.0f, 0.02f, 13.0f, 6.0f, 0.04f, 4.0f, 70, 130, 190 },
	{ -20.0f, 2.0f, -24.0f, 24.0f, 6.0f, 8.0f, 88, 120, 96 },
	{ 14.0f, 3.0f, -26.0f, 30.0f, 8.0f, 8.0f, 80, 112, 90 },
	{ 28.0f, 2.5f, 4.0f, 8.0f, 7.0f, 30.0f, 86, 118, 94 },
	{ -28.0f, 2.5f, 6.0f, 8.0f, 7.0f, 34.0f, 84, 116, 92 },
};
#define BOXES ((int)(sizeof boxes / sizeof boxes[0]))

static GLfloat quads[MAX_BOXES * 24 * 3];	/* six faces a box, four corners a face */

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
	GLfloat *q = quads;
	int i, f, c;

	for (i = 0; i < BOXES; i++)
		for (f = 0; f < 6; f++)
			for (c = 0; c < 4; c++) {
				*q++ = boxes[i].x + (GLfloat)face[f][c][0] * (boxes[i].w * 0.5f);
				*q++ = boxes[i].y + (GLfloat)face[f][c][1] * (boxes[i].h * 0.5f);
				*q++ = boxes[i].z + (GLfloat)face[f][c][2] * (boxes[i].d * 0.5f);
			}
}

static void
draw(GLfloat x, GLfloat z, int yaw)
{
	static const int shade[3] = { 205, 160, 255 };	/* sides along x, along z, top */
	int width, height, i, pair;
	GLfloat tall;

	seglSize(&width, &height);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	tall = 0.1f * (GLfloat)height / (GLfloat)width;
	glFrustumf(-0.1f, 0.1f, -tall, tall, 0.1f, 120.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glRotatef((GLfloat)yaw, 0.0f, 1.0f, 0.0f);
	glTranslatef(-x, -1.6f, -z);		/* eyes 1.6 m up */

	glClearColor(0.55f, 0.74f, 0.93f, 1.0f);
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
	GLfloat x = 0.0f, z = 7.0f, step = 0.25f;
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
	glVertexPointer(3, GL_FLOAT, 0, quads);

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
			GLfloat turn = (GLfloat)yaw * 0.017453292f, fx = sinf(turn) * step, fz = -cosf(turn) * step;

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
