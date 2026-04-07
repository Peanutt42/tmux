/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Peter Hennig <peterhennig42@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "tmux.h"

struct win_slide_anim_pending_key_event {
	/*
	 * allocated by us as a deep copy, freed when dispatching with
	 * `server_client_handle_key`, or manually on `win_slide_anim_free`
	 */
	struct key_event *event;
	TAILQ_ENTRY(win_slide_anim_pending_key_event) entry;
};

static struct key_event *deep_copy_key_event(struct key_event *key_event);

struct win_slide_anim_data {
	u_int new_window_id;
	int old_window_provided;
	u_int old_window_id;
	enum win_slide_anim_direction direction;
	uint64_t start_time_ms;
	struct timespec last_frame_time;
	TAILQ_HEAD(, win_slide_anim_pending_key_event) pending_key_events;
};

/* configuration getters */
int
is_win_slide_anim_enabled(struct client *c)
{
	if (c == NULL || c->session == NULL)
		return (WIN_SLIDE_ANIM_DEFAULT_ENABLED);
	return (options_get_number(c->session->options, "window-slide-animation"));
}

static u_int
win_slide_anim_get_duration_ms(struct client *c)
{
	if (c == NULL || c->session == NULL)
		return (WIN_SLIDE_ANIM_DEFAULT_DURATION_MS);
	return (options_get_number(c->session->options,
							   "window-slide-animation-duration"));
}

static enum win_slide_anim_axis
win_slide_anim_get_axis(struct client *c)
{
	int choice;
	if (c == NULL || c->session == NULL)
		return (WIN_SLIDE_ANIM_DEFAULT_AXIS);
	choice =
		options_get_number(c->session->options, "window-slide-animation-axis");
	return ((enum win_slide_anim_axis)choice);
}

static float
win_slide_anim_get_target_fps(struct client *c)
{
	if (c == NULL || c->session == NULL)
		return ((float)(WIN_SLIDE_ANIM_DEFAULT_TARGET_FPS));
	return ((float)((int)options_get_number(
		c->session->options, "window-slide-animation-target-fps")));
}

static enum win_slide_anim_ease_function
win_slide_anim_get_ease_function(struct client *c)
{
	int choice;
	if (c == NULL || c->session == NULL)
		return (WIN_SLIDE_ANIM_DEFAULT_EASE_FUNCTION);
	choice = options_get_number(c->session->options,
								"window-slide-animation-ease-function");
	return ((enum win_slide_anim_ease_function)choice);
}

/*
 * `data_ptr` is ptr to pending_key_event (key_event*), needs to be freed by
 * this function
 */
static enum cmd_retval win_slide_anim_key_callback(struct cmdq_item *item,
												   void *data_ptr);

/* `data_ptr` is client* */
static void win_slide_anim_timer(__unused int fd, __unused short events,
								 void *data_ptr);

/* `data_ptr` is window_slide_animation* */
static void win_slide_anim_free(__unused struct client *c, void *data_ptr);

/* `data_ptr` is window_slide_animation* */
static void win_slide_anim_resize(__unused struct client *c, __unused void *data_ptr);

/* `data_ptr` is window_slide_animation* */
static void win_slide_anim_draw(struct client *c, void *data_ptr,
								__unused struct screen_redraw_ctx *ctx);

static void
win_slide_anim_clear(struct tty *tty);

static void win_slide_anim_draw_window(struct tty *tty, struct window *window,
									   int draw_x_offset, int draw_y_offset);
/*
 * draws a pane's region (start_x,start_y with width,height) onto the screen
 * with offset
 */
static void win_slide_anim_draw_pane(struct tty *tty, struct window_pane *wp,
									 u_int start_x, u_int start_y, u_int width,
									 u_int height, u_int screen_offset_x,
									 u_int screen_offset_y);

/* `data_ptr` is window_slide_animation* */
static int win_slide_anim_key(struct client *c, void *data_ptr,
							  struct key_event *event);

static float
win_slide_anim_calculate_ease_function(enum win_slide_anim_ease_function fn,
									   float x);

static float clamp01f(float x);

/*
 * the allocated `window_slide_animation` will free itself once the animation is
 * finished. should check `is_win_slide_anim_enabled` before calling
 */
void
win_slide_anim_start(struct client *c, struct session *s,
					 struct winlink *old_wl)
{
	struct winlink *new_wl = s->curw;

	enum win_slide_anim_direction direction;

	if (c->overlay_draw == win_slide_anim_draw || new_wl == old_wl)
		return;

	if (old_wl) {
		direction = (new_wl->idx < old_wl->idx) ? WIN_SLIDE_ANIM_LEFT_OR_UP
												: WIN_SLIDE_ANIM_RIGHT_OR_DOWN;
	} else {
		/* `old_wl` should generally not be null here, but better safe than
		 * sorry */
		direction = WIN_SLIDE_ANIM_RIGHT_OR_DOWN;
	}

	win_slide_anim_start_ext(c, new_wl, old_wl, direction);
}

/*
 * `old_wl` is allowed to be `NULL`, when the old window is destroyed.
 * should check `is_win_slide_anim_enabled` before calling
 */
void
win_slide_anim_start_ext(struct client *c, struct winlink *new_wl,
						 struct winlink *old_wl,
						 enum win_slide_anim_direction direction)
{
	struct win_slide_anim_data *data;
	uint64_t		now_ms;
	struct timespec	now_ts;
	struct timeval	next_frame_delay;

	if (c->overlay_draw == win_slide_anim_draw || new_wl == old_wl)
		return;

	now_ms = get_timer();

	clock_gettime(CLOCK_MONOTONIC, &now_ts);

	data = xmalloc(sizeof *data);
	data->new_window_id = new_wl->window->id;
	data->old_window_provided = old_wl != NULL;
	/* UINT_MAX as in invalid */
	data->old_window_id = old_wl ? old_wl->window->id : UINT_MAX;
	data->direction = direction;
	data->start_time_ms = now_ms;
	data->last_frame_time = now_ts;
	TAILQ_INIT(&data->pending_key_events);
	server_client_set_overlay(c, 0, NULL, NULL, win_slide_anim_draw,
							  win_slide_anim_key, win_slide_anim_free,
							  win_slide_anim_resize, data);

	evtimer_set(&c->overlay_timer, win_slide_anim_timer, c);

	next_frame_delay.tv_sec = 0;
	next_frame_delay.tv_usec =
		roundf((1.0f / win_slide_anim_get_target_fps(c)) * 1000.0f * 1000.0f);
	evtimer_add(&c->overlay_timer, &next_frame_delay);
}

static void
win_slide_anim_free(__unused struct client *c, void *data_ptr)
{
	struct win_slide_anim_data *data = data_ptr;

	struct win_slide_anim_pending_key_event *pending_key_event;

	if (data == NULL)
		return;

	while ((pending_key_event = TAILQ_FIRST(&data->pending_key_events)) !=
		   NULL)
	{
		TAILQ_REMOVE(&data->pending_key_events, pending_key_event, entry);
		free(pending_key_event->event->buf);
		free(pending_key_event->event);
		free(pending_key_event);
	}

	free(data);
}

static void
win_slide_anim_resize(__unused struct client *c, __unused void *data_ptr)
{
}

static void
win_slide_anim_timer(__unused int fd, __unused short events, void *data_ptr)
{
	struct client	*c = data_ptr;
	struct win_slide_anim_data *data = c->overlay_data;
	uint64_t		time_passed_ms;
	struct timespec	now_ts;
	uint64_t		last_frame_usec;
	uint64_t		usec_till_next_frame;
	uint64_t		target_frame_time_usec;
	struct timeval	next_frame_delay;

	time_passed_ms = get_timer() - data->start_time_ms;

	if (time_passed_ms >= (uint64_t)win_slide_anim_get_duration_ms(c)) {
		/* dispatch all pending key events */
		struct win_slide_anim_pending_key_event *pending_key_event = NULL;
		while ((pending_key_event = TAILQ_FIRST(&data->pending_key_events)) !=
			   NULL)
		{
			TAILQ_REMOVE(&data->pending_key_events, pending_key_event, entry);
			cmdq_append(c, cmdq_get_callback(win_slide_anim_key_callback,
											 pending_key_event->event));
			free(pending_key_event);
		}

		server_client_clear_overlay(c);
	} else {
		clock_gettime(CLOCK_MONOTONIC, &now_ts);

		target_frame_time_usec =
			roundf((1.0f / win_slide_anim_get_target_fps(c)) * 1000000.0f);

		server_redraw_client(c);

		last_frame_usec =
			(now_ts.tv_sec - data->last_frame_time.tv_sec) * 1000000 +
			(now_ts.tv_nsec - data->last_frame_time.tv_nsec) / 1000;

		usec_till_next_frame = target_frame_time_usec > last_frame_usec
								   ? target_frame_time_usec - last_frame_usec
								   : 0;

		next_frame_delay.tv_sec = usec_till_next_frame / 1000000;
		next_frame_delay.tv_usec = usec_till_next_frame % 1000000;
		evtimer_add(&c->overlay_timer, &next_frame_delay);
	}
}

static void
win_slide_anim_draw(struct client *c, void *data_ptr,
		            __unused struct screen_redraw_ctx *ctx)
{
	struct win_slide_anim_data *data = data_ptr;
	struct tty	*tty = &c->tty;
	uint64_t	current_ms, time_passed_ms;
	float		progress;
	u_int		window_width, window_height;
	int			offset_x, offset_y;
	int			draw_x_offset_old = 0, draw_x_offset_new = 0;
	int			draw_y_offset_old = 0, draw_y_offset_new = 0;
	enum win_slide_anim_axis axis;
	struct winlink *new_wl = NULL, *old_wl = NULL, *wl_iter;

	if (c->session == NULL)
		return;

	current_ms = get_timer();
	time_passed_ms = current_ms - data->start_time_ms;
		progress =
		clamp01f((float)time_passed_ms /
				 (float)((int)win_slide_anim_get_duration_ms(c)));
	progress = win_slide_anim_calculate_ease_function(
		win_slide_anim_get_ease_function(c), progress);

	axis = win_slide_anim_get_axis(c);

	RB_FOREACH(wl_iter, winlinks, &c->session->windows)
	{
		if (wl_iter->window->id == data->new_window_id)
			new_wl = wl_iter;
		if (data->old_window_provided &&
			wl_iter->window->id == data->old_window_id)
		{
			old_wl = wl_iter;
		}
	}

	if (new_wl == NULL) {
		log_debug(
			"could not find new window to animate to with id %u, not drawing the win_slide_anim overlay",
			data->new_window_id
		);
		return;
	}

	window_width = new_wl->window->sx;
	window_height = new_wl->window->sy;
	offset_x = roundf((float)window_width * progress);
	offset_y = roundf((float)window_height * progress);

	tty_update_mode(tty, tty->mode, NULL);

	win_slide_anim_clear(tty);

	/* assume direction to be right/down, * (-1) for left/up */
	switch (axis) {
	default:
	case WIN_SLIDE_ANIM_HORIZONTAL:
		draw_x_offset_old = -offset_x;
		draw_x_offset_new = window_width - offset_x;
		if (data->direction == WIN_SLIDE_ANIM_LEFT_OR_UP) {
			draw_x_offset_old *= -1;
			draw_x_offset_new *= -1;
		}
		break;
	case WIN_SLIDE_ANIM_VERTICAL:
		draw_x_offset_old = 0;
		draw_x_offset_new = 0;
		draw_y_offset_old = -offset_y;
		draw_y_offset_new = window_height - offset_y;
		if (data->direction == WIN_SLIDE_ANIM_LEFT_OR_UP) {
			draw_y_offset_old *= -1;
			draw_y_offset_new *= -1;
		}
		break;
	}

	/* if statusline at top, offset y by statusline height */
	if (tty->client->session->statusat == 0) {
		draw_y_offset_old += tty->client->session->statuslines;
		draw_y_offset_new += tty->client->session->statuslines;
	}

	if (old_wl) {
		win_slide_anim_draw_window(tty, old_wl->window, draw_x_offset_old,
								   draw_y_offset_old);
	}

	win_slide_anim_draw_window(tty, new_wl->window, draw_x_offset_new,
							   draw_y_offset_new);

	tty_reset(tty);

	clock_gettime(CLOCK_MONOTONIC, &data->last_frame_time);
}

static void
win_slide_anim_draw_window(struct tty *tty, struct window *window,
						   int draw_x_offset, int draw_y_offset)
{
	struct session		*s = tty->client->session;
	struct window_panes *panes = &window->panes;
	struct window_pane	*wp = NULL;
	int		draw_x = 0, draw_y = 0;
	u_int	screen_offset_x = 0, screen_offset_y = 0;
	u_int	width = 0, height = 0;
	u_int	min_y = 0, max_y = tty->sy;

	/* statusline at top */
	if (s->statusat == 0)
		min_y = s->statuslines;
	else if (s->statusat == 1) /* statusline at bottom */
		max_y = tty->sy - s->statuslines;

	TAILQ_FOREACH(wp, panes, entry)
	{
		draw_x = wp->xoff + draw_x_offset;
		draw_y = wp->yoff + draw_y_offset;
		screen_offset_x = 0;
		screen_offset_y = 0;
		width = wp->sx;
		height = wp->sy;

		if (draw_x >= (int)tty->sx || draw_x + (int)width <= 0 ||
			draw_y >= (int)max_y || draw_y + (int)height <= (int)min_y)
		{
			continue;
		}

		if (draw_x < 0) {
			screen_offset_x = (u_int)(-draw_x);
			width -= screen_offset_x;
			draw_x = 0;
		}
		if (draw_x + (int)width > (int)tty->sx)
			width = (int)tty->sx - draw_x;

		if (draw_y < (int)min_y) {
			screen_offset_y = (u_int)(min_y - draw_y);
			height -= min_y - draw_y;
			draw_y = min_y;
		}
		if (draw_y + (int)height > (int)max_y)
			height = (int)max_y - draw_y;

		win_slide_anim_draw_pane(tty, wp, (u_int)draw_x, (u_int)draw_y, width,
								 height, screen_offset_x, screen_offset_y);
	}
}

/*
 * `start_x`, `start_y`: tty space coords
 * `width`, `height`: area that will have the pane drawn on
 * `screen_offset_x`, screen_offset_y`: offset of the start in pane space
 *	all parameters are assumed to be all within the visible tty area (excluding the statusline)
 */
static void
win_slide_anim_draw_pane(struct tty *tty, struct window_pane *wp,
						 u_int start_x, u_int start_y, u_int width,
						 u_int height, u_int screen_offset_x,
						 u_int screen_offset_y)
{
	struct grid_cell	defaults;
	u_int				y;

	tty_default_colours(&defaults, wp);

	for (y = 0; y < height; y++) {
		tty_draw_line(tty, wp->screen, screen_offset_x, y + screen_offset_y,
					  width, start_x, start_y + y, &defaults, &wp->palette);
	}
}

/* 
 * clears entire tty portion of panes, borders and scrollbars,
 * as they are not drawn in the animation
 */
static void
win_slide_anim_clear(struct tty *tty)
{
	struct session		*s = tty->client->session;
	struct grid_cell	 gc;
	u_int				 y = 0, max_height = tty->sy;

	memcpy(&gc, &grid_default_cell, sizeof gc);

	tty_attributes(tty, &gc, &gc, NULL, NULL);

	/* status at top -> start at offset */
	if (s->statusat == 0)
		y = s->statuslines;
	else if (s->statusat == 1) /* status at bottom -> stop early */
		max_height -= s->statuslines;

	for (; y < max_height; y++) {
		tty_cursor(tty, 0, y);
		tty_repeat_space(tty, tty->sx);
	}

	tty_reset(tty);
}

/* wraps `server_client_handle_key` around a command */
static enum cmd_retval
win_slide_anim_key_callback(struct cmdq_item *item,
							void *data)
{
	struct key_event	*event = data;
	struct client		*c;

	c = cmdq_get_client(item);

	/* frees event */
	server_client_handle_key(c, event);

	return (CMD_RETURN_NORMAL);
}

/* gets called when a key/mouse event is emitted while the animation overlay is
 * active */
static int
win_slide_anim_key(__unused struct client *c, void *data_ptr, struct key_event *event)
{
	struct win_slide_anim_data *data = data_ptr;
	struct win_slide_anim_pending_key_event *pending_key_event;

	pending_key_event = xmalloc(sizeof *pending_key_event);
	pending_key_event->event = deep_copy_key_event(event);
	TAILQ_INSERT_TAIL(&data->pending_key_events, pending_key_event, entry);

	return (0);
}

static struct key_event *
deep_copy_key_event(struct key_event *key_event)
{
	struct key_event *copy;

	copy = xmalloc(sizeof *copy);
	memcpy(copy, key_event, sizeof *copy);
	if (key_event->buf)
		copy->buf = xstrndup(key_event->buf, key_event->len);

	return (copy);
}

/* see https://easings.net */
static float
win_slide_anim_calculate_ease_function(enum win_slide_anim_ease_function fn,
									   float x)
{
	switch (fn) {
	case WIN_SLIDE_ANIM_EASE_LINEAR:
		return (x);
	case WIN_SLIDE_ANIM_EASE_IN_OUT_SINE:
		return (-(cosf(M_PI * x) - 1.0f) / 2.0f);
	case WIN_SLIDE_ANIM_EASE_IN_OUT_QUAD:
		if (x < 0.5f)
			return (2.0f * x * x);
		else
			return (1.0f - (powf(-2.0f * x + 2.0f, 2.0f) / 2.0f));
	case WIN_SLIDE_ANIM_EASE_IN_OUT_CUBIC:
		if (x < 0.5f)
			return (4.0f * x * x * x);
		else
			return (1.0f - (powf(-2.0f * x + 2.0f, 3.0f) / 2.0f));
	case WIN_SLIDE_ANIM_EASE_IN_OUT_QUART:
		if (x < 0.5f)
			return (8.0f * x * x * x * x);
		else
			return (1.0f - (powf(-2.0f * x + 2.0f, 4.0f) / 2.0f));
	case WIN_SLIDE_ANIM_EASE_IN_OUT_QUINT:
		if (x < 0.5f)
			return (16.0f * x * x * x * x * x);
		else
			return (1.0f - (powf(-2.0f * x + 2.0f, 5.0f) / 2.0f));
	case WIN_SLIDE_ANIM_EASE_IN_OUT_EXPO:
		if (x == 0.0f || x == 1.0f)
			return (x);
		if (x < 0.5f)
			return (powf(2.0f, (20.0f * x) - 10.0f) / 2.0f);
		else
			return ((2.0f - powf(2.0f, (-20.0f * x) + 10.0f)) / 2.0f);
	case WIN_SLIDE_ANIM_EASE_IN_OUT_CIRC:
		if (x < 0.5f)
			return ((1.0f - sqrtf(1.0f - powf(2.0f * x, 2.0f))) / 2.0f);
		else
			return ((sqrtf(1.0f - powf((-2.0f * x) + 2.0f, 2.0f)) + 1.0f) / 2.0f);
	default:
		log_debug("invalid win_slide_anim_ease_function of value %i", fn);
		/* linear */
		return (x);
	}
}

/* clamps `value` between 0.0f and 1.0f */
static float
clamp01f(float value)
{
	return (fminf(fmaxf(value, 0.0f), 1.0f));
}
