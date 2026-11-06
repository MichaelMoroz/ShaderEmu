/*
 * nxray's panel by Dear ImGui (ray_panel.cpp; docs/apps/raytrace.md, "nxray's panel"): the bar under the
 * picture and a window of figures over it, put into each frame nxray shows (imgui_panel.h). What a control
 * asks for is handed back as nxray's own keys, and done by act() as a key would be.
 */
#ifndef RAY_PANEL_H
#define RAY_PANEL_H
#ifdef __cplusplus
extern "C" {
#endif

#define RAY_FIGURES 96		/* pictures whose times the plot keeps */

struct ray_panel {
	/* what it shows */
	int width, height, sizes, size_at;	/* the picture, how many sizes there are and which it is (-1: another) */
	const int (*size_list)[2];
	int depth, depth_most, shadows, stopped, owners;
	int wanted, workers, most_workers, field;	/* asked for, had; on the lanes' field */
	const char *status;
	const float *took;			/* milliseconds of the last pictures, the oldest first */
	int took_count;
	const float *worker_rays;		/* each worker's rays in the last picture (thousands), worker 1 first */
	int worker_count;
	/* what it asked for: keys of act()'s, a size by its number (-1 none), the workers to take (-1 none) */
	int keys[8], key_count;
	int size_asked, workers_asked;
};

int ray_panel_init(unsigned int window);
int ray_panel_bar(void);			/* the bar's height */
/* The panel into the frame being built over the picture: 1 when a frame of ImGui's was made (and so
 * `p` may ask for something). */
int ray_panel_put(struct ray_panel *p, int wide, int high);

#ifdef __cplusplus
}
#endif
#endif
