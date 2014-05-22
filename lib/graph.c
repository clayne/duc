
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#include <libgen.h>

#include <cairo.h>
#include <pango/pangocairo.h>

#include "duc.h"
#include "duc-graph.h"

#define FONT_SIZE_LABEL 8
#define FONT_SIZE_BACK 12

struct label {
	int x, y;
	char *text;
	struct label *next;
};


struct graph {
	struct duc *duc;
	int cx, cy;
	int ring_width;
	int depth;
	struct label *label_list;
	cairo_t *cr;
	double spot_a, spot_r;
};


static void pol2car(struct graph *graph, double a, double r, int *x, int *y)
{
	*x = cos(a) * r + graph->cx;
	*y = sin(a) * r + graph->cy;
}


static void car2pol(struct graph *graph, int x, int y, double *a, double *r)
{
	x -= graph->cx;
	y -= graph->cy;
	*r = hypot(y, x);
	*a = atan2(x, -y) / (M_PI*2);
	if(*a < 0) *a += 1;
}


static void hsv2rgb(double h, double s, double v, double *r, double *g, double *b)
{	
	double f, m, n;
	int i;
	
	h *= 6.0;
	i = floor(h);
	f = (h) - i;
	if (!(i & 1)) f = 1 - f;
	m = v * (1 - s);
	n = v * (1 - s * f);
	if(i<0) i=0;
	if(i>6) i=6;
	switch (i) {
		case 6:
		case 0: *r=v; *g=n, *b=m; break;
		case 1: *r=n; *g=v, *b=m; break;
		case 2: *r=m; *g=v, *b=n; break;
		case 3: *r=m; *g=n, *b=v; break;
		case 4: *r=n; *g=m, *b=v; break;
		case 5: *r=v; *g=m, *b=n; break;
		default: *r=*g=*b=1;
	}
}


static void draw_text(cairo_t *cr, int x, int y, int size, char *text)
{
	char font[32];
	snprintf(font, sizeof font, "Arial, Sans, %d", size);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);

	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	pango_layout_set_text(layout, text, -1);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);

	pango_cairo_update_layout(cr, layout);

	int w,h;
	pango_layout_get_size(layout, &w, &h);

	x -= (w/PANGO_SCALE/2);
	y -= (h/PANGO_SCALE/2);

	cairo_move_to(cr, x, y);
	pango_cairo_layout_path(cr, layout);
	g_object_unref(layout);
	
	/* light grey background */

	cairo_set_line_join (cr, CAIRO_LINE_JOIN_BEVEL);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.4);
	cairo_set_line_width(cr, 3);
	cairo_stroke_preserve(cr);

	/* black text */

	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_set_line_width(cr, 1);
	cairo_fill(cr);
}


double ang(double a)
{
	return -M_PI * 0.5 + M_PI * 2 * a;
}


static void draw_section(struct graph *graph, double a_from, double a_to, int r_from, int r_to, double hue, double brightness)
{
	cairo_t *cr = graph->cr;

	double r = 0.6, g = 0.6, b = 0.6;

	if(brightness > 0) {
		hsv2rgb(hue, 1.0-brightness, brightness/2+0.5, &r, &g, &b);
	}
		
	cairo_new_path(cr);
	cairo_arc(cr, graph->cx, graph->cy, r_from, ang(a_from), ang(a_to));
	cairo_arc_negative(cr, graph->cx, graph->cy, r_to, ang(a_to), ang(a_from));
	cairo_close_path(cr);

	cairo_pattern_t *pat;
	pat = cairo_pattern_create_radial(graph->cx, graph->cy, 0, graph->cx, graph->cy, graph->cx-50);
	cairo_pattern_add_color_stop_rgb(pat, (double)r_from / graph->cx, r*0.5, g*0.5, b*0.5);
	cairo_pattern_add_color_stop_rgb(pat, (double)r_to   / graph->cx, r*1.5, g*1.5, b*1.5);
	cairo_set_source(cr, pat);

	cairo_fill_preserve(cr);
	cairo_pattern_destroy(pat);

	cairo_set_line_width(cr, 0.5);
	cairo_set_source_rgba(cr, 0.2, 0.2, 0.2, 0.9);
	cairo_stroke(cr);
}


static void draw_ring(struct graph *graph, duc_dir *dir, int level, double a_min, double a_max)
{
	double a_range = a_max - a_min;
	double a_from = a_min;
	double a_to = a_min;
			
	double r_from = (level+1) * graph->ring_width;
	double r_to = r_from + graph->ring_width;

	/* Calculate max and total size */
	
	off_t size_total = duc_dirsize(dir);

	struct duc_dirent *e;

	size_t size_min = size_total;
	size_t size_max = 0;

	while( (e = duc_readdir(dir)) != NULL) {
		if(e->size < size_min) size_min = e->size;
		if(e->size > size_max) size_max = e->size;
	}

	duc_rewinddir(dir);
	while( (e = duc_readdir(dir)) != NULL) {

		a_to += a_range * e->size / size_total;

		/* Skip any segments that would be smaller then one pixel */

		if(r_to * (a_to - a_from) * M_PI * 2 < 2) break;
		if(a_to <= a_from) break;

		/* Color of the segment depends on it's relative size to other
		 * objects in the same directory */

		double hue = 0.8 - 0.8 * (double)(e->size - size_min + 1) / (size_max - size_min + 1);
		double brightness = 0.8 * r_from / graph->cx;

		/* Draw section for this object */

		draw_section(graph, a_from, a_to, r_from, r_to, hue, brightness);

		/* Recurse into subdirectories */

		if(e->mode == DUC_MODE_DIR) {
			if(level+1 < graph->depth) {
				duc_dir *dir_child = duc_opendirat(dir, e);
				if(dir_child) {
					draw_ring(graph, dir_child, level + 1, a_from, a_to);
					duc_closedir(dir_child);
				}
			} else {
				draw_section(graph, a_from, a_to, r_to, r_to+5, hue, 0.5);
			}
		}

		/* Place labels if there is enough room to display */

		if(r_from * (a_to - a_from) > 5) {
			struct label *label = malloc(sizeof *label);
			pol2car(graph, ang((a_from+a_to)/2), (r_from+r_to)/2, &label->x, &label->y);
			char siz[32];
			duc_humanize(e->size, siz, sizeof siz);
			int r = asprintf(&label->text, "%s\n%s", e->name, siz);
			if(r > 0) {
				label->next = graph->label_list;
				graph->label_list = label;
			} else {
				free(label);
			}
		}
		
		a_from = a_to;
	}
}


static int find_spot(struct graph *graph, duc_dir *dir, int level, double a_min, double a_max, char *part[])
{
	double a_range = a_max - a_min;
	double a_from = a_min;
	double a_to = a_min;

	/* Calculate max and total size */
	
	off_t size_total = duc_dirsize(dir);

	struct duc_dirent *e;
	while( (e = duc_readdir(dir)) != NULL) {

		a_to += a_range * e->size / size_total;

		if(a_to > a_from) {
			double r_from = (level+1) * graph->ring_width;
			double r_to = r_from + graph->ring_width;
		
			double a = graph->spot_a;
			double r = graph->spot_r;

			if(a >= a_from && a <= a_to && r >= r_from && r <= r_to) {
				part[level] = strdup(e->name);
				return 1;
			}

			if(e->mode == DUC_MODE_DIR) {
				if(level+1 < graph->depth) {
					duc_dir *dir_child = duc_opendirat(dir, e);
					if(dir_child) {
						int r = find_spot(graph, dir_child, level + 1, a_from, a_to, part);
						duc_closedir(dir_child);
						if(r) {
							part[level] = strdup(e->name);
							return 1;
						}
					}
				}
			}

		}
		
		a_from = a_to;
	}

	return 0;
}


static cairo_status_t cairo_writer(void *closure, const unsigned char *data, unsigned int length)
{
	FILE *f = closure;
	fwrite(data, length, 1, f);
	return CAIRO_STATUS_SUCCESS;
}


int duc_graph(duc *duc, duc_dir *dir, int size, int depth, FILE *fout)
{
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	assert(surface);
	cairo_t *cr = cairo_create(surface);
	assert(cr);

	duc_graph_cairo(duc, dir, size, depth, cr);

	cairo_destroy(cr);
	cairo_surface_write_to_png_stream(surface, cairo_writer, fout);
	cairo_surface_destroy(surface);

	return 0;
}


int duc_graph_cairo(duc *duc, duc_dir *dir, int size, int depth, cairo_t *cr)
{
	struct graph graph;
	memset(&graph, 0, sizeof graph);

	graph.duc = duc;
	graph.depth = depth;
	graph.ring_width = ((size-30) / 2) / (graph.depth + 1);
	graph.label_list = NULL;
	graph.cx = size / 2;
	graph.cy = size / 2;

	graph.cr = cr;

	cairo_save(cr);

	/* Recursively draw graph */
	
	duc_rewinddir(dir);
	draw_ring(&graph, dir, 1, 0, 1);

	/* Draw collected labels */

	struct label *label = graph.label_list;
	while(label) {
		draw_text(cr, label->x, label->y, FONT_SIZE_LABEL, label->text);
		free(label->text);
		struct label *next = label->next;
		free(label);
		label = next;
	}

	draw_text(cr, graph.cx, graph.cy, 16, "cd ../");
	
	cairo_restore(cr);

	return 0;
}


int duc_graph_xy_to_path(duc *duc, duc_dir *dir, int size, int depth, int x, int y, char *path, size_t path_len)
{
	struct graph graph;
	memset(&graph, 0, sizeof graph);

	/* If clicked in the center, go up one directory */

	graph.duc = duc;
	graph.depth = depth;
	graph.ring_width = ((size-30) / 2) / (graph.depth + 1);
	graph.label_list = NULL;
	graph.cx = size / 2;
	graph.cy = size / 2;

	double r = hypot(x - size/2, y - size/2) / graph.ring_width;

	if(r < 2) {
		snprintf(path, path_len, "%s", duc_dirpath(dir));
		dirname(path);
		return 1;
	}

	car2pol(&graph, x, y, &graph.spot_a, &graph.spot_r);

	char *part[graph.depth];
	memset(part, 0, sizeof part);

	duc_rewinddir(dir);
	int found = find_spot(&graph, dir, 1, 0, 1, part);
		
	strncpy(path, duc_dirpath(dir), path_len);

	if(found) {

		int i;
		for(i=0; i<graph.depth; i++) {
			if(part[i]) {
				strcat(path, "/");
				strcat(path, part[i]);
				free(part[i]);
			}
		}
	}

	return found;
}


/*
 * End
 */
