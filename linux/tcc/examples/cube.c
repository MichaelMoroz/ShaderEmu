/*
 * A spinning cube: the smallest OpenGL program for this machine. Compile it here with
 *     cc /usr/share/example-cube.c -o cube
 * Numbers are floats: write constants with an f (a plain 0.5 is a double, a library call here).
 */
#include <stdlib.h>
#define MWINCLUDECOLORS
#include <nano-X.h>
#include <GLES/segl.h>

/* four corners a face, six faces */
static const GLfloat corners[] = {
	-1, -1, 1,   1, -1, 1,    1, 1, 1,     -1, 1, 1,	/* front */
	1, -1, -1,   -1, -1, -1,  -1, 1, -1,   1, 1, -1,	/* back */
	-1, -1, -1,  -1, -1, 1,   -1, 1, 1,    -1, 1, -1,	/* left */
	1, -1, 1,    1, -1, -1,   1, 1, -1,    1, 1, 1,		/* right */
	-1, 1, 1,    1, 1, 1,     1, 1, -1,    -1, 1, -1,	/* top */
	-1, -1, -1,  1, -1, -1,   1, -1, 1,    -1, -1, 1,	/* bottom */
};
static const unsigned char colours[6][3] = {
	{ 230, 70, 60 }, { 60, 170, 90 }, { 70, 110, 230 }, { 240, 200, 60 }, { 200, 90, 220 }, { 70, 200, 220 },
};

int
main(void)
{
	GR_WINDOW_ID window;
	GR_EVENT event;
	int width, height, face;
	GLfloat angle = 0, tall;

	if (GrOpen() < 0)
		return 1;
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Cube", GR_ROOT_WINDOW_ID, -1, -1, 320, 240, BLACK);
	GrSelectEvents(window, GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_EXPOSURE);
	GrMapWindow(window);
	do
		GrGetNextEvent(&event);
	while (event.type != GR_EVENT_TYPE_EXPOSURE);
	if (seglInit(window) < 0)
		return 1;

	glEnable(GL_DEPTH_TEST);
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FLOAT, 0, corners);
	glClearColor(0.05f, 0.06f, 0.10f, 1.0f);
	for (;;) {
		/* 30 frames a second is plenty, and leaves the processor to other programs */
		GrGetNextEventTimeout(&event, 33);
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ)
			break;

		seglSize(&width, &height);
		tall = (GLfloat)height / (GLfloat)width;
		glViewport(0, 0, width, height);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glFrustumf(-1.0f, 1.0f, -tall, tall, 2.0f, 40.0f);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glTranslatef(0.0f, 0.0f, -6.0f);
		glRotatef(angle, 1.0f, 0.0f, 0.0f);
		glRotatef(angle * 1.7f, 0.0f, 1.0f, 0.0f);
		angle += 2.0f;
		if (angle >= 3600.0f)
			angle -= 3600.0f;

		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		for (face = 0; face < 6; face++) {
			glColor4ub(colours[face][0], colours[face][1], colours[face][2], 255);
			glDrawArrays(GL_QUADS, face * 4, 4);
		}
		seglSwap();
	}
	GrClose();
	return 0;
}
