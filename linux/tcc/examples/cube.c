/*
 * A spinning cube: the smallest OpenGL program for this machine. Compile it here with
 *     cc /usr/share/example-cube.c -o cube
 * Numbers are 16.16 fixed point (the machine has no floating point unit); F() makes one at
 * compile time. The GPU does the matrices and the pixels; this program only says what to draw.
 */
#include <stdlib.h>
#define MWINCLUDECOLORS
#include <nano-X.h>
#include <GLES/segl.h>

#define F(x) ((GLfixed)((x) * 65536))

/* four corners a face, six faces */
static const GLfixed corners[] = {
	F(-1), F(-1), F(1),   F(1), F(-1), F(1),    F(1), F(1), F(1),     F(-1), F(1), F(1),	/* front */
	F(1), F(-1), F(-1),   F(-1), F(-1), F(-1),  F(-1), F(1), F(-1),   F(1), F(1), F(-1),	/* back */
	F(-1), F(-1), F(-1),  F(-1), F(-1), F(1),   F(-1), F(1), F(1),    F(-1), F(1), F(-1),	/* left */
	F(1), F(-1), F(1),    F(1), F(-1), F(-1),   F(1), F(1), F(-1),    F(1), F(1), F(1),	/* right */
	F(-1), F(1), F(1),    F(1), F(1), F(1),     F(1), F(1), F(-1),    F(-1), F(1), F(-1),	/* top */
	F(-1), F(-1), F(-1),  F(1), F(-1), F(-1),   F(1), F(-1), F(1),    F(-1), F(-1), F(1),	/* bottom */
};
static const unsigned char colours[6][3] = {
	{ 230, 70, 60 }, { 60, 170, 90 }, { 70, 110, 230 }, { 240, 200, 60 }, { 200, 90, 220 }, { 70, 200, 220 },
};

int
main(void)
{
	GR_WINDOW_ID window;
	GR_EVENT event;
	int width, height, face, angle = 0;

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
	glVertexPointer(3, GL_FIXED, 0, corners);
	glClearColorx(F(0.05), F(0.06), F(0.10), F(1));
	for (;;) {
		/* 30 frames a second is plenty, and leaves the processor to other programs */
		GrGetNextEventTimeout(&event, 33);
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ)
			break;

		seglSize(&width, &height);
		glViewport(0, 0, width, height);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glFrustumx(-F(1), F(1), -F(1) * height / width, F(1) * height / width, F(2), F(40));
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glTranslatex(0, 0, -F(6));
		glRotatex(angle * F(1), F(1), 0, 0);
		glRotatex(angle * F(1.7), 0, F(1), 0);
		angle = (angle + 2) % 3600;

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
