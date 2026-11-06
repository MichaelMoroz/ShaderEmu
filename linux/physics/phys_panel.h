/*
 * nxphys's panel by Dear ImGui (phys_panel.cpp; docs/apps/physics.md, "The panel"): the bar under the picture
 * and a window over it with the boxes' settings and figures, put into each picture nxphys shows (imgui_panel.h),
 * but a holodeck sitter's. What a control asks for is handed back as nxphys's own keys, done by act() as a key
 * would be, and a new gravity or number of boxes.
 */
#ifndef PHYS_PANEL_H
#define PHYS_PANEL_H
#ifdef __cplusplus
extern "C" {
#endif

#define PHYS_FIGURES 64		/* seconds whose figures the plots keep */
#define PHYS_SPEEDS 24		/* the speed histogram's bars */

struct phys_panel {
	/* what it shows */
	int scene, stopped, owners, wanted, workers, most_workers, boxes, heap_boxes, most_boxes;
	float gravity, speed_most;
	const char *status;
	const float *rates, *energy;	/* substeps a second and the boxes' energy (joules), a second each, the oldest first */
	int figure_count;
	const float *speeds;		/* how many boxes in each of PHYS_SPEEDS steps of speed, from 0 to speed_most m/s */
	/* what it asked for: keys of act()'s, the workers to take, a gravity, boxes for the heap (-1, < 0: none) */
	int keys[8], key_count;
	int workers_asked, heap_asked;
	float gravity_asked;
};

int phys_panel_init(unsigned int window);
int phys_panel_bar(void);
/* The panel into the frame being built: 1 when a frame of ImGui's was made (and `p` may ask for something). */
int phys_panel_put(struct phys_panel *p, int wide, int high);

#ifdef __cplusplus
}
#endif
#endif
