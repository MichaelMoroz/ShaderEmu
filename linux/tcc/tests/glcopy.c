/*
 * Copies from the framebuffer into textures (docs/machine/gpu.md, "Copies"): a framebuffer object whose colour is a
 * texture, drawn as a list of its own and drawn from in the window; glCopyTexSubImage2D of the window's corner into a
 * second texture, drawn the next frame; a texture of the program's own pixels, streamed into the VRAM by every list that
 * draws it ("Streams"); and on frame 5 a draw after a copy, which SEGL_DEBUG=1 reports. Compiled in the
 * machine:   cc glcopy.c -o glcopy && SEGL_DEBUG=1 ./glcopy
 * It draws 8 frames and prints "glcopy: done", then stays until it is closed (for a capture).
 */
#include <stdio.h>
#include <stdlib.h>
#define MWINCLUDECOLORS
#include <nano-X.h>
#include <GLES/segl.h>

static const GLfloat corners[] = { 0, 0,  1, 0,  1, 1,  0, 1 };
static const GLfloat triangle[] = { -0.8f, -0.8f,  0.8f, -0.8f,  0, 0.8f };

static void quad(GLfloat x0, GLfloat y0, GLfloat x1, GLfloat y1)
{
	GLfloat at[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
	glVertexPointer(2, GL_FLOAT, 0, at);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

int
main(void)
{
	GR_WINDOW_ID window;
	GR_EVENT event;
	GLuint tex[3], fb;
	static unsigned char checks[16 * 16 * 4];
	int frame, width, height;

	if (GrOpen() < 0)
		return 1;
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Copies", GR_ROOT_WINDOW_ID, 20, 20, 320, 240, BLACK);
	GrSelectEvents(window, GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_EXPOSURE);
	GrMapWindow(window);
	do
		GrGetNextEvent(&event);
	while (event.type != GR_EVENT_TYPE_EXPOSURE);
	if (seglInit(window) < 0)
		return 1;
	glGenTextures(3, tex);
	for (int i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, tex[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	}
	for (int i = 0; i < 16 * 16; i++) {
		int on = ((i % 16) ^ (i / 16)) & 4;
		checks[4 * i] = on ? 240 : 30, checks[4 * i + 1] = on ? 200 : 30, checks[4 * i + 2] = on ? 40 : 90, checks[4 * i + 3] = 255;
	}
	glBindTexture(GL_TEXTURE_2D, tex[2]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, checks);
	glGenFramebuffersOES(1, &fb);
	glBindFramebufferOES(GL_FRAMEBUFFER_OES, fb);
	glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, tex[0], 0);
	printf("glcopy: framebuffer %s\n", glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES) == GL_FRAMEBUFFER_COMPLETE_OES ? "complete" : "incomplete");
	glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0);
	glEnableClientState(GL_VERTEX_ARRAY);
	for (frame = 1;; frame++) {
		seglSize(&width, &height);
		/* the framebuffer: a red square and a green triangle, 64 x 64 */
		glBindFramebufferOES(GL_FRAMEBUFFER_OES, fb);
		glViewport(0, 0, 64, 64);
		glClearColor(0.8f, 0.1f, 0.1f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_TEXTURE_2D);
		glColor4f(0.1f, 0.9f, 0.2f, 1);
		glVertexPointer(2, GL_FLOAT, 0, triangle);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0);
		/* the window: its texture over most of it, and the corner copied the frame before in the top right */
		glViewport(0, 0, width, height);
		glClearColor(0.1f, 0.15f, 0.4f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glEnable(GL_TEXTURE_2D);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glTexCoordPointer(2, GL_FLOAT, 0, corners);
		glColor4f(1, 1, 1, 1);
		glBindTexture(GL_TEXTURE_2D, tex[0]);
		quad(-0.9f, -0.9f, 0.3f, 0.6f);
		glBindTexture(GL_TEXTURE_2D, tex[1]);
		quad(0.45f, 0.45f, 0.95f, 0.95f);
		glBindTexture(GL_TEXTURE_2D, tex[2]);
		quad(0.45f, -0.3f, 0.95f, 0.3f);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		/* the window's bottom left 64 x 64 into the second texture, at the list's end */
		glBindTexture(GL_TEXTURE_2D, tex[1]);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 64, 64);
		if (frame == 5) {
			/* the mistake SEGL_DEBUG reports: a draw after the copy in the same list */
			glDisable(GL_TEXTURE_2D);
			glColor4f(1, 1, 0, 1);
			quad(0.6f, -0.9f, 0.9f, -0.6f);
		}
		seglSwap();
		GrGetNextEventTimeout(&event, 1);
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ || frame == 8)
			break;
	}
	/* (the window's list the last the GPU drew, for a capture of its picture) */
	printf("glcopy: done\n");
	fflush(stdout);
	while (event.type != GR_EVENT_TYPE_CLOSE_REQ)
		GrGetNextEvent(&event);
	return 0;
}
