/*
 * A fractal shader written in HLSL, compiled by the machine itself when this program starts and
 * run on the machine's lanes (hlsl.h; docs/system/hlsl.md): /usr/share/example-fractal.hlsl, or
 * the file named on the command line. The shader ray marches a Sierpinski pyramid, the attractor
 * of an iterated function system; this program only says where the eye is.
 *
 * Drag with the mouse, or the arrows (or w a s d), to go round it; the wheel or + and - to come
 * nearer; space stops and goes; q leaves. Change the .hlsl and Run again: the IDE runs any .hlsl
 * file with this program (built as /usr/bin/hlslview), which sets the uniforms of these names it has.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define MWINCLUDECOLORS
#include <nano-X.h>
#include <GLES/segl.h>
#include <mcw.h>
#include <hlsl.h>

#define WIDTH 160	/* (a multiple of four) */
#define HEIGHT 120
#define SCALE 4
#define ONE 65536

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	char *s;

	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	s = malloc(n + 1);
	if (!s || fread(s, 1, n, f) != (size_t)n) { fclose(f); return 0; }
	s[n] = 0;
	fclose(f);
	return s;
}

static unsigned milliseconds(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned)t.tv_sec * 1000u + (unsigned)t.tv_nsec / 1000000u;
}

/* The eye on a ball round the middle, looking at it; the turn of each fold slowly changing. */
static void camera(hlsl_program *p, float yaw, float pitch, float distance, float t)
{
	float eye[3], fwd[3], right[3], up[3], size[2] = { WIDTH, HEIGHT }, turn[9];
	float cp = cosf(pitch), a = 0.18f * sinf(0.05f * t), c = cosf(a), s = sinf(a);

	eye[0] = distance * cp * sinf(yaw), eye[1] = distance * sinf(pitch), eye[2] = -distance * cp * cosf(yaw);
	fwd[0] = -eye[0] / distance, fwd[1] = -eye[1] / distance, fwd[2] = -eye[2] / distance;
	right[0] = cosf(yaw), right[1] = 0, right[2] = sinf(yaw);
	up[0] = fwd[1] * right[2] - fwd[2] * right[1];
	up[1] = fwd[2] * right[0] - fwd[0] * right[2];
	up[2] = fwd[0] * right[1] - fwd[1] * right[0];
	/* a turn by a round the axis (0, 0, 1) */
	turn[0] = c, turn[1] = -s, turn[2] = 0;
	turn[3] = s, turn[4] = c, turn[5] = 0;
	turn[6] = 0, turn[7] = 0, turn[8] = 1;
	hlsl_set(p, "eye", eye, 3);
	hlsl_set(p, "forward", fwd, 3);
	hlsl_set(p, "right", right, 3);
	hlsl_set(p, "up", up, 3);
	hlsl_set(p, "size", size, 2);
	hlsl_set(p, "turn", turn, 9);
	hlsl_set(p, "time", &t, 1);
}

int main(int argc, char **argv)
{
	static const GLfixed whole[16] = { 2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE };
	static const GLfixed corners[12] = { 0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0 }, uv[8] = { 0, 0, ONE, 0, ONE, ONE, 0, ONE };
	const char *path = argc > 1 ? argv[1] : "/usr/share/example-fractal.hlsl";
	char error[256], title[200], *source;
	hlsl_program *prog;
	struct hlsl_stats st;
	uint32_t *pixels[2];
	GR_WINDOW_ID window;
	GR_EVENT event;
	GLuint texture[2];
	int width, height, at = 0, stopped = 0, pictures = 0, dragging = 0, mx = 0, my = 0, frames = 0;
	unsigned since, t0;
	float t = 0, yaw = 0.6f, pitch = 0.35f, distance = 3.6f;

	/* the shader compiled here, at run time, to the machine's own code */
	if (!(source = read_file(path))) {
		printf("fractal: no %s\n", path);
		return 1;
	}
	t0 = milliseconds();
	prog = hlsl_compile(source, "main", error, sizeof error);
	if (!prog) {
		if (!strncmp(error, "line ", 5) && strstr(error, ": "))	/* as the C compiler says it, for the IDE */
			printf("%s:%d: error: %s\n", path, atoi(error + 5), strstr(error, ": ") + 2);
		else
			printf("%s:1: error: %s\n", path, error);
		return 1;
	}
	st = hlsl_stats(prog);
	printf("fractal: %s compiled in %u ms: %u bytes of RISC-V, %u places a lane may stop, %u texels a step keeps, the longest step %u "
	       "instructions\n", path, milliseconds() - t0, st.code_bytes, st.yields, st.texels, st.longest_step);

	if (GrOpen() < 0)
		return 1;
	snprintf(title, sizeof title, "%s: HLSL on the machine's lanes", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, title, GR_ROOT_WINDOW_ID, -1, -1, WIDTH * SCALE, HEIGHT * SCALE, BLACK);
	GrSelectEvents(window, GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN |
				       GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_MOTION);
	GrMapWindow(window);
	do
		GrGetNextEvent(&event);
	while (event.type != GR_EVENT_TYPE_EXPOSURE);
	if (seglInit(window) < 0)
		return 1;
	pixels[0] = seglMemory(WIDTH * HEIGHT * 4);
	pixels[1] = seglMemory(WIDTH * HEIGHT * 4);
	if (!pixels[0] || !pixels[1])
		return 1;
	memset(pixels[0], 0, WIDTH * HEIGHT * 4);
	memset(pixels[1], 0, WIDTH * HEIGHT * 4);
	glGenTextures(2, texture);
	glBindTexture(GL_TEXTURE_2D, texture[0]);
	seglTexturePointer(pixels[0], WIDTH, HEIGHT, GL_RGBA);
	glBindTexture(GL_TEXTURE_2D, texture[1]);
	seglTexturePointer(pixels[1], WIDTH, HEIGHT, GL_RGBA);
	seglSize(&width, &height);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glDisable(GL_DEPTH_TEST);

	/* the field's lanes where the machine has them, else the workers' */
	if (mcw_field_lanes())
		printf("fractal: the field's %d lanes, %d x %d pixels\n", mcw_field_lanes(), WIDTH, HEIGHT);
	else
		printf("fractal: %d worker cores, %d x %d pixels\n", mcw_open(mcw_most()), WIDTH, HEIGHT);
	fflush(stdout);
	since = milliseconds();
	camera(prog, yaw, pitch, distance, t);
	hlsl_draw_begin(prog, pixels[0], WIDTH, HEIGHT);
	for (;;) {
		int moved = 0;

		GrCheckNextEvent(&event);
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ)
			break;
		if (event.type == GR_EVENT_TYPE_KEY_DOWN) {
			switch (event.keystroke.ch) {
			case 'q': goto done;
			case ' ': stopped = !stopped; break;
			case MWKEY_LEFT: case 'a': yaw -= 0.12f; break;
			case MWKEY_RIGHT: case 'd': yaw += 0.12f; break;
			case MWKEY_UP: case 'w': pitch += 0.08f; break;
			case MWKEY_DOWN: case 's': pitch -= 0.08f; break;
			case '+': case '=': distance *= 0.9f; break;
			case '-': distance *= 1.1f; break;
			}
			moved = 1;
		}
		if (event.type == GR_EVENT_TYPE_BUTTON_DOWN) {
			if (event.button.changebuttons & GR_BUTTON_SCROLLUP) distance *= 0.92f;
			else if (event.button.changebuttons & GR_BUTTON_SCROLLDN) distance *= 1.08f;
			else dragging = 1, mx = event.button.x, my = event.button.y;
			moved = 1;
		}
		if (event.type == GR_EVENT_TYPE_BUTTON_UP)
			dragging = 0;
		if (event.type == GR_EVENT_TYPE_MOUSE_MOTION && dragging) {
			yaw += 0.01f * (float)(event.mouse.x - mx);
			pitch += 0.01f * (float)(event.mouse.y - my);
			mx = event.mouse.x, my = event.mouse.y;
			moved = 1;
		}
		if (pitch > 1.5f) pitch = 1.5f;
		if (pitch < -1.5f) pitch = -1.5f;
		if (distance < 1.9f) distance = 1.9f;
		if (distance > 9.0f) distance = 9.0f;
		(void)moved;
		if (!hlsl_draw_ready(prog)) {
			mcw_rest();
			continue;
		}
		/* the picture is whole: shown, and the next begun in the other memory */
		seglQuad(corners, uv, texture[at], 255, 255, 0);
		seglSwapEarly();
		if (!stopped) {
			t += 1.0f;
			if (!dragging) yaw += 0.01f;
		}
		at ^= 1;
		camera(prog, yaw, pitch, distance, t);
		seglSettle();
		hlsl_draw_begin(prog, pixels[at], WIDTH, HEIGHT);
		frames++;
		if (++pictures % 20 == 0) {
			unsigned ms = milliseconds() - since;

			printf("fractal: 20 pictures in %u ms: %u.%u a second\n", ms, ms ? 20000 / ms : 0, ms ? 200000 / ms % 10 : 0);
			fflush(stdout);
			since += ms;
		}
	}
done:
	hlsl_draw_wait(prog);
	mcw_close();
	GrClose();
	hlsl_free(prog);
	return 0;
}
