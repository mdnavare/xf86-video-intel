/*
 * Copyright (c) 2007  David Turner
 * Copyright (c) 2008  M Joonas Pihlaja
 * Copyright (c) 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sna.h"
#include "sna_render.h"
#include "sna_render_inline.h"
#include "sna_trapezoids.h"
#include "fb/fbpict.h"

#include <mipict.h>

#undef FAST_SAMPLES_X
#undef FAST_SAMPLES_Y

#if 0
#define __DBG(x) ErrorF x
#else
#define __DBG(x)
#endif

/* TODO: Emit unantialiased and MSAA triangles. */

#ifndef MAX
#define MAX(x,y) ((x) >= (y) ? (x) : (y))
#endif

#ifndef MIN
#define MIN(x,y) ((x) <= (y) ? (x) : (y))
#endif

#define _GRID_TO_INT_FRAC(t, i, f, m) do {      \
	(i) = (t) / (m);                   \
	(f) = (t) % (m);                   \
	if ((f) < 0) {                     \
		--(i);                     \
		(f) += (m);                \
	}                                  \
} while (0)

#define GRID_AREA (2*SAMPLES_X*SAMPLES_Y)

static inline int pixman_fixed_to_grid_x(pixman_fixed_t v)
{
	return ((int64_t)v * SAMPLES_X) >> 16;
}

static inline int pixman_fixed_to_grid_y(pixman_fixed_t v)
{
	return ((int64_t)v * SAMPLES_Y) >> 16;
}

typedef void (*span_func_t)(struct sna *sna,
			    struct sna_composite_spans_op *op,
			    pixman_region16_t *clip,
			    const BoxRec *box,
			    int coverage);

#if HAS_DEBUG_FULL
static void _assert_pixmap_contains_box(PixmapPtr pixmap, BoxPtr box, const char *function)
{
	if (box->x1 < 0 || box->y1 < 0 ||
	    box->x2 > pixmap->drawable.width ||
	    box->y2 > pixmap->drawable.height)
	{
		ErrorF("%s: damage box is beyond the pixmap: box=(%d, %d), (%d, %d), pixmap=(%d, %d)\n",
		       __FUNCTION__,
		       box->x1, box->y1, box->x2, box->y2,
		       pixmap->drawable.width,
		       pixmap->drawable.height);
		assert(0);
	}
}
#define assert_pixmap_contains_box(p, b) _assert_pixmap_contains_box(p, b, __FUNCTION__)
#else
#define assert_pixmap_contains_box(p, b)
#endif

static void apply_damage(struct sna_composite_op *op, RegionPtr region)
{
	DBG(("%s: damage=%p, region=%ldx[(%d, %d), (%d, %d)]\n",
	     __FUNCTION__, op->damage,
	     (long)REGION_NUM_RECTS(region),
	     region->extents.x1, region->extents.y1,
	     region->extents.x2, region->extents.y2));

	if (op->damage == NULL)
		return;

	RegionTranslate(region, op->dst.x, op->dst.y);

	assert_pixmap_contains_box(op->dst.pixmap, RegionExtents(region));
	sna_damage_add(op->damage, region);
}

static void _apply_damage_box(struct sna_composite_op *op, const BoxRec *box)
{
	BoxRec r;

	r.x1 = box->x1 + op->dst.x;
	r.x2 = box->x2 + op->dst.x;
	r.y1 = box->y1 + op->dst.y;
	r.y2 = box->y2 + op->dst.y;

	assert_pixmap_contains_box(op->dst.pixmap, &r);
	sna_damage_add_box(op->damage, &r);
}

inline static void apply_damage_box(struct sna_composite_op *op, const BoxRec *box)
{
	if (op->damage)
		_apply_damage_box(op, box);
}

#define SAMPLES_X_TO_INT_FRAC(x, i, f) \
	_GRID_TO_INT_FRAC(x, i, f, SAMPLES_X)

#define AREA_TO_FLOAT(c)  ((c) / (float)GRID_AREA)
#define TO_ALPHA(c) (((c)+1) >> 1)

struct quorem {
	int32_t quo;
	int32_t rem;
};

struct edge {
	struct edge *next, *prev;

	int dir;

	int height_left;

	struct quorem x;

	/* Advance of the current x when moving down a subsample line. */
	struct quorem dxdy;
	int dy;

	/* The clipped y of the top of the edge. */
	int ytop;

	/* y2-y1 after orienting the edge downwards.  */
};

/* Number of subsample rows per y-bucket. Must be SAMPLES_Y. */
#define EDGE_Y_BUCKET_HEIGHT SAMPLES_Y
#define EDGE_Y_BUCKET_INDEX(y, ymin) (((y) - (ymin))/EDGE_Y_BUCKET_HEIGHT)

/* A collection of sorted and vertically clipped edges of the polygon.
 * Edges are moved from the polygon to an active list while scan
 * converting. */
struct polygon {
	/* The vertical clip extents. */
	int ymin, ymax;

	/* Array of edges all starting in the same bucket.	An edge is put
	 * into bucket EDGE_BUCKET_INDEX(edge->ytop, polygon->ymin) when
	 * it is added to the polygon. */
	struct edge **y_buckets;
	struct edge *y_buckets_embedded[64];

	struct edge edges_embedded[32];
	struct edge *edges;
	int num_edges;
};

/* A cell records the effect on pixel coverage of polygon edges
 * passing through a pixel.  It contains two accumulators of pixel
 * coverage.
 *
 * Consider the effects of a polygon edge on the coverage of a pixel
 * it intersects and that of the following one.  The coverage of the
 * following pixel is the height of the edge multiplied by the width
 * of the pixel, and the coverage of the pixel itself is the area of
 * the trapezoid formed by the edge and the right side of the pixel.
 *
 * +-----------------------+-----------------------+
 * |                       |                       |
 * |                       |                       |
 * |_______________________|_______________________|
 * |   \...................|.......................|\
 * |    \..................|.......................| |
 * |     \.................|.......................| |
 * |      \....covered.....|.......................| |
 * |       \....area.......|.......................| } covered height
 * |        \..............|.......................| |
 * |uncovered\.............|.......................| |
 * |  area    \............|.......................| |
 * |___________\...........|.......................|/
 * |                       |                       |
 * |                       |                       |
 * |                       |                       |
 * +-----------------------+-----------------------+
 *
 * Since the coverage of the following pixel will always be a multiple
 * of the width of the pixel, we can store the height of the covered
 * area instead.  The coverage of the pixel itself is the total
 * coverage minus the area of the uncovered area to the left of the
 * edge.  As it's faster to compute the uncovered area we only store
 * that and subtract it from the total coverage later when forming
 * spans to blit.
 *
 * The heights and areas are signed, with left edges of the polygon
 * having positive sign and right edges having negative sign.  When
 * two edges intersect they swap their left/rightness so their
 * contribution above and below the intersection point must be
 * computed separately. */
struct cell {
	struct cell *next;
	int x;
	int16_t uncovered_area;
	int16_t covered_height;
};

/* A cell list represents the scan line sparsely as cells ordered by
 * ascending x.  It is geared towards scanning the cells in order
 * using an internal cursor. */
struct cell_list {
	struct cell *cursor;

	/* Points to the left-most cell in the scan line. */
	struct cell head, tail;

	int16_t x1, x2;
	int16_t count, size;
	struct cell *cells;
	struct cell embedded[256];
};

/* The active list contains edges in the current scan line ordered by
 * the x-coordinate of the intercept of the edge and the scan line. */
struct active_list {
	/* Leftmost edge on the current scan line. */
	struct edge head, tail;
};

struct tor {
    struct polygon	polygon[1];
    struct active_list	active[1];
    struct cell_list	coverages[1];

    /* Clip box. */
    int xmin, xmax;
    int ymin, ymax;
};

/* Compute the floored division a/b. Assumes / and % perform symmetric
 * division. */
inline static struct quorem
floored_divrem(int a, int b)
{
	struct quorem qr;
	assert(b>0);
	qr.quo = a/b;
	qr.rem = a%b;
	if (qr.rem < 0) {
		qr.quo -= 1;
		qr.rem += b;
	}
	return qr;
}

/* Compute the floored division (x*a)/b. Assumes / and % perform symmetric
 * division. */
static struct quorem
floored_muldivrem(int32_t x, int32_t a, int32_t b)
{
	struct quorem qr;
	int64_t xa = (int64_t)x*a;
	assert(b>0);
	qr.quo = xa/b;
	qr.rem = xa%b;
	if (qr.rem < 0) {
		qr.quo -= 1;
		qr.rem += b;
	}
	return qr;
}

/* Rewinds the cell list's cursor to the beginning.  After rewinding
 * we're good to cell_list_find() the cell any x coordinate. */
inline static void
cell_list_rewind(struct cell_list *cells)
{
	cells->cursor = &cells->head;
}

static bool
cell_list_init(struct cell_list *cells, int x1, int x2)
{
	cells->tail.next = NULL;
	cells->tail.x = INT_MAX;
	cells->head.x = INT_MIN;
	cells->head.next = &cells->tail;
	cells->head.covered_height = 0;
	cell_list_rewind(cells);
	cells->count = 0;
	cells->x1 = x1;
	cells->x2 = x2;
	cells->size = x2 - x1 + 1;
	cells->cells = cells->embedded;
	if (cells->size > ARRAY_SIZE(cells->embedded))
		cells->cells = malloc(cells->size * sizeof(struct cell));
	return cells->cells != NULL;
}

static void
cell_list_fini(struct cell_list *cells)
{
	if (cells->cells != cells->embedded)
		free(cells->cells);
}

inline static void
cell_list_reset(struct cell_list *cells)
{
	cell_list_rewind(cells);
	cells->head.next = &cells->tail;
	cells->head.covered_height = 0;
	cells->count = 0;
}

inline static struct cell *
cell_list_alloc(struct cell_list *cells,
		struct cell *tail,
		int x)
{
	struct cell *cell;

	assert(cells->count < cells->size);
	cell = cells->cells + cells->count++;
	cell->next = tail->next;
	tail->next = cell;

	cell->x = x;
	cell->covered_height = 0;
	cell->uncovered_area = 0;
	return cell;
}

/* Find a cell at the given x-coordinate.  Returns %NULL if a new cell
 * needed to be allocated but couldn't be.  Cells must be found with
 * non-decreasing x-coordinate until the cell list is rewound using
 * cell_list_rewind(). Ownership of the returned cell is retained by
 * the cell list. */
inline static struct cell *
cell_list_find(struct cell_list *cells, int x)
{
	struct cell *tail;

	if (x >= cells->x2)
		return &cells->tail;

	if (x < cells->x1)
		return &cells->head;

	tail = cells->cursor;
	if (tail->x == x)
		return tail;

	do {
		if (tail->next->x > x)
			break;

		tail = tail->next;
		if (tail->next->x > x)
			break;

		tail = tail->next;
		if (tail->next->x > x)
			break;

		tail = tail->next;
	} while (1);

	if (tail->x != x)
		tail = cell_list_alloc(cells, tail, x);

	return cells->cursor = tail;
}

/* Add a subpixel span covering [x1, x2) to the coverage cells. */
inline static void
cell_list_add_subspan(struct cell_list *cells, int x1, int x2)
{
	struct cell *cell;
	int ix1, fx1;
	int ix2, fx2;

	if (x1 == x2)
		return;

	SAMPLES_X_TO_INT_FRAC(x1, ix1, fx1);
	SAMPLES_X_TO_INT_FRAC(x2, ix2, fx2);

	__DBG(("%s: x1=%d (%d+%d), x2=%d (%d+%d)\n", __FUNCTION__,
	       x1, ix1, fx1, x2, ix2, fx2));

	cell = cell_list_find(cells, ix1);
	if (ix1 != ix2) {
		cell->uncovered_area += 2*fx1;
		++cell->covered_height;

		cell = cell_list_find(cells, ix2);
		cell->uncovered_area -= 2*fx2;
		--cell->covered_height;
	} else
		cell->uncovered_area += 2*(fx1-fx2);
}

inline static void
cell_list_add_span(struct cell_list *cells, int x1, int x2)
{
	struct cell *cell;
	int ix1, fx1;
	int ix2, fx2;

	SAMPLES_X_TO_INT_FRAC(x1, ix1, fx1);
	SAMPLES_X_TO_INT_FRAC(x2, ix2, fx2);

	__DBG(("%s: x1=%d (%d+%d), x2=%d (%d+%d)\n", __FUNCTION__,
	       x1, ix1, fx1, x2, ix2, fx2));

	cell = cell_list_find(cells, ix1);
	if (ix1 != ix2) {
		cell->uncovered_area += 2*fx1*SAMPLES_Y;
		cell->covered_height += SAMPLES_Y;

		cell = cell_list_find(cells, ix2);
		cell->uncovered_area -= 2*fx2*SAMPLES_Y;
		cell->covered_height -= SAMPLES_Y;
	} else
		cell->uncovered_area += 2*(fx1-fx2)*SAMPLES_Y;
}

static void
polygon_fini(struct polygon *polygon)
{
	if (polygon->y_buckets != polygon->y_buckets_embedded)
		free(polygon->y_buckets);

	if (polygon->edges != polygon->edges_embedded)
		free(polygon->edges);
}

static bool
polygon_init(struct polygon *polygon,
	     int num_edges,
	     int ymin,
	     int ymax)
{
	unsigned num_buckets =
		EDGE_Y_BUCKET_INDEX(ymax+EDGE_Y_BUCKET_HEIGHT-1, ymin);

	if (unlikely(ymax - ymin > 0x7FFFFFFFU - EDGE_Y_BUCKET_HEIGHT))
		return false;

	polygon->edges = polygon->edges_embedded;
	polygon->y_buckets = polygon->y_buckets_embedded;

	polygon->num_edges = 0;
	if (num_edges > (int)ARRAY_SIZE(polygon->edges_embedded)) {
		polygon->edges = malloc(sizeof(struct edge)*num_edges);
		if (unlikely(NULL == polygon->edges))
			goto bail_no_mem;
	}

	if (num_buckets >= ARRAY_SIZE(polygon->y_buckets_embedded)) {
		polygon->y_buckets = malloc((1+num_buckets)*sizeof(struct edge *));
		if (unlikely(NULL == polygon->y_buckets))
			goto bail_no_mem;
	}
	memset(polygon->y_buckets, 0, num_buckets * sizeof(struct edge *));
	polygon->y_buckets[num_buckets] = (void *)-1;

	polygon->ymin = ymin;
	polygon->ymax = ymax;
	return true;

bail_no_mem:
	polygon_fini(polygon);
	return false;
}

static void
_polygon_insert_edge_into_its_y_bucket(struct polygon *polygon, struct edge *e)
{
	unsigned ix = EDGE_Y_BUCKET_INDEX(e->ytop, polygon->ymin);
	struct edge **ptail = &polygon->y_buckets[ix];
	e->next = *ptail;
	*ptail = e;
}

inline static void
polygon_add_edge(struct polygon *polygon,
		 int x1, int x2,
		 int y1, int y2,
		 int top, int bottom,
		 int dir)
{
	struct edge *e = &polygon->edges[polygon->num_edges];
	int dx = x2 - x1;
	int dy = y2 - y1;
	int ytop, ybot;
	int ymin = polygon->ymin;
	int ymax = polygon->ymax;

	assert (dy > 0);

	e->dy = dy;
	e->dir = dir;

	ytop = top >= ymin ? top : ymin;
	ybot = bottom <= ymax ? bottom : ymax;
	e->ytop = ytop;
	e->height_left = ybot - ytop;
	if (e->height_left <= 0)
		return;

	if (dx == 0) {
		e->x.quo = x1;
		e->x.rem = 0;
		e->dy = 0;
		e->dxdy.quo = 0;
		e->dxdy.rem = 0;
	} else {
		e->dxdy = floored_divrem(dx, dy);
		if (ytop == y1) {
			e->x.quo = x1;
			e->x.rem = 0;
		} else {
			e->x = floored_muldivrem(ytop - y1, dx, dy);
			e->x.quo += x1;
		}
	}
	e->x.rem -= dy; /* Bias the remainder for faster edge advancement. */

	_polygon_insert_edge_into_its_y_bucket(polygon, e);
	polygon->num_edges++;
}

inline static void
polygon_add_line(struct polygon *polygon,
		 const xPointFixed *p1,
		 const xPointFixed *p2)
{
	struct edge *e = &polygon->edges[polygon->num_edges];
	int dx = p2->x - p1->x;
	int dy = p2->y - p1->y;
	int top, bot;

	if (dy == 0)
		return;

	__DBG(("%s: line=(%d, %d), (%d, %d)\n",
	       __FUNCTION__, (int)p1->x, (int)p1->y, (int)p2->x, (int)p2->y));

	e->dir = 1;
	if (dy < 0) {
		const xPointFixed *t;

		dx = -dx;
		dy = -dy;

		e->dir = -1;

		t = p1;
		p1 = p2;
		p2 = t;
	}
	assert (dy > 0);
	e->dy = dy;

	top = MAX(p1->y, polygon->ymin);
	bot = MIN(p2->y, polygon->ymax);
	if (bot <= top)
		return;

	e->ytop = top;
	e->height_left = bot - top;
	if (e->height_left <= 0)
		return;

	if (dx == 0) {
		e->x.quo = p1->x;
		e->x.rem = -dy;
		e->dxdy.quo = 0;
		e->dxdy.rem = 0;
		e->dy = 0;
	} else {
		e->dxdy = floored_divrem(dx, dy);
		if (top == p1->y) {
			e->x.quo = p1->x;
			e->x.rem = -dy;
		} else {
			e->x = floored_muldivrem(top - p1->y, dx, dy);
			e->x.quo += p1->x;
			e->x.rem -= dy;
		}
	}

	if (polygon->num_edges > 0) {
		struct edge *prev = &polygon->edges[polygon->num_edges-1];
		/* detect degenerate triangles inserted into tristrips */
		if (e->dir == -prev->dir &&
		    e->ytop == prev->ytop &&
		    e->height_left == prev->height_left &&
		    e->x.quo == prev->x.quo &&
		    e->x.rem == prev->x.rem &&
		    e->dxdy.quo == prev->dxdy.quo &&
		    e->dxdy.rem == prev->dxdy.rem) {
			unsigned ix = EDGE_Y_BUCKET_INDEX(e->ytop,
							  polygon->ymin);
			polygon->y_buckets[ix] = prev->next;
			return;
		}
	}

	_polygon_insert_edge_into_its_y_bucket(polygon, e);
	polygon->num_edges++;
}

static void
active_list_reset(struct active_list *active)
{
	active->head.height_left = INT_MAX;
	active->head.x.quo = INT_MIN;
	active->head.dy = 0;
	active->head.prev = NULL;
	active->head.next = &active->tail;
	active->tail.prev = &active->head;
	active->tail.next = NULL;
	active->tail.x.quo = INT_MAX;
	active->tail.height_left = INT_MAX;
	active->tail.dy = 0;
}

static struct edge *
merge_sorted_edges(struct edge *head_a, struct edge *head_b)
{
	struct edge *head, **next, *prev;
	int32_t x;

	if (head_b == NULL)
		return head_a;

	prev = head_a->prev;
	next = &head;
	if (head_a->x.quo <= head_b->x.quo) {
		head = head_a;
	} else {
		head = head_b;
		head_b->prev = prev;
		goto start_with_b;
	}

	do {
		x = head_b->x.quo;
		while (head_a != NULL && head_a->x.quo <= x) {
			prev = head_a;
			next = &head_a->next;
			head_a = head_a->next;
		}

		head_b->prev = prev;
		*next = head_b;
		if (head_a == NULL)
			return head;

start_with_b:
		x = head_a->x.quo;
		while (head_b != NULL && head_b->x.quo <= x) {
			prev = head_b;
			next = &head_b->next;
			head_b = head_b->next;
		}

		head_a->prev = prev;
		*next = head_a;
		if (head_b == NULL)
			return head;
	} while (1);
}

static struct edge *
sort_edges(struct edge  *list,
	   unsigned int  level,
	   struct edge **head_out)
{
	struct edge *head_other, *remaining;
	unsigned int i;

	head_other = list->next;
	if (head_other == NULL) {
		*head_out = list;
		return NULL;
	}

	remaining = head_other->next;
	if (list->x.quo <= head_other->x.quo) {
		*head_out = list;
		head_other->next = NULL;
	} else {
		*head_out = head_other;
		head_other->prev = list->prev;
		head_other->next = list;
		list->prev = head_other;
		list->next = NULL;
	}

	for (i = 0; i < level && remaining; i++) {
		remaining = sort_edges(remaining, i, &head_other);
		*head_out = merge_sorted_edges(*head_out, head_other);
	}

	return remaining;
}

static struct edge *filter(struct edge *edges)
{
	struct edge *e;

	e = edges;
	do {
		struct edge *n = e->next;
		if (e->dir == -n->dir &&
		    e->height_left == n->height_left &&
		    *(uint64_t *)&e->x == *(uint64_t *)&n->x &&
		    *(uint64_t *)&e->dxdy == *(uint64_t *)&n->dxdy) {
			if (e->prev)
				e->prev->next = n->next;
			else
				edges = n->next;
			if (n->next)
				n->next->prev = e->prev;
			else
				break;

			e = n->next;
		} else
			e = e->next;
	} while (e->next);

	return edges;
}

static struct edge *
merge_unsorted_edges(struct edge *head, struct edge *unsorted)
{
	sort_edges(unsorted, UINT_MAX, &unsorted);
	return merge_sorted_edges(head, filter(unsorted));
}

/* Test if the edges on the active list can be safely advanced by a
 * full row without intersections or any edges ending. */
inline static int
can_full_step(struct active_list *active)
{
	const struct edge *e;
	int min_height = INT_MAX;

	assert(active->head.next != &active->tail);
	for (e = active->head.next; &active->tail != e; e = e->next) {
		assert(e->height_left > 0);

		if (e->dy != 0)
			return 0;

		if (e->height_left < min_height) {
			min_height = e->height_left;
			if (min_height < SAMPLES_Y)
				return 0;
		}
	}

	return min_height;
}

inline static void
merge_edges(struct active_list *active, struct edge *edges)
{
	active->head.next = merge_unsorted_edges(active->head.next, edges);
}

inline static void
fill_buckets(struct active_list *active,
	     struct edge *edge,
	     int ymin,
	     struct edge **buckets)
{
	while (edge) {
		struct edge *next = edge->next;
		struct edge **b = &buckets[edge->ytop - ymin];
		if (*b)
			(*b)->prev = edge;
		edge->next = *b;
		edge->prev = NULL;
		*b = edge;
		edge = next;
	}
}

inline static void
nonzero_subrow(struct active_list *active, struct cell_list *coverages)
{
	struct edge *edge = active->head.next;
	int prev_x = INT_MIN;
	int winding = 0, xstart = edge->x.quo;

	cell_list_rewind(coverages);

	while (&active->tail != edge) {
		struct edge *next = edge->next;

		winding += edge->dir;
		if (0 == winding && edge->next->x.quo != edge->x.quo) {
			cell_list_add_subspan(coverages,
					      xstart, edge->x.quo);
			xstart = edge->next->x.quo;
		}

		assert(edge->height_left > 0);
		if (--edge->height_left) {
			if (edge->dy) {
				edge->x.quo += edge->dxdy.quo;
				edge->x.rem += edge->dxdy.rem;
				if (edge->x.rem >= 0) {
					++edge->x.quo;
					edge->x.rem -= edge->dy;
				}
			}

			if (edge->x.quo < prev_x) {
				struct edge *pos = edge->prev;
				pos->next = next;
				next->prev = pos;
				do {
					pos = pos->prev;
				} while (edge->x.quo < pos->x.quo);
				pos->next->prev = edge;
				edge->next = pos->next;
				edge->prev = pos;
				pos->next = edge;
			} else
				prev_x = edge->x.quo;
		} else {
			edge->prev->next = next;
			next->prev = edge->prev;
		}

		edge = next;
	}
}

static void
nonzero_row(struct active_list *active, struct cell_list *coverages)
{
	struct edge *left = active->head.next;

	while (&active->tail != left) {
		struct edge *right;
		int winding = left->dir;

		left->height_left -= SAMPLES_Y;
		assert(left->height_left >= 0);
		if (!left->height_left) {
			left->prev->next = left->next;
			left->next->prev = left->prev;
		}

		right = left->next;
		do {
			right->height_left -= SAMPLES_Y;
			assert(right->height_left >= 0);
			if (!right->height_left) {
				right->prev->next = right->next;
				right->next->prev = right->prev;
			}

			winding += right->dir;
			if (0 == winding)
				break;

			right = right->next;
		} while (1);

		cell_list_add_span(coverages, left->x.quo, right->x.quo);
		left = right->next;
	}
}

static void
tor_fini(struct tor *converter)
{
	polygon_fini(converter->polygon);
	cell_list_fini(converter->coverages);
}

static bool
tor_init(struct tor *converter, const BoxRec *box, int num_edges)
{
	__DBG(("%s: (%d, %d),(%d, %d) x (%d, %d), num_edges=%d\n",
	       __FUNCTION__,
	       box->x1, box->y1, box->x2, box->y2,
	       SAMPLES_X, SAMPLES_Y,
	       num_edges));

	converter->xmin = box->x1;
	converter->ymin = box->y1;
	converter->xmax = box->x2;
	converter->ymax = box->y2;

	if (!cell_list_init(converter->coverages, box->x1, box->x2))
		return false;

	active_list_reset(converter->active);
	if (!polygon_init(converter->polygon, num_edges,
			  (int)box->y1 * SAMPLES_Y, (int)box->y2 * SAMPLES_Y)) {
		cell_list_fini(converter->coverages);
		return false;
	}

	return true;
}

static void
tor_add_edge(struct tor *converter,
	     const xTrapezoid *t,
	     const xLineFixed *edge,
	     int dir)
{
	polygon_add_edge(converter->polygon,
			 edge->p1.x, edge->p2.x,
			 edge->p1.y, edge->p2.y,
			 t->top, t->bottom,
			 dir);
}

static void
step_edges(struct active_list *active, int count)
{
	struct edge *edge;

	count *= SAMPLES_Y;
	for (edge = active->head.next; edge != &active->tail; edge = edge->next) {
		edge->height_left -= count;
		assert(edge->height_left >= 0);
		if (!edge->height_left) {
			edge->prev->next = edge->next;
			edge->next->prev = edge->prev;
		}
	}
}

static void
tor_blt_span(struct sna *sna,
	     struct sna_composite_spans_op *op,
	     pixman_region16_t *clip,
	     const BoxRec *box,
	     int coverage)
{
	__DBG(("%s: %d -> %d @ %d\n", __FUNCTION__, box->x1, box->x2, coverage));

	op->box(sna, op, box, AREA_TO_FLOAT(coverage));
	apply_damage_box(&op->base, box);
}

static void
tor_blt_span__no_damage(struct sna *sna,
			struct sna_composite_spans_op *op,
			pixman_region16_t *clip,
			const BoxRec *box,
			int coverage)
{
	__DBG(("%s: %d -> %d @ %d\n", __FUNCTION__, box->x1, box->x2, coverage));

	op->box(sna, op, box, AREA_TO_FLOAT(coverage));
}

static void
tor_blt_span_clipped(struct sna *sna,
		     struct sna_composite_spans_op *op,
		     pixman_region16_t *clip,
		     const BoxRec *box,
		     int coverage)
{
	pixman_region16_t region;
	float opacity;

	opacity = AREA_TO_FLOAT(coverage);
	__DBG(("%s: %d -> %d @ %f\n", __FUNCTION__, box->x1, box->x2, opacity));

	pixman_region_init_rects(&region, box, 1);
	RegionIntersect(&region, &region, clip);
	if (REGION_NUM_RECTS(&region)) {
		op->boxes(sna, op,
			  REGION_RECTS(&region),
			  REGION_NUM_RECTS(&region),
			  opacity);
		apply_damage(&op->base, &region);
	}
	pixman_region_fini(&region);
}

static void
tor_blt(struct sna *sna,
	struct sna_composite_spans_op *op,
	pixman_region16_t *clip,
	void (*span)(struct sna *sna,
		     struct sna_composite_spans_op *op,
		     pixman_region16_t *clip,
		     const BoxRec *box,
		     int coverage),
	struct cell_list *cells,
	int y, int height,
	int xmin, int xmax,
	int unbounded)
{
	struct cell *cell;
	BoxRec box;
	int cover;

	box.y1 = y;
	box.y2 = y + height;
	box.x1 = xmin;

	/* Form the spans from the coverages and areas. */
	cover = cells->head.covered_height*SAMPLES_X*2;
	assert(cover >= 0);
	for (cell = cells->head.next; cell != &cells->tail; cell = cell->next) {
		int x = cell->x;

		assert(x >= xmin);
		assert(x < xmax);
		__DBG(("%s: cell=(%d, %d, %d), cover=%d, max=%d\n", __FUNCTION__,
		       cell->x, cell->covered_height, cell->uncovered_area,
		       cover, xmax));

		if (cell->covered_height || cell->uncovered_area) {
			box.x2 = x;
			if (box.x2 > box.x1 && (unbounded || cover)) {
				__DBG(("%s: span (%d, %d)x(%d, %d) @ %d\n", __FUNCTION__,
				       box.x1, box.y1,
				       box.x2 - box.x1,
				       box.y2 - box.y1,
				       cover));
				span(sna, op, clip, &box, cover);
			}
			box.x1 = box.x2;
			cover += cell->covered_height*SAMPLES_X*2;
		}

		if (cell->uncovered_area) {
			int area = cover - cell->uncovered_area;
			box.x2 = x + 1;
			if (unbounded || area) {
				__DBG(("%s: span (%d, %d)x(%d, %d) @ %d\n", __FUNCTION__,
				       box.x1, box.y1,
				       box.x2 - box.x1,
				       box.y2 - box.y1,
				       area));
				span(sna, op, clip, &box, area);
			}
			box.x1 = box.x2;
		}
	}

	box.x2 = xmax;
	if (box.x2 > box.x1 && (unbounded || cover)) {
		__DBG(("%s: span (%d, %d)x(%d, %d) @ %d\n", __FUNCTION__,
		       box.x1, box.y1,
		       box.x2 - box.x1,
		       box.y2 - box.y1,
		       cover));
		span(sna, op, clip, &box, cover);
	}
}

static void
tor_blt_empty(struct sna *sna,
	      struct sna_composite_spans_op *op,
	      pixman_region16_t *clip,
	      void (*span)(struct sna *sna,
			   struct sna_composite_spans_op *op,
			   pixman_region16_t *clip,
			   const BoxRec *box,
			   int coverage),
	      int y, int height,
	      int xmin, int xmax)
{
	BoxRec box;

	box.x1 = xmin;
	box.x2 = xmax;
	box.y1 = y;
	box.y2 = y + height;

	span(sna, op, clip, &box, 0);
}

flatten static void
tor_render(struct sna *sna,
	   struct tor *converter,
	   struct sna_composite_spans_op *op,
	   pixman_region16_t *clip,
	   void (*span)(struct sna *sna,
			struct sna_composite_spans_op *op,
			pixman_region16_t *clip,
			const BoxRec *box,
			int coverage),
	   int unbounded)
{
	int ymin = converter->ymin;
	int xmin = converter->xmin;
	int xmax = converter->xmax;
	int i, j, h = converter->ymax - ymin;
	struct polygon *polygon = converter->polygon;
	struct cell_list *coverages = converter->coverages;
	struct active_list *active = converter->active;
	struct edge *buckets[SAMPLES_Y] = { 0 };

	__DBG(("%s: unbounded=%d\n", __FUNCTION__, unbounded));

	/* Render each pixel row. */
	for (i = 0; i < h; i = j) {
		int do_full_step = 0;

		j = i + 1;

		/* Determine if we can ignore this row or use the full pixel
		 * stepper. */
		if (!polygon->y_buckets[i]) {
			if (active->head.next == &active->tail) {
				for (; !polygon->y_buckets[j]; j++)
					;
				__DBG(("%s: no new edges and no exisiting edges, skipping, %d -> %d\n",
				       __FUNCTION__, i, j));

				if (unbounded)
					tor_blt_empty(sna, op, clip, span, i+ymin, j-i, xmin, xmax);
				continue;
			}

			do_full_step = can_full_step(active);
		}

		__DBG(("%s: y=%d [%d], do_full_step=%d, new edges=%d\n",
		       __FUNCTION__,
		       i, i+ymin, do_full_step,
		       polygon->y_buckets[i] != NULL));
		if (do_full_step) {
			nonzero_row(active, coverages);

			while (polygon->y_buckets[j] == NULL &&
			       do_full_step >= 2*SAMPLES_Y) {
				do_full_step -= SAMPLES_Y;
				j++;
			}
			if (j != i + 1)
				step_edges(active, j - (i + 1));

			__DBG(("%s: vertical edges, full step (%d, %d)\n",
			       __FUNCTION__,  i, j));
		} else {
			int suby;

			fill_buckets(active, polygon->y_buckets[i], (i+ymin)*SAMPLES_Y, buckets);

			/* Subsample this row. */
			for (suby = 0; suby < SAMPLES_Y; suby++) {
				if (buckets[suby]) {
					merge_edges(active, buckets[suby]);
					buckets[suby] = NULL;
				}

				nonzero_subrow(active, coverages);
			}
		}

		tor_blt(sna, op, clip, span, coverages,
			i+ymin, j-i, xmin, xmax,
			unbounded);
		cell_list_reset(coverages);
	}
}

static void
inplace_row(struct active_list *active, uint8_t *row, int width)
{
	struct edge *left = active->head.next;

	while (&active->tail != left) {
		struct edge *right;
		int winding = left->dir;
		int lfx, rfx;
		int lix, rix;

		left->height_left -= SAMPLES_Y;
		assert(left->height_left >= 0);
		if (!left->height_left) {
			left->prev->next = left->next;
			left->next->prev = left->prev;
		}

		right = left->next;
		do {
			right->height_left -= SAMPLES_Y;
			assert(right->height_left >= 0);
			if (!right->height_left) {
				right->prev->next = right->next;
				right->next->prev = right->prev;
			}

			winding += right->dir;
			if (0 == winding && right->x.quo != right->next->x.quo)
				break;

			right = right->next;
		} while (1);

		if (left->x.quo < 0) {
			lix = lfx = 0;
		} else if (left->x.quo >= width * SAMPLES_X) {
			lix = width;
			lfx = 0;
		} else
			SAMPLES_X_TO_INT_FRAC(left->x.quo, lix, lfx);

		if (right->x.quo < 0) {
			rix = rfx = 0;
		} else if (right->x.quo >= width * SAMPLES_X) {
			rix = width;
			rfx = 0;
		} else
			SAMPLES_X_TO_INT_FRAC(right->x.quo, rix, rfx);
		if (lix == rix) {
			if (rfx != lfx) {
				assert(lix < width);
				row[lix] += (rfx-lfx) * SAMPLES_Y;
			}
		} else {
			assert(lix < width);
			if (lfx == 0)
				row[lix] = 0xff;
			else
				row[lix] += 255 - lfx * SAMPLES_Y;

			assert(rix <= width);
			if (rfx) {
				assert(rix < width);
				row[rix] += rfx * SAMPLES_Y;
			}

			if (rix > ++lix) {
				uint8_t *r = row + lix;
				rix -= lix;
#if 0
				if (rix == 1)
					*row = 0xff;
				else
					memset(row, 0xff, rix);
#else
				if ((uintptr_t)r & 1 && rix) {
					*r++ = 0xff;
					rix--;
				}
				if ((uintptr_t)r & 2 && rix >= 2) {
					*(uint16_t *)r = 0xffff;
					r += 2;
					rix -= 2;
				}
				if ((uintptr_t)r & 4 && rix >= 4) {
					*(uint32_t *)r = 0xffffffff;
					r += 4;
					rix -= 4;
				}
				while (rix >= 8) {
					*(uint64_t *)r = 0xffffffffffffffff;
					r += 8;
					rix -= 8;
				}
				if (rix & 4) {
					*(uint32_t *)r = 0xffffffff;
					r += 4;
				}
				if (rix & 2) {
					*(uint16_t *)r = 0xffff;
					r += 2;
				}
				if (rix & 1)
					*r = 0xff;
#endif
			}
		}

		left = right->next;
	}
}

inline static void
inplace_subrow(struct active_list *active, int8_t *row, int width)
{
	struct edge *edge = active->head.next;
	int prev_x = INT_MIN;

	while (&active->tail != edge) {
		struct edge *next = edge->next;
		int winding = edge->dir;
		int lfx, rfx;
		int lix, rix;

		if (edge->x.quo < 0) {
			lix = lfx = 0;
		} else if (edge->x.quo >= width * SAMPLES_X) {
			lix = width;
			lfx = 0;
		} else
			SAMPLES_X_TO_INT_FRAC(edge->x.quo, lix, lfx);

		assert(edge->height_left > 0);
		if (--edge->height_left) {
			if (edge->dy) {
				edge->x.quo += edge->dxdy.quo;
				edge->x.rem += edge->dxdy.rem;
				if (edge->x.rem >= 0) {
					++edge->x.quo;
					edge->x.rem -= edge->dy;
				}
			}

			if (edge->x.quo < prev_x) {
				struct edge *pos = edge->prev;
				pos->next = next;
				next->prev = pos;
				do {
					pos = pos->prev;
				} while (edge->x.quo < pos->x.quo);
				pos->next->prev = edge;
				edge->next = pos->next;
				edge->prev = pos;
				pos->next = edge;
			} else
				prev_x = edge->x.quo;
		} else {
			edge->prev->next = next;
			next->prev = edge->prev;
		}

		edge = next;
		do {
			next = edge->next;
			winding += edge->dir;
			if (0 == winding && edge->x.quo != next->x.quo)
				break;

			assert(edge->height_left > 0);
			if (--edge->height_left) {
				if (edge->dy) {
					edge->x.quo += edge->dxdy.quo;
					edge->x.rem += edge->dxdy.rem;
					if (edge->x.rem >= 0) {
						++edge->x.quo;
						edge->x.rem -= edge->dy;
					}
				}

				if (edge->x.quo < prev_x) {
					struct edge *pos = edge->prev;
					pos->next = next;
					next->prev = pos;
					do {
						pos = pos->prev;
					} while (edge->x.quo < pos->x.quo);
					pos->next->prev = edge;
					edge->next = pos->next;
					edge->prev = pos;
					pos->next = edge;
				} else
					prev_x = edge->x.quo;
			} else {
				edge->prev->next = next;
				next->prev = edge->prev;
			}

			edge = next;
		} while (1);

		if (edge->x.quo < 0) {
			rix = rfx = 0;
		} else if (edge->x.quo >= width * SAMPLES_X) {
			rix = width;
			rfx = 0;
		} else
			SAMPLES_X_TO_INT_FRAC(edge->x.quo, rix, rfx);

		assert(edge->height_left > 0);
		if (--edge->height_left) {
			if (edge->dy) {
				edge->x.quo += edge->dxdy.quo;
				edge->x.rem += edge->dxdy.rem;
				if (edge->x.rem >= 0) {
					++edge->x.quo;
					edge->x.rem -= edge->dy;
				}
			}

			if (edge->x.quo < prev_x) {
				struct edge *pos = edge->prev;
				pos->next = next;
				next->prev = pos;
				do {
					pos = pos->prev;
				} while (edge->x.quo < pos->x.quo);
				pos->next->prev = edge;
				edge->next = pos->next;
				edge->prev = pos;
				pos->next = edge;
			} else
				prev_x = edge->x.quo;
		} else {
			edge->prev->next = next;
			next->prev = edge->prev;
		}

		edge = next;

		if (lix == rix) {
			if (rfx != lfx) {
				assert(lix < width);
				row[lix] += (rfx-lfx);
			}
		} else {
			assert(lix < width);
			row[lix] += SAMPLES_X - lfx;

			assert(rix <= width);
			if (rfx) {
				assert(rix < width);
				row[rix] += rfx;
			}

			while (++lix < rix)
				row[lix] += SAMPLES_X;
		}
	}
}

flatten static void
tor_inplace(struct tor *converter, PixmapPtr scratch)
{
	uint8_t buf[TOR_INPLACE_SIZE];
	int i, j, h = converter->ymax - converter->ymin;
	struct polygon *polygon = converter->polygon;
	struct active_list *active = converter->active;
	struct edge *buckets[SAMPLES_Y] = { 0 };
	uint8_t *row = scratch->devPrivate.ptr;
	int stride = scratch->devKind;
	int width = scratch->drawable.width;

	__DBG(("%s: buf?=%d\n", __FUNCTION__, buf != NULL));
	assert(converter->xmin == 0);
	assert(scratch->drawable.depth == 8);

	row += converter->ymin * stride;

	/* Render each pixel row. */
	for (i = 0; i < h; i = j) {
		int do_full_step = 0;
		void *ptr = scratch->usage_hint ? buf : row;

		j = i + 1;

		/* Determine if we can ignore this row or use the full pixel
		 * stepper. */
		if (!polygon->y_buckets[i]) {
			if (active->head.next == &active->tail) {
				for (; !polygon->y_buckets[j]; j++)
					;
				__DBG(("%s: no new edges and no exisiting edges, skipping, %d -> %d\n",
				       __FUNCTION__, i, j));

				memset(row, 0, stride*(j-i));
				row += stride*(j-i);
				continue;
			}

			do_full_step = can_full_step(active);
		}

		__DBG(("%s: y=%d, do_full_step=%d, new edges=%d\n",
		       __FUNCTION__, i, do_full_step,
		       polygon->y_buckets[i] != NULL));
		if (do_full_step) {
			memset(ptr, 0, width);
			inplace_row(active, ptr, width);
			if (row != ptr)
				memcpy(row, ptr, width);

			while (polygon->y_buckets[j] == NULL &&
			       do_full_step >= 2*SAMPLES_Y) {
				do_full_step -= SAMPLES_Y;
				row += stride;
				memcpy(row, ptr, width);
				j++;
			}
			if (j != i + 1)
				step_edges(active, j - (i + 1));

			__DBG(("%s: vertical edges, full step (%d, %d)\n",
			       __FUNCTION__,  i, j));
		} else {
			int suby;

			fill_buckets(active, polygon->y_buckets[i], (i+converter->ymin)*SAMPLES_Y, buckets);

			/* Subsample this row. */
			memset(ptr, 0, width);
			for (suby = 0; suby < SAMPLES_Y; suby++) {
				if (buckets[suby]) {
					merge_edges(active, buckets[suby]);
					buckets[suby] = NULL;
				}

				inplace_subrow(active, ptr, width);
			}
			if (row != ptr)
				memcpy(row, ptr, width);
		}

		row += stride;
	}
}

static int operator_is_bounded(uint8_t op)
{
	switch (op) {
	case PictOpOver:
	case PictOpOutReverse:
	case PictOpAdd:
		return true;
	default:
		return false;
	}
}

static inline bool
project_trapezoid_onto_grid(const xTrapezoid *in,
			    int dx, int dy,
			    xTrapezoid *out)
{
	__DBG(("%s: in: L:(%d, %d), (%d, %d); R:(%d, %d), (%d, %d), [%d, %d]\n",
	       __FUNCTION__,
	       in->left.p1.x, in->left.p1.y, in->left.p2.x, in->left.p2.y,
	       in->right.p1.x, in->right.p1.y, in->right.p2.x, in->right.p2.y,
	       in->top, in->bottom));

	out->left.p1.x = dx + pixman_fixed_to_grid_x(in->left.p1.x);
	out->left.p1.y = dy + pixman_fixed_to_grid_y(in->left.p1.y);
	out->left.p2.x = dx + pixman_fixed_to_grid_x(in->left.p2.x);
	out->left.p2.y = dy + pixman_fixed_to_grid_y(in->left.p2.y);

	out->right.p1.x = dx + pixman_fixed_to_grid_x(in->right.p1.x);
	out->right.p1.y = dy + pixman_fixed_to_grid_y(in->right.p1.y);
	out->right.p2.x = dx + pixman_fixed_to_grid_x(in->right.p2.x);
	out->right.p2.y = dy + pixman_fixed_to_grid_y(in->right.p2.y);

	out->top = dy + pixman_fixed_to_grid_y(in->top);
	out->bottom = dy + pixman_fixed_to_grid_y(in->bottom);

	__DBG(("%s: out: L:(%d, %d), (%d, %d); R:(%d, %d), (%d, %d), [%d, %d]\n",
	       __FUNCTION__,
	       out->left.p1.x, out->left.p1.y, out->left.p2.x, out->left.p2.y,
	       out->right.p1.x, out->right.p1.y, out->right.p2.x, out->right.p2.y,
	       out->top, out->bottom));

	return xTrapezoidValid(out);
}

static span_func_t
choose_span(struct sna_composite_spans_op *tmp,
	    PicturePtr dst,
	    PictFormatPtr maskFormat,
	    RegionPtr clip)
{
	span_func_t span;

	assert(!is_mono(dst, maskFormat));
	if (clip->data)
		span = tor_blt_span_clipped;
	else if (tmp->base.damage == NULL)
		span = tor_blt_span__no_damage;
	else
		span = tor_blt_span;

	return span;
}

struct span_thread {
	struct sna *sna;
	const struct sna_composite_spans_op *op;
	const xTrapezoid *traps;
	RegionPtr clip;
	span_func_t span;
	BoxRec extents;
	int dx, dy, draw_y;
	int ntrap;
	bool unbounded;
};

#define SPAN_THREAD_MAX_BOXES (8192/sizeof(struct sna_opacity_box))
struct span_thread_boxes {
	const struct sna_composite_spans_op *op;
	int num_boxes;
	struct sna_opacity_box boxes[SPAN_THREAD_MAX_BOXES];
};

static void span_thread_add_boxes(struct sna *sna, void *data,
				  const BoxRec *box, int count, float alpha)
{
	struct span_thread_boxes *b = data;

	__DBG(("%s: adding %d boxes with alpha=%f\n",
	       __FUNCTION__, count, alpha));

	assert(count > 0 && count <= SPAN_THREAD_MAX_BOXES);
	if (unlikely(b->num_boxes + count > SPAN_THREAD_MAX_BOXES)) {
		DBG(("%s: flushing %d boxes, adding %d\n", __FUNCTION__, b->num_boxes, count));
		assert(b->num_boxes <= SPAN_THREAD_MAX_BOXES);
		b->op->thread_boxes(sna, b->op, b->boxes, b->num_boxes);
		b->num_boxes = 0;
	}

	do {
		b->boxes[b->num_boxes].box = *box++;
		b->boxes[b->num_boxes].alpha = alpha;
		b->num_boxes++;
	} while (--count);
	assert(b->num_boxes <= SPAN_THREAD_MAX_BOXES);
}

static void
span_thread_box(struct sna *sna,
		struct sna_composite_spans_op *op,
		pixman_region16_t *clip,
		const BoxRec *box,
		int coverage)
{
	__DBG(("%s: %d -> %d @ %d\n", __FUNCTION__, box->x1, box->x2, coverage));
	span_thread_add_boxes(sna, op, box, 1, AREA_TO_FLOAT(coverage));
}

static void
span_thread_clipped_box(struct sna *sna,
			struct sna_composite_spans_op *op,
			pixman_region16_t *clip,
			const BoxRec *box,
			int coverage)
{
	pixman_region16_t region;

	__DBG(("%s: %d -> %d @ %f\n", __FUNCTION__, box->x1, box->x2,
	       AREA_TO_FLOAT(coverage)));

	pixman_region_init_rects(&region, box, 1);
	RegionIntersect(&region, &region, clip);
	if (REGION_NUM_RECTS(&region)) {
		span_thread_add_boxes(sna, op,
				      REGION_RECTS(&region),
				      REGION_NUM_RECTS(&region),
				      AREA_TO_FLOAT(coverage));
	}
	pixman_region_fini(&region);
}

static span_func_t
thread_choose_span(struct sna_composite_spans_op *tmp,
		   PicturePtr dst,
		   PictFormatPtr maskFormat,
		   RegionPtr clip)
{
	span_func_t span;

	if (tmp->base.damage) {
		DBG(("%s: damaged -> no thread support\n", __FUNCTION__));
		return NULL;
	}

	assert(!is_mono(dst, maskFormat));
	assert(tmp->thread_boxes);
	DBG(("%s: clipped? %d\n", __FUNCTION__, clip->data != NULL));
	if (clip->data)
		span = span_thread_clipped_box;
	else
		span = span_thread_box;

	return span;
}

static void
span_thread(void *arg)
{
	struct span_thread *thread = arg;
	struct span_thread_boxes boxes;
	struct tor tor;
	const xTrapezoid *t;
	int n, y1, y2;

	if (!tor_init(&tor, &thread->extents, 2*thread->ntrap))
		return;

	boxes.op = thread->op;
	boxes.num_boxes = 0;

	y1 = thread->extents.y1 - thread->draw_y;
	y2 = thread->extents.y2 - thread->draw_y;
	for (n = thread->ntrap, t = thread->traps; n--; t++) {
		xTrapezoid tt;

		if (pixman_fixed_integer_floor(t->top) >= y2 ||
		    pixman_fixed_integer_ceil(t->bottom) <= y1)
			continue;

		if (!project_trapezoid_onto_grid(t, thread->dx, thread->dy, &tt))
			continue;

		tor_add_edge(&tor, &tt, &tt.left, 1);
		tor_add_edge(&tor, &tt, &tt.right, -1);
	}

	tor_render(thread->sna, &tor,
		   (struct sna_composite_spans_op *)&boxes, thread->clip,
		   thread->span, thread->unbounded);

	tor_fini(&tor);

	if (boxes.num_boxes) {
		DBG(("%s: flushing %d boxes\n", __FUNCTION__, boxes.num_boxes));
		assert(boxes.num_boxes <= SPAN_THREAD_MAX_BOXES);
		thread->op->thread_boxes(thread->sna, thread->op,
					 boxes.boxes, boxes.num_boxes);
	}
}

bool
precise_trapezoid_span_converter(struct sna *sna,
				 CARD8 op, PicturePtr src, PicturePtr dst,
				 PictFormatPtr maskFormat, unsigned int flags,
				 INT16 src_x, INT16 src_y,
				 int ntrap, xTrapezoid *traps)
{
	struct sna_composite_spans_op tmp;
	pixman_region16_t clip;
	int16_t dst_x, dst_y;
	bool was_clear;
	int dx, dy, n;
	int num_threads;

	if (NO_PRECISE)
		return false;

	if (!sna->render.check_composite_spans(sna, op, src, dst, 0, 0, flags)) {
		DBG(("%s: fallback -- composite spans not supported\n",
		     __FUNCTION__));
		return false;
	}

	if (!trapezoids_bounds(ntrap, traps, &clip.extents))
		return true;

#if 1
	if (((clip.extents.y2 - clip.extents.y1) | (clip.extents.x2 - clip.extents.x1)) < 32) {
		DBG(("%s: fallback -- traps extents too small %dx%d\n", __FUNCTION__,
		     clip.extents.y2 - clip.extents.y1,
		     clip.extents.x2 - clip.extents.x1));
		return false;
	}
#endif

	DBG(("%s: extents (%d, %d), (%d, %d)\n",
	     __FUNCTION__,
	     clip.extents.x1, clip.extents.y1,
	     clip.extents.x2, clip.extents.y2));

	trapezoid_origin(&traps[0].left, &dst_x, &dst_y);

	if (!sna_compute_composite_region(&clip,
					  src, NULL, dst,
					  src_x + clip.extents.x1 - dst_x,
					  src_y + clip.extents.y1 - dst_y,
					  0, 0,
					  clip.extents.x1, clip.extents.y1,
					  clip.extents.x2 - clip.extents.x1,
					  clip.extents.y2 - clip.extents.y1)) {
		DBG(("%s: trapezoids do not intersect drawable clips\n",
		     __FUNCTION__)) ;
		return true;
	}

	if (!sna->render.check_composite_spans(sna, op, src, dst,
					       clip.extents.x2 - clip.extents.x1,
					       clip.extents.y2 - clip.extents.y1,
					       flags)) {
		DBG(("%s: fallback -- composite spans not supported\n",
		     __FUNCTION__));
		return false;
	}

	dx = dst->pDrawable->x;
	dy = dst->pDrawable->y;

	DBG(("%s: after clip -- extents (%d, %d), (%d, %d), delta=(%d, %d) src -> (%d, %d)\n",
	     __FUNCTION__,
	     clip.extents.x1, clip.extents.y1,
	     clip.extents.x2, clip.extents.y2,
	     dx, dy,
	     src_x + clip.extents.x1 - dst_x - dx,
	     src_y + clip.extents.y1 - dst_y - dy));

	was_clear = sna_drawable_is_clear(dst->pDrawable);
	switch (op) {
	case PictOpAdd:
	case PictOpOver:
		if (was_clear)
			op = PictOpSrc;
		break;
	case PictOpIn:
		if (was_clear)
			return true;
		break;
	}

	if (!sna->render.composite_spans(sna, op, src, dst,
					 src_x + clip.extents.x1 - dst_x - dx,
					 src_y + clip.extents.y1 - dst_y - dy,
					 clip.extents.x1,  clip.extents.y1,
					 clip.extents.x2 - clip.extents.x1,
					 clip.extents.y2 - clip.extents.y1,
					 flags, memset(&tmp, 0, sizeof(tmp)))) {
		DBG(("%s: fallback -- composite spans render op not supported\n",
		     __FUNCTION__));
		return false;
	}

	dx *= SAMPLES_X;
	dy *= SAMPLES_Y;

	num_threads = 1;
	if (!NO_GPU_THREADS &&
	    (flags & COMPOSITE_SPANS_RECTILINEAR) == 0 &&
	    tmp.thread_boxes &&
	    thread_choose_span(&tmp, dst, maskFormat, &clip))
		num_threads = sna_use_threads(clip.extents.x2-clip.extents.x1,
					      clip.extents.y2-clip.extents.y1,
					      8);
	DBG(("%s: using %d threads\n", __FUNCTION__, num_threads));
	if (num_threads == 1) {
		struct tor tor;

		if (!tor_init(&tor, &clip.extents, 2*ntrap))
			goto skip;

		for (n = 0; n < ntrap; n++) {
			xTrapezoid t;

			if (pixman_fixed_integer_floor(traps[n].top) + dst->pDrawable->y >= clip.extents.y2 ||
			    pixman_fixed_integer_ceil(traps[n].bottom) + dst->pDrawable->y <= clip.extents.y1)
				continue;

			if (!project_trapezoid_onto_grid(&traps[n], dx, dy, &t))
				continue;

			tor_add_edge(&tor, &t, &t.left, 1);
			tor_add_edge(&tor, &t, &t.right, -1);
		}

		tor_render(sna, &tor, &tmp, &clip,
			   choose_span(&tmp, dst, maskFormat, &clip),
			   !was_clear && maskFormat && !operator_is_bounded(op));

		tor_fini(&tor);
	} else {
		struct span_thread threads[num_threads];
		int y, h;

		DBG(("%s: using %d threads for span compositing %dx%d\n",
		     __FUNCTION__, num_threads,
		     clip.extents.x2 - clip.extents.x1,
		     clip.extents.y2 - clip.extents.y1));

		threads[0].sna = sna;
		threads[0].op = &tmp;
		threads[0].traps = traps;
		threads[0].ntrap = ntrap;
		threads[0].extents = clip.extents;
		threads[0].clip = &clip;
		threads[0].dx = dx;
		threads[0].dy = dy;
		threads[0].draw_y = dst->pDrawable->y;
		threads[0].unbounded = !was_clear && maskFormat && !operator_is_bounded(op);
		threads[0].span = thread_choose_span(&tmp, dst, maskFormat, &clip);

		y = clip.extents.y1;
		h = clip.extents.y2 - clip.extents.y1;
		h = (h + num_threads - 1) / num_threads;

		for (n = 1; n < num_threads; n++) {
			threads[n] = threads[0];
			threads[n].extents.y1 = y;
			threads[n].extents.y2 = y += h;

			sna_threads_run(span_thread, &threads[n]);
		}

		threads[0].extents.y1 = y;
		span_thread(&threads[0]);

		sna_threads_wait();
	}
skip:
	tmp.done(sna, &tmp);

	REGION_UNINIT(NULL, &clip);
	return true;
}

static void
tor_blt_mask(struct sna *sna,
	     struct sna_composite_spans_op *op,
	     pixman_region16_t *clip,
	     const BoxRec *box,
	     int coverage)
{
	uint8_t *ptr = (uint8_t *)op;
	int stride = (intptr_t)clip;
	int h, w;

	coverage = TO_ALPHA(coverage);
	ptr += box->y1 * stride + box->x1;

	h = box->y2 - box->y1;
	w = box->x2 - box->x1;
	if ((w | h) == 1) {
		*ptr = coverage;
	} else if (w == 1) {
		do {
			*ptr = coverage;
			ptr += stride;
		} while (--h);
	} else do {
		memset(ptr, coverage, w);
		ptr += stride;
	} while (--h);
}

struct mask_thread {
	PixmapPtr scratch;
	const xTrapezoid *traps;
	BoxRec extents;
	int dx, dy, dst_y;
	int ntrap;
};

static void
mask_thread(void *arg)
{
	struct mask_thread *thread = arg;
	struct tor tor;
	const xTrapezoid *t;
	int n, y1, y2;

	if (!tor_init(&tor, &thread->extents, 2*thread->ntrap))
		return;

	y1 = thread->extents.y1 + thread->dst_y;
	y2 = thread->extents.y2 + thread->dst_y;
	for (n = thread->ntrap, t = thread->traps; n--; t++) {
		xTrapezoid tt;

		if (pixman_fixed_integer_floor(t->top) >= y2 ||
		    pixman_fixed_integer_ceil(t->bottom) <= y1)
			continue;

		if (!project_trapezoid_onto_grid(t, thread->dx, thread->dy, &tt))
			continue;

		tor_add_edge(&tor, &tt, &tt.left, 1);
		tor_add_edge(&tor, &tt, &tt.right, -1);
	}

	if (thread->extents.x2 <= TOR_INPLACE_SIZE) {
		tor_inplace(&tor, thread->scratch);
	} else {
		tor_render(NULL, &tor,
			   thread->scratch->devPrivate.ptr,
			   (void *)(intptr_t)thread->scratch->devKind,
			   tor_blt_mask,
			   true);
	}

	tor_fini(&tor);
}

bool
precise_trapezoid_mask_converter(CARD8 op, PicturePtr src, PicturePtr dst,
				 PictFormatPtr maskFormat, unsigned flags,
				 INT16 src_x, INT16 src_y,
				 int ntrap, xTrapezoid *traps)
{
	ScreenPtr screen = dst->pDrawable->pScreen;
	PixmapPtr scratch;
	PicturePtr mask;
	BoxRec extents;
	int num_threads;
	int16_t dst_x, dst_y;
	int dx, dy;
	int error, n;

	if (NO_PRECISE)
		return false;

	if (maskFormat == NULL && ntrap > 1) {
		DBG(("%s: individual rasterisation requested\n",
		     __FUNCTION__));
		do {
			/* XXX unwind errors? */
			if (!precise_trapezoid_mask_converter(op, src, dst, NULL, flags,
							      src_x, src_y, 1, traps++))
				return false;
		} while (--ntrap);
		return true;
	}

	if (!trapezoids_bounds(ntrap, traps, &extents))
		return true;

	DBG(("%s: ntraps=%d, extents (%d, %d), (%d, %d)\n",
	     __FUNCTION__, ntrap, extents.x1, extents.y1, extents.x2, extents.y2));

	if (!sna_compute_composite_extents(&extents,
					   src, NULL, dst,
					   src_x, src_y,
					   0, 0,
					   extents.x1, extents.y1,
					   extents.x2 - extents.x1,
					   extents.y2 - extents.y1))
		return true;

	DBG(("%s: extents (%d, %d), (%d, %d)\n",
	     __FUNCTION__, extents.x1, extents.y1, extents.x2, extents.y2));

	extents.y2 -= extents.y1;
	extents.x2 -= extents.x1;
	extents.x1 -= dst->pDrawable->x;
	extents.y1 -= dst->pDrawable->y;
	dst_x = extents.x1;
	dst_y = extents.y1;
	dx = -extents.x1 * SAMPLES_X;
	dy = -extents.y1 * SAMPLES_Y;
	extents.x1 = extents.y1 = 0;

	DBG(("%s: mask (%dx%d), dx=(%d, %d)\n",
	     __FUNCTION__, extents.x2, extents.y2, dx, dy));
	scratch = sna_pixmap_create_upload(screen,
					   extents.x2, extents.y2, 8,
					   KGEM_BUFFER_WRITE_INPLACE);
	if (!scratch)
		return true;

	DBG(("%s: created buffer %p, stride %d\n",
	     __FUNCTION__, scratch->devPrivate.ptr, scratch->devKind));

	num_threads = 1;
	if ((flags & COMPOSITE_SPANS_RECTILINEAR) == 0)
		num_threads = sna_use_threads(extents.x2 - extents.x1,
					      extents.y2 - extents.y1,
					      4);
	if (num_threads == 1) {
		struct tor tor;

		if (!tor_init(&tor, &extents, 2*ntrap)) {
			sna_pixmap_destroy(scratch);
			return true;
		}

		for (n = 0; n < ntrap; n++) {
			xTrapezoid t;

			if (pixman_fixed_to_int(traps[n].top) - dst_y >= extents.y2 ||
			    pixman_fixed_to_int(traps[n].bottom) - dst_y < 0)
				continue;

			if (!project_trapezoid_onto_grid(&traps[n], dx, dy, &t))
				continue;

			tor_add_edge(&tor, &t, &t.left, 1);
			tor_add_edge(&tor, &t, &t.right, -1);
		}

		if (extents.x2 <= TOR_INPLACE_SIZE) {
			tor_inplace(&tor, scratch);
		} else {
			tor_render(NULL, &tor,
				   scratch->devPrivate.ptr,
				   (void *)(intptr_t)scratch->devKind,
				   tor_blt_mask,
				   true);
		}
		tor_fini(&tor);
	} else {
		struct mask_thread threads[num_threads];
		int y, h;

		DBG(("%s: using %d threads for mask compositing %dx%d\n",
		     __FUNCTION__, num_threads,
		     extents.x2 - extents.x1,
		     extents.y2 - extents.y1));

		threads[0].scratch = scratch;
		threads[0].traps = traps;
		threads[0].ntrap = ntrap;
		threads[0].extents = extents;
		threads[0].dx = dx;
		threads[0].dy = dy;
		threads[0].dst_y = dst_y;

		y = extents.y1;
		h = extents.y2 - extents.y1;
		h = (h + num_threads - 1) / num_threads;

		for (n = 1; n < num_threads; n++) {
			threads[n] = threads[0];
			threads[n].extents.y1 = y;
			threads[n].extents.y2 = y += h;

			sna_threads_run(mask_thread, &threads[n]);
		}

		threads[0].extents.y1 = y;
		mask_thread(&threads[0]);

		sna_threads_wait();
	}

	mask = CreatePicture(0, &scratch->drawable,
			     PictureMatchFormat(screen, 8, PICT_a8),
			     0, 0, serverClient, &error);
	if (mask) {
		int16_t x0, y0;

		trapezoid_origin(&traps[0].left, &x0, &y0);

		CompositePicture(op, src, mask, dst,
				 src_x + dst_x - x0,
				 src_y + dst_y - y0,
				 0, 0,
				 dst_x, dst_y,
				 extents.x2, extents.y2);
		FreePicture(mask, 0);
	}
	sna_pixmap_destroy(scratch);

	return true;
}

struct inplace {
	uint8_t *ptr;
	uint32_t stride;
	union {
		uint8_t opacity;
		uint32_t color;
	};
};

static force_inline uint8_t coverage_opacity(int coverage, uint8_t opacity)
{
	coverage = TO_ALPHA(coverage);
	return opacity == 255 ? coverage : mul_8_8(coverage, opacity);
}

static void _tor_blt_src(struct inplace *in, const BoxRec *box, uint8_t v)
{
	uint8_t *ptr = in->ptr;
	int h, w;

	ptr += box->y1 * in->stride + box->x1;

	h = box->y2 - box->y1;
	w = box->x2 - box->x1;
	if ((w | h) == 1) {
		*ptr = v;
	} else if (w == 1) {
		do {
			*ptr = v;
			ptr += in->stride;
		} while (--h);
	} else do {
		memset(ptr, v, w);
		ptr += in->stride;
	} while (--h);
}

static void
tor_blt_src(struct sna *sna,
	    struct sna_composite_spans_op *op,
	    pixman_region16_t *clip,
	    const BoxRec *box,
	    int coverage)
{
	struct inplace *in = (struct inplace *)op;

	_tor_blt_src(in, box, coverage_opacity(coverage, in->opacity));
}

static void
tor_blt_src_clipped(struct sna *sna,
		    struct sna_composite_spans_op *op,
		    pixman_region16_t *clip,
		    const BoxRec *box,
		    int coverage)
{
	pixman_region16_t region;
	int n;

	pixman_region_init_rects(&region, box, 1);
	RegionIntersect(&region, &region, clip);
	n = REGION_NUM_RECTS(&region);
	box = REGION_RECTS(&region);
	while (n--)
		tor_blt_src(sna, op, NULL, box++, coverage);
	pixman_region_fini(&region);
}

static void
tor_blt_in(struct sna *sna,
	   struct sna_composite_spans_op *op,
	   pixman_region16_t *clip,
	   const BoxRec *box,
	   int coverage)
{
	struct inplace *in = (struct inplace *)op;
	uint8_t *ptr = in->ptr;
	int h, w, i;

	if (coverage == 0 || in->opacity == 0) {
		_tor_blt_src(in, box, 0);
		return;
	}

	coverage = coverage_opacity(coverage, in->opacity);
	if (coverage == 0xff)
		return;

	ptr += box->y1 * in->stride + box->x1;

	h = box->y2 - box->y1;
	w = box->x2 - box->x1;
	do {
		for (i = 0; i < w; i++)
			ptr[i] = mul_8_8(ptr[i], coverage);
		ptr += in->stride;
	} while (--h);
}

static void
tor_blt_in_clipped(struct sna *sna,
		   struct sna_composite_spans_op *op,
		   pixman_region16_t *clip,
		   const BoxRec *box,
		   int coverage)
{
	pixman_region16_t region;
	int n;

	pixman_region_init_rects(&region, box, 1);
	RegionIntersect(&region, &region, clip);
	n = REGION_NUM_RECTS(&region);
	box = REGION_RECTS(&region);
	while (n--)
		tor_blt_in(sna, op, NULL, box++, coverage);
	pixman_region_fini(&region);
}

static void
tor_blt_add(struct sna *sna,
	    struct sna_composite_spans_op *op,
	    pixman_region16_t *clip,
	    const BoxRec *box,
	    int coverage)
{
	struct inplace *in = (struct inplace *)op;
	uint8_t *ptr = in->ptr;
	int h, w, v, i;

	if (coverage == 0)
		return;

	coverage = coverage_opacity(coverage, in->opacity);
	if (coverage == 0xff) {
		_tor_blt_src(in, box, 0xff);
		return;
	}

	ptr += box->y1 * in->stride + box->x1;

	h = box->y2 - box->y1;
	w = box->x2 - box->x1;
	if ((w | h) == 1) {
		v = coverage + *ptr;
		*ptr = v >= 255 ? 255 : v;
	} else {
		do {
			for (i = 0; i < w; i++) {
				v = coverage + ptr[i];
				ptr[i] = v >= 255 ? 255 : v;
			}
			ptr += in->stride;
		} while (--h);
	}
}

static void
tor_blt_add_clipped(struct sna *sna,
		    struct sna_composite_spans_op *op,
		    pixman_region16_t *clip,
		    const BoxRec *box,
		    int coverage)
{
	pixman_region16_t region;
	int n;

	pixman_region_init_rects(&region, box, 1);
	RegionIntersect(&region, &region, clip);
	n = REGION_NUM_RECTS(&region);
	box = REGION_RECTS(&region);
	while (n--)
		tor_blt_add(sna, op, NULL, box++, coverage);
	pixman_region_fini(&region);
}

static void
tor_blt_lerp32(struct sna *sna,
	       struct sna_composite_spans_op *op,
	       pixman_region16_t *clip,
	       const BoxRec *box,
	       int coverage)
{
	struct inplace *in = (struct inplace *)op;
	uint32_t *ptr = (uint32_t *)in->ptr;
	int stride = in->stride / sizeof(uint32_t);
	int h, w, i;

	if (coverage == 0)
		return;

	ptr += box->y1 * stride + box->x1;

	h = box->y2 - box->y1;
	w = box->x2 - box->x1;
	if (coverage == GRID_AREA) {
		if ((w | h) == 1) {
			*ptr = in->color;
		} else {
			if (w < 16) {
				do {
					for (i = 0; i < w; i++)
						ptr[i] = in->color;
					ptr += stride;
				} while (--h);
			} else {
				pixman_fill(ptr, stride, 32,
					    0, 0, w, h, in->color);
			}
		}
	} else {
		coverage = TO_ALPHA(coverage);
		if ((w | h) == 1) {
			*ptr = lerp8x4(in->color, coverage, *ptr);
		} else if (w == 1) {
			do {
				*ptr = lerp8x4(in->color, coverage, *ptr);
				ptr += stride;
			} while (--h);
		} else{
			do {
				for (i = 0; i < w; i++)
					ptr[i] = lerp8x4(in->color, coverage, ptr[i]);
				ptr += stride;
			} while (--h);
		}
	}
}

static void
tor_blt_lerp32_clipped(struct sna *sna,
		       struct sna_composite_spans_op *op,
		       pixman_region16_t *clip,
		       const BoxRec *box,
		       int coverage)
{
	pixman_region16_t region;
	int n;

	pixman_region_init_rects(&region, box, 1);
	RegionIntersect(&region, &region, clip);
	n = REGION_NUM_RECTS(&region);
	box = REGION_RECTS(&region);
	while (n--)
		tor_blt_lerp32(sna, op, NULL, box++, coverage);
	pixman_region_fini(&region);
}

struct pixman_inplace {
	pixman_image_t *image, *source, *mask;
	uint32_t color;
	uint32_t *bits;
	int dx, dy;
	int sx, sy;
	uint8_t op;
};

static void
pixmask_span_solid(struct sna *sna,
		   struct sna_composite_spans_op *op,
		   pixman_region16_t *clip,
		   const BoxRec *box,
		   int coverage)
{
	struct pixman_inplace *pi = (struct pixman_inplace *)op;
	if (coverage != GRID_AREA)
		*pi->bits = mul_4x8_8(pi->color, TO_ALPHA(coverage));
	else
		*pi->bits = pi->color;
	pixman_image_composite(pi->op, pi->source, NULL, pi->image,
			       box->x1, box->y1,
			       0, 0,
			       pi->dx + box->x1, pi->dy + box->y1,
			       box->x2 - box->x1, box->y2 - box->y1);
}
static void
pixmask_span_solid__clipped(struct sna *sna,
			    struct sna_composite_spans_op *op,
			    pixman_region16_t *clip,
			    const BoxRec *box,
			    int coverage)
{
	pixman_region16_t region;
	int n;

	pixman_region_init_rects(&region, box, 1);
	RegionIntersect(&region, &region, clip);
	n = REGION_NUM_RECTS(&region);
	box = REGION_RECTS(&region);
	while (n--)
		pixmask_span_solid(sna, op, NULL, box++, coverage);
	pixman_region_fini(&region);
}

static void
pixmask_span(struct sna *sna,
	     struct sna_composite_spans_op *op,
	     pixman_region16_t *clip,
	     const BoxRec *box,
	     int coverage)
{
	struct pixman_inplace *pi = (struct pixman_inplace *)op;
	pixman_image_t *mask = NULL;
	if (coverage != GRID_AREA) {
		*pi->bits = TO_ALPHA(coverage);
		mask = pi->mask;
	}
	pixman_image_composite(pi->op, pi->source, mask, pi->image,
			       pi->sx + box->x1, pi->sy + box->y1,
			       0, 0,
			       pi->dx + box->x1, pi->dy + box->y1,
			       box->x2 - box->x1, box->y2 - box->y1);
}
static void
pixmask_span__clipped(struct sna *sna,
		      struct sna_composite_spans_op *op,
		      pixman_region16_t *clip,
		      const BoxRec *box,
		      int coverage)
{
	pixman_region16_t region;
	int n;

	pixman_region_init_rects(&region, box, 1);
	RegionIntersect(&region, &region, clip);
	n = REGION_NUM_RECTS(&region);
	box = REGION_RECTS(&region);
	while (n--)
		pixmask_span(sna, op, NULL, box++, coverage);
	pixman_region_fini(&region);
}

struct inplace_x8r8g8b8_thread {
	xTrapezoid *traps;
	PicturePtr dst, src;
	BoxRec extents;
	int dx, dy;
	int ntrap;
	bool lerp, is_solid;
	uint32_t color;
	int16_t src_x, src_y;
	uint8_t op;
};

static void inplace_x8r8g8b8_thread(void *arg)
{
	struct inplace_x8r8g8b8_thread *thread = arg;
	struct tor tor;
	span_func_t span;
	RegionPtr clip;
	int y1, y2, n;

	if (!tor_init(&tor, &thread->extents, 2*thread->ntrap))
		return;

	y1 = thread->extents.y1 - thread->dst->pDrawable->y;
	y2 = thread->extents.y2 - thread->dst->pDrawable->y;
	for (n = 0; n < thread->ntrap; n++) {
		xTrapezoid t;

		if (pixman_fixed_to_int(thread->traps[n].top) >= y2 ||
		    pixman_fixed_to_int(thread->traps[n].bottom) < y1)
			continue;

		if (!project_trapezoid_onto_grid(&thread->traps[n], thread->dx, thread->dy, &t))
			continue;

		tor_add_edge(&tor, &t, &t.left, 1);
		tor_add_edge(&tor, &t, &t.right, -1);
	}

	clip = thread->dst->pCompositeClip;
	if (thread->lerp) {
		struct inplace inplace;
		int16_t dst_x, dst_y;
		PixmapPtr pixmap;

		pixmap = get_drawable_pixmap(thread->dst->pDrawable);

		inplace.ptr = pixmap->devPrivate.ptr;
		if (get_drawable_deltas(thread->dst->pDrawable, pixmap, &dst_x, &dst_y))
			inplace.ptr += dst_y * pixmap->devKind + dst_x * 4;
		inplace.stride = pixmap->devKind;
		inplace.color = thread->color;

		if (clip->data)
			span = tor_blt_lerp32_clipped;
		else
			span = tor_blt_lerp32;

		tor_render(NULL, &tor, (void*)&inplace, clip, span, false);
	} else if (thread->is_solid) {
		struct pixman_inplace pi;

		pi.image = image_from_pict(thread->dst, false, &pi.dx, &pi.dy);
		pi.op = thread->op;
		pi.color = thread->color;

		pi.bits = (uint32_t *)&pi.sx;
		pi.source = pixman_image_create_bits(PIXMAN_a8r8g8b8,
						     1, 1, pi.bits, 0);
		pixman_image_set_repeat(pi.source, PIXMAN_REPEAT_NORMAL);

		if (clip->data)
			span = pixmask_span_solid__clipped;
		else
			span = pixmask_span_solid;

		tor_render(NULL, &tor, (void*)&pi, clip, span, false);

		pixman_image_unref(pi.source);
		pixman_image_unref(pi.image);
	} else {
		struct pixman_inplace pi;
		int16_t x0, y0;

		trapezoid_origin(&thread->traps[0].left, &x0, &y0);

		pi.image = image_from_pict(thread->dst, false, &pi.dx, &pi.dy);
		pi.source = image_from_pict(thread->src, false, &pi.sx, &pi.sy);
		pi.sx += thread->src_x - x0;
		pi.sy += thread->src_y - y0;
		pi.mask = pixman_image_create_bits(PIXMAN_a8, 1, 1, NULL, 0);
		pixman_image_set_repeat(pi.mask, PIXMAN_REPEAT_NORMAL);
		pi.bits = pixman_image_get_data(pi.mask);
		pi.op = thread->op;

		if (clip->data)
			span = pixmask_span__clipped;
		else
			span = pixmask_span;

		tor_render(NULL, &tor, (void*)&pi, clip, span, false);

		pixman_image_unref(pi.mask);
		pixman_image_unref(pi.source);
		pixman_image_unref(pi.image);
	}

	tor_fini(&tor);
}

static bool
trapezoid_span_inplace__x8r8g8b8(CARD8 op,
				 PicturePtr dst,
				 PicturePtr src, int16_t src_x, int16_t src_y,
				 PictFormatPtr maskFormat, unsigned flags,
				 int ntrap, xTrapezoid *traps)
{
	uint32_t color;
	bool lerp, is_solid;
	RegionRec region;
	int dx, dy;
	int num_threads, n;

	lerp = false;
	is_solid = sna_picture_is_solid(src, &color);
	if (is_solid) {
		if (op == PictOpOver && (color >> 24) == 0xff)
			op = PictOpSrc;
		if (op == PictOpOver && sna_drawable_is_clear(dst->pDrawable))
			op = PictOpSrc;
		lerp = op == PictOpSrc;
	}
	if (!lerp) {
		switch (op) {
		case PictOpOver:
		case PictOpAdd:
		case PictOpOutReverse:
			break;
		case PictOpSrc:
			if (!sna_drawable_is_clear(dst->pDrawable))
				return false;
			break;
		default:
			return false;
		}
	}

	if (maskFormat == NULL && ntrap > 1) {
		DBG(("%s: individual rasterisation requested\n",
		     __FUNCTION__));
		do {
			/* XXX unwind errors? */
			if (!trapezoid_span_inplace__x8r8g8b8(op, dst,
							      src, src_x, src_y,
							      NULL, flags,
							      1, traps++))
				return false;
		} while (--ntrap);
		return true;
	}

	if (!trapezoids_bounds(ntrap, traps, &region.extents))
		return true;

	DBG(("%s: extents (%d, %d), (%d, %d)\n",
	     __FUNCTION__,
	     region.extents.x1, region.extents.y1,
	     region.extents.x2, region.extents.y2));

	if (!sna_compute_composite_extents(&region.extents,
					   src, NULL, dst,
					   src_x, src_y,
					   0, 0,
					   region.extents.x1, region.extents.y1,
					   region.extents.x2 - region.extents.x1,
					   region.extents.y2 - region.extents.y1))
		return true;

	DBG(("%s: clipped extents (%d, %d), (%d, %d)\n",
	     __FUNCTION__,
	     region.extents.x1, region.extents.y1,
	     region.extents.x2, region.extents.y2));

	region.data = NULL;
	if (!sna_drawable_move_region_to_cpu(dst->pDrawable, &region,
					    MOVE_WRITE | MOVE_READ))
		return true;

	if (!is_solid && src->pDrawable) {
		if (!sna_drawable_move_to_cpu(src->pDrawable,
					      MOVE_READ))
			return true;

		if (src->alphaMap &&
		    !sna_drawable_move_to_cpu(src->alphaMap->pDrawable,
					      MOVE_READ))
			return true;
	}

	dx = dst->pDrawable->x * SAMPLES_X;
	dy = dst->pDrawable->y * SAMPLES_Y;

	num_threads = 1;
	if ((flags & COMPOSITE_SPANS_RECTILINEAR) == 0 && (lerp || is_solid))
		num_threads = sna_use_threads(4*(region.extents.x2 - region.extents.x1),
					      region.extents.y2 - region.extents.y1,
					      4);

	DBG(("%s: %dx%d, format=%x, op=%d, lerp?=%d, num_threads=%d\n",
	     __FUNCTION__,
	     region.extents.x2 - region.extents.x1,
	     region.extents.y2 - region.extents.y1,
	     dst->format, op, lerp, num_threads));

	if (num_threads == 1) {
		struct tor tor;
		span_func_t span;

		if (!tor_init(&tor, &region.extents, 2*ntrap))
			return true;

		for (n = 0; n < ntrap; n++) {
			xTrapezoid t;

			if (pixman_fixed_to_int(traps[n].top) >= region.extents.y2 - dst->pDrawable->y ||
			    pixman_fixed_to_int(traps[n].bottom) < region.extents.y1 - dst->pDrawable->y)
				continue;

			if (!project_trapezoid_onto_grid(&traps[n], dx, dy, &t))
				continue;

			tor_add_edge(&tor, &t, &t.left, 1);
			tor_add_edge(&tor, &t, &t.right, -1);
		}

		if (lerp) {
			struct inplace inplace;
			PixmapPtr pixmap;
			int16_t dst_x, dst_y;

			pixmap = get_drawable_pixmap(dst->pDrawable);

			inplace.ptr = pixmap->devPrivate.ptr;
			if (get_drawable_deltas(dst->pDrawable, pixmap, &dst_x, &dst_y))
				inplace.ptr += dst_y * pixmap->devKind + dst_x * 4;
			inplace.stride = pixmap->devKind;
			inplace.color = color;

			if (dst->pCompositeClip->data)
				span = tor_blt_lerp32_clipped;
			else
				span = tor_blt_lerp32;

			DBG(("%s: render inplace op=%d, color=%08x\n",
			     __FUNCTION__, op, color));

			tor_render(NULL, &tor, (void*)&inplace,
				   dst->pCompositeClip, span, false);
		} else if (is_solid) {
			struct pixman_inplace pi;

			pi.image = image_from_pict(dst, false, &pi.dx, &pi.dy);
			pi.op = op;
			pi.color = color;

			pi.bits = (uint32_t *)&pi.sx;
			pi.source = pixman_image_create_bits(PIXMAN_a8r8g8b8,
							     1, 1, pi.bits, 0);
			pixman_image_set_repeat(pi.source, PIXMAN_REPEAT_NORMAL);

			if (dst->pCompositeClip->data)
				span = pixmask_span_solid__clipped;
			else
				span = pixmask_span_solid;

			tor_render(NULL, &tor, (void*)&pi,
				   dst->pCompositeClip, span,
				   false);

			pixman_image_unref(pi.source);
			pixman_image_unref(pi.image);
		} else {
			struct pixman_inplace pi;
			int16_t x0, y0;

			trapezoid_origin(&traps[0].left, &x0, &y0);

			pi.image = image_from_pict(dst, false, &pi.dx, &pi.dy);
			pi.source = image_from_pict(src, false, &pi.sx, &pi.sy);
			pi.sx += src_x - x0;
			pi.sy += src_y - y0;
			pi.mask = pixman_image_create_bits(PIXMAN_a8, 1, 1, NULL, 0);
			pixman_image_set_repeat(pi.mask, PIXMAN_REPEAT_NORMAL);
			pi.bits = pixman_image_get_data(pi.mask);
			pi.op = op;

			if (dst->pCompositeClip->data)
				span = pixmask_span__clipped;
			else
				span = pixmask_span;

			tor_render(NULL, &tor, (void*)&pi,
				   dst->pCompositeClip, span,
				   false);

			pixman_image_unref(pi.mask);
			pixman_image_unref(pi.source);
			pixman_image_unref(pi.image);
		}

		tor_fini(&tor);
	} else {
		struct inplace_x8r8g8b8_thread threads[num_threads];
		int y, h;

		DBG(("%s: using %d threads for inplace compositing %dx%d\n",
		     __FUNCTION__, num_threads,
		     region.extents.x2 - region.extents.x1,
		     region.extents.y2 - region.extents.y1));

		threads[0].traps = traps;
		threads[0].ntrap = ntrap;
		threads[0].extents = region.extents;
		threads[0].lerp = lerp;
		threads[0].is_solid = is_solid;
		threads[0].color = color;
		threads[0].dx = dx;
		threads[0].dy = dy;
		threads[0].dst = dst;
		threads[0].src = src;
		threads[0].op = op;
		threads[0].src_x = src_x;
		threads[0].src_y = src_y;

		y = region.extents.y1;
		h = region.extents.y2 - region.extents.y1;
		h = (h + num_threads - 1) / num_threads;

		for (n = 1; n < num_threads; n++) {
			threads[n] = threads[0];
			threads[n].extents.y1 = y;
			threads[n].extents.y2 = y += h;

			sna_threads_run(inplace_x8r8g8b8_thread, &threads[n]);
		}

		threads[0].extents.y1 = y;
		threads[0].extents.y2 = region.extents.y2;
		inplace_x8r8g8b8_thread(&threads[0]);

		sna_threads_wait();
	}

	return true;
}

struct inplace_thread {
	xTrapezoid *traps;
	RegionPtr clip;
	span_func_t span;
	struct inplace inplace;
	BoxRec extents;
	int dx, dy;
	int draw_x, draw_y;
	bool unbounded;
	int ntrap;
};

static void inplace_thread(void *arg)
{
	struct inplace_thread *thread = arg;
	struct tor tor;
	int n;

	if (!tor_init(&tor, &thread->extents, 2*thread->ntrap))
		return;

	for (n = 0; n < thread->ntrap; n++) {
		xTrapezoid t;

		if (pixman_fixed_to_int(thread->traps[n].top) >= thread->extents.y2 - thread->draw_y ||
		    pixman_fixed_to_int(thread->traps[n].bottom) < thread->extents.y1 - thread->draw_y)
			continue;

		if (!project_trapezoid_onto_grid(&thread->traps[n], thread->dx, thread->dy, &t))
			continue;

		tor_add_edge(&tor, &t, &t.left, 1);
		tor_add_edge(&tor, &t, &t.right, -1);
	}

	tor_render(NULL, &tor, (void*)&thread->inplace,
		   thread->clip, thread->span, thread->unbounded);

	tor_fini(&tor);
}

bool
precise_trapezoid_span_inplace(struct sna *sna,
			       CARD8 op, PicturePtr src, PicturePtr dst,
			       PictFormatPtr maskFormat, unsigned flags,
			       INT16 src_x, INT16 src_y,
			       int ntrap, xTrapezoid *traps,
			       bool fallback)
{
	struct inplace inplace;
	span_func_t span;
	PixmapPtr pixmap;
	struct sna_pixmap *priv;
	RegionRec region;
	uint32_t color;
	bool unbounded;
	int16_t dst_x, dst_y;
	int dx, dy;
	int num_threads, n;

	if (NO_PRECISE)
		return false;

	if (dst->format == PICT_a8r8g8b8 || dst->format == PICT_x8r8g8b8)
		return trapezoid_span_inplace__x8r8g8b8(op, dst,
							src, src_x, src_y,
							maskFormat, flags,
							ntrap, traps);

	if (!sna_picture_is_solid(src, &color)) {
		DBG(("%s: fallback -- can not perform operation in place, requires solid source\n",
		     __FUNCTION__));
		return false;
	}

	if (dst->format != PICT_a8) {
		DBG(("%s: fallback -- can not perform operation in place, format=%x\n",
		     __FUNCTION__, dst->format));
		return false;
	}

	pixmap = get_drawable_pixmap(dst->pDrawable);

	unbounded = false;
	priv = sna_pixmap(pixmap);
	if (priv) {
		switch (op) {
		case PictOpAdd:
			if (priv->clear && priv->clear_color == 0) {
				unbounded = true;
				op = PictOpSrc;
			}
			if ((color >> 24) == 0)
				return true;
			break;
		case PictOpIn:
			if (priv->clear && priv->clear_color == 0)
				return true;
			if (priv->clear && priv->clear_color == 0xff)
				op = PictOpSrc;
			unbounded = true;
			break;
		case PictOpSrc:
			unbounded = true;
			break;
		default:
			DBG(("%s: fallback -- can not perform op [%d] in place\n",
			     __FUNCTION__, op));
			return false;
		}
	} else {
		switch (op) {
		case PictOpAdd:
			if ((color >> 24) == 0)
				return true;
			break;
		case PictOpIn:
		case PictOpSrc:
			unbounded = true;
			break;
		default:
			DBG(("%s: fallback -- can not perform op [%d] in place\n",
			     __FUNCTION__, op));
			return false;
		}
	}

	DBG(("%s: format=%x, op=%d, color=%x\n",
	     __FUNCTION__, dst->format, op, color));

	if (maskFormat == NULL && ntrap > 1) {
		DBG(("%s: individual rasterisation requested\n",
		     __FUNCTION__));
		do {
			/* XXX unwind errors? */
			if (!precise_trapezoid_span_inplace(sna, op, src, dst, NULL, flags,
							    src_x, src_y, 1, traps++,
							    fallback))
				return false;
		} while (--ntrap);
		return true;
	}

	if (!trapezoids_bounds(ntrap, traps, &region.extents))
		return true;

	DBG(("%s: extents (%d, %d), (%d, %d)\n",
	     __FUNCTION__,
	     region.extents.x1, region.extents.y1,
	     region.extents.x2, region.extents.y2));

	if (!sna_compute_composite_extents(&region.extents,
					   NULL, NULL, dst,
					   0, 0,
					   0, 0,
					   region.extents.x1, region.extents.y1,
					   region.extents.x2 - region.extents.x1,
					   region.extents.y2 - region.extents.y1))
		return true;

	DBG(("%s: clipped extents (%d, %d), (%d, %d) [complex clip? %d]\n",
	     __FUNCTION__,
	     region.extents.x1, region.extents.y1,
	     region.extents.x2, region.extents.y2,
	     dst->pCompositeClip->data != NULL));

	if (op == PictOpSrc) {
		if (dst->pCompositeClip->data)
			span = tor_blt_src_clipped;
		else
			span = tor_blt_src;
	} else if (op == PictOpIn) {
		if (dst->pCompositeClip->data)
			span = tor_blt_in_clipped;
		else
			span = tor_blt_in;
	} else {
		assert(op == PictOpAdd);
		if (dst->pCompositeClip->data)
			span = tor_blt_add_clipped;
		else
			span = tor_blt_add;
	}

	DBG(("%s: move-to-cpu(dst)\n", __FUNCTION__));
	region.data = NULL;
	if (!sna_drawable_move_region_to_cpu(dst->pDrawable, &region,
					     op == PictOpSrc ? MOVE_WRITE | MOVE_INPLACE_HINT : MOVE_WRITE | MOVE_READ))
		return true;

	dx = dst->pDrawable->x * SAMPLES_X;
	dy = dst->pDrawable->y * SAMPLES_Y;

	inplace.ptr = pixmap->devPrivate.ptr;
	if (get_drawable_deltas(dst->pDrawable, pixmap, &dst_x, &dst_y))
		inplace.ptr += dst_y * pixmap->devKind + dst_x;
	inplace.stride = pixmap->devKind;
	inplace.opacity = color >> 24;

	num_threads = 1;
	if ((flags & COMPOSITE_SPANS_RECTILINEAR) == 0)
		num_threads = sna_use_threads(region.extents.x2 - region.extents.x1,
					      region.extents.y2 - region.extents.y1,
					      4);
	if (num_threads == 1) {
		struct tor tor;

		if (!tor_init(&tor, &region.extents, 2*ntrap))
			return true;

		for (n = 0; n < ntrap; n++) {
			xTrapezoid t;

			if (pixman_fixed_to_int(traps[n].top) >= region.extents.y2 - dst->pDrawable->y ||
			    pixman_fixed_to_int(traps[n].bottom) < region.extents.y1 - dst->pDrawable->y)
				continue;

			if (!project_trapezoid_onto_grid(&traps[n], dx, dy, &t))
				continue;

			tor_add_edge(&tor, &t, &t.left, 1);
			tor_add_edge(&tor, &t, &t.right, -1);
		}

		tor_render(NULL, &tor, (void*)&inplace,
			   dst->pCompositeClip, span, unbounded);

		tor_fini(&tor);
	} else {
		struct inplace_thread threads[num_threads];
		int y, h;

		DBG(("%s: using %d threads for inplace compositing %dx%d\n",
		     __FUNCTION__, num_threads,
		     region.extents.x2 - region.extents.x1,
		     region.extents.y2 - region.extents.y1));

		threads[0].traps = traps;
		threads[0].ntrap = ntrap;
		threads[0].inplace = inplace;
		threads[0].extents = region.extents;
		threads[0].clip = dst->pCompositeClip;
		threads[0].span = span;
		threads[0].unbounded = unbounded;
		threads[0].dx = dx;
		threads[0].dy = dy;
		threads[0].draw_x = dst->pDrawable->x;
		threads[0].draw_y = dst->pDrawable->y;

		y = region.extents.y1;
		h = region.extents.y2 - region.extents.y1;
		h = (h + num_threads - 1) / num_threads;

		for (n = 1; n < num_threads; n++) {
			threads[n] = threads[0];
			threads[n].extents.y1 = y;
			threads[n].extents.y2 = y += h;

			sna_threads_run(inplace_thread, &threads[n]);
		}

		threads[0].extents.y1 = y;
		threads[0].extents.y2 = region.extents.y2;
		inplace_thread(&threads[0]);

		sna_threads_wait();
	}

	return true;
}

bool
precise_trapezoid_span_fallback(CARD8 op, PicturePtr src, PicturePtr dst,
				PictFormatPtr maskFormat, unsigned flags,
				INT16 src_x, INT16 src_y,
				int ntrap, xTrapezoid *traps)
{
	ScreenPtr screen = dst->pDrawable->pScreen;
	PixmapPtr scratch;
	PicturePtr mask;
	BoxRec extents;
	int16_t dst_x, dst_y;
	int dx, dy, num_threads;
	int error, n;

	if (NO_PRECISE)
		return false;

	if (maskFormat == NULL && ntrap > 1) {
		DBG(("%s: individual rasterisation requested\n",
		     __FUNCTION__));
		do {
			/* XXX unwind errors? */
			if (!precise_trapezoid_span_fallback(op, src, dst, NULL, flags,
							     src_x, src_y, 1, traps++))
				return false;
		} while (--ntrap);
		return true;
	}

	if (!trapezoids_bounds(ntrap, traps, &extents))
		return true;

	DBG(("%s: ntraps=%d, extents (%d, %d), (%d, %d)\n",
	     __FUNCTION__, ntrap, extents.x1, extents.y1, extents.x2, extents.y2));

	if (!sna_compute_composite_extents(&extents,
					   src, NULL, dst,
					   src_x, src_y,
					   0, 0,
					   extents.x1, extents.y1,
					   extents.x2 - extents.x1,
					   extents.y2 - extents.y1))
		return true;

	DBG(("%s: extents (%d, %d), (%d, %d)\n",
	     __FUNCTION__, extents.x1, extents.y1, extents.x2, extents.y2));

	extents.y2 -= extents.y1;
	extents.x2 -= extents.x1;
	extents.x1 -= dst->pDrawable->x;
	extents.y1 -= dst->pDrawable->y;
	dst_x = extents.x1;
	dst_y = extents.y1;
	dx = -extents.x1 * SAMPLES_X;
	dy = -extents.y1 * SAMPLES_Y;
	extents.x1 = extents.y1 = 0;

	DBG(("%s: mask (%dx%d), dx=(%d, %d)\n",
	     __FUNCTION__, extents.x2, extents.y2, dx, dy));
	scratch = sna_pixmap_create_unattached(screen,
					       extents.x2, extents.y2, 8);
	if (!scratch)
		return true;

	DBG(("%s: created buffer %p, stride %d\n",
	     __FUNCTION__, scratch->devPrivate.ptr, scratch->devKind));

	num_threads = 1;
	if ((flags & COMPOSITE_SPANS_RECTILINEAR) == 0)
		num_threads = sna_use_threads(extents.x2 - extents.x1,
					      extents.y2 - extents.y1,
					      4);
	if (num_threads == 1) {
		struct tor tor;

		if (!tor_init(&tor, &extents, 2*ntrap)) {
			sna_pixmap_destroy(scratch);
			return true;
		}

		for (n = 0; n < ntrap; n++) {
			xTrapezoid t;

			if (pixman_fixed_to_int(traps[n].top) - dst_y >= extents.y2 ||
			    pixman_fixed_to_int(traps[n].bottom) - dst_y < 0)
				continue;

			if (!project_trapezoid_onto_grid(&traps[n], dx, dy, &t))
				continue;

			tor_add_edge(&tor, &t, &t.left, 1);
			tor_add_edge(&tor, &t, &t.right, -1);
		}

		if (extents.x2 <= TOR_INPLACE_SIZE) {
			tor_inplace(&tor, scratch);
		} else {
			tor_render(NULL, &tor,
				   scratch->devPrivate.ptr,
				   (void *)(intptr_t)scratch->devKind,
				   tor_blt_mask,
				   true);
		}
		tor_fini(&tor);
	} else {
		struct mask_thread threads[num_threads];
		int y, h;

		DBG(("%s: using %d threads for mask compositing %dx%d\n",
		     __FUNCTION__, num_threads,
		     extents.x2 - extents.x1,
		     extents.y2 - extents.y1));

		threads[0].scratch = scratch;
		threads[0].traps = traps;
		threads[0].ntrap = ntrap;
		threads[0].extents = extents;
		threads[0].dx = dx;
		threads[0].dy = dy;
		threads[0].dst_y = dst_y;

		y = extents.y1;
		h = extents.y2 - extents.y1;
		h = (h + num_threads - 1) / num_threads;

		for (n = 1; n < num_threads; n++) {
			threads[n] = threads[0];
			threads[n].extents.y1 = y;
			threads[n].extents.y2 = y += h;

			sna_threads_run(mask_thread, &threads[n]);
		}

		threads[0].extents.y1 = y;
		mask_thread(&threads[0]);

		sna_threads_wait();
	}

	mask = CreatePicture(0, &scratch->drawable,
			     PictureMatchFormat(screen, 8, PICT_a8),
			     0, 0, serverClient, &error);
	if (mask) {
		RegionRec region;
		int16_t x0, y0;

		region.extents.x1 = dst_x + dst->pDrawable->x;
		region.extents.y1 = dst_y + dst->pDrawable->y;
		region.extents.x2 = region.extents.x1 + extents.x2;
		region.extents.y2 = region.extents.y1 + extents.y2;
		region.data = NULL;

		trapezoid_origin(&traps[0].left, &x0, &y0);

		DBG(("%s: fbComposite()\n", __FUNCTION__));
		sna_composite_fb(op, src, mask, dst, &region,
				 src_x + dst_x - x0, src_y + dst_y - y0,
				 0, 0,
				 dst_x, dst_y,
				 extents.x2, extents.y2);

		FreePicture(mask, 0);
	}
	sna_pixmap_destroy(scratch);

	return true;
}
