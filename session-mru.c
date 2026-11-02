/*
 * Copyright (c) 2026 OpenAI
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include "tmux.h"

struct session_mru_entry {
	u_int id;
	struct timeval attached;
};

struct session_mru {
	struct session_mru_entry *entries;
	u_int count;
	struct menu_view view;
	struct grid_cell style, border_style, selected_style;
	enum box_lines border_lines;
};

static int
session_mru_compare(const void *a, const void *b)
{
	const struct session_mru_entry *ea = a, *eb = b;

	if (timercmp(&ea->attached, &eb->attached, >))
		return (-1);
	if (timercmp(&ea->attached, &eb->attached, <))
		return (1);
	return ((ea->id > eb->id) - (ea->id < eb->id));
}

static void
session_mru_check(__unused struct client *c, void *data, u_int px, u_int py,
    u_int nx, struct overlay_ranges *r)
{
	struct session_mru *mru = data;
	struct menu_view *mv = &mru->view;

	server_client_overlay_range(mv->px, mv->py, mv->menu->width + 4,
	    mv->visible + 2, px, py, nx, r);
}

static struct screen *
session_mru_mode(__unused struct client *c, void *data, u_int *cx, u_int *cy)
{
	struct session_mru *mru = data;
	struct menu_view *mv = &mru->view;

	*cx = mv->px + 2;
	*cy = mv->py + 1 + menu_view_choice_row(mv, mv->choice);
	return (&mv->s);
}

static void
session_mru_draw(struct client *c, void *data,
    __unused struct screen_redraw_ctx *rctx)
{
	struct session_mru *mru = data;
	struct menu_view *mv = &mru->view;
	overlay_check_cb check = c->overlay_check;
	u_int i;

	menu_view_draw(mv, mru->border_lines, &mru->style,
	    &mru->border_style, &mru->selected_style);
	/* There are no upper views: allow the owned view to draw itself. */
	c->overlay_check = NULL;
	for (i = 0; i < screen_size_y(&mv->s); i++)
		tty_draw_line(&c->tty, &mv->s, 0, i, mv->menu->width + 4,
		    mv->px, mv->py + i, &grid_default_cell, NULL);
	c->overlay_check = check;
}

static void
session_mru_free(struct client *c, void *data)
{
	struct session_mru *mru = data;
	struct menu *menu = mru->view.menu;

	c->session_mru = NULL;
	menu_view_free(&mru->view);
	menu_free(menu);
	free(mru->entries);
	free(mru);
}

void
session_mru_cancel(struct client *c)
{
	if (c->session_mru != NULL)
		server_client_clear_overlay(c);
}

int
session_mru_start(struct client *c, int reverse, char **cause)
{
	struct session_mru *mru;
	struct session *s;
	struct menu *menu;
	struct cmd_find_state fs;
	struct options *o;
	struct menu_item item = { "#{session_id}  #{q/e:session_name}", KEYC_NONE, NULL };
	u_int i = 0, start, px, py;

	if (c->session == NULL || !tty_kitty_ready(&c->tty)) {
		*cause = xstrdup("terminal Ctrl-release reporting is not ready");
		return (-1);
	}
	if (c->session_mru != NULL || c->overlay_draw != NULL ||
	    c->prompt_string != NULL || (c->flags & CLIENT_READONLY) ||
	    (~c->flags & CLIENT_FOCUSED)) {
		*cause = xstrdup("client is not available for session switching");
		return (-1);
	}
	if (c->tty.sx < 8 || c->tty.sy < 4) {
		*cause = xstrdup("terminal is too small for session switching");
		return (-1);
	}
	mru = xcalloc(1, sizeof *mru);
	RB_FOREACH(s, sessions, &sessions)
		mru->count++;
	mru->entries = xcalloc(mru->count, sizeof *mru->entries);
	mru->entries[i++].id = c->session->id;
	RB_FOREACH(s, sessions, &sessions) {
		if (s == c->session)
			continue;
		mru->entries[i].id = s->id;
		mru->entries[i++].attached = s->last_attached_time;
	}
	qsort(mru->entries + 1, mru->count - 1, sizeof *mru->entries,
	    session_mru_compare);
	menu = menu_create("Sessions (release Ctrl to select)");
	for (i = 0; i < mru->count; i++) {
		s = session_find_by_id(mru->entries[i].id);
		cmd_find_from_session(&fs, s, 0);
		menu_add_item(menu, &item, NULL, c, &fs);
	}
	start = reverse ? mru->count - 1 : (mru->count > 1 ? 1 : 0);
	px = c->tty.sx > menu->width + 4 ?
	    (c->tty.sx - menu->width - 4) / 2 : 0;
	py = c->tty.sy > menu->count + 2 ?
	    (c->tty.sy - menu->count - 2) / 2 : 0;
	if (menu_view_init(&mru->view, menu, start, px, py,
	    c->tty.sx, c->tty.sy) != 0) {
		menu_free(menu);
		free(mru->entries);
		free(mru);
		*cause = xstrdup("cannot display session menu");
		return (-1);
	}
	o = c->session->curw->window->options;
	mru->border_lines = options_get_number(o, "menu-border-lines");
	memcpy(&mru->style, &grid_default_cell, sizeof mru->style);
	memcpy(&mru->border_style, &grid_default_cell, sizeof mru->border_style);
	memcpy(&mru->selected_style, &grid_default_cell, sizeof mru->selected_style);
	style_apply(&mru->style, o, "menu-style", NULL);
	style_apply(&mru->border_style, o, "menu-border-style", NULL);
	style_apply(&mru->selected_style, o, "menu-selected-style", NULL);
	mru->style.attr = mru->border_style.attr = mru->selected_style.attr = 0;
	c->session_mru = mru;
	server_client_set_overlay(c, 0, session_mru_check, session_mru_mode,
	    session_mru_draw, NULL, session_mru_free, NULL, mru);
	return (0);
}

/* Called in key queue order, including the first gesture's queued release. */
int
session_mru_key(struct client *c, struct key_event *event)
{
	struct session_mru *mru = c->session_mru;
	struct session *s;
	key_code key = event->key & ~KEYC_MASK_FLAGS;
	u_int id, choice;

	/* A queued release must not confirm after adapter loss. */
	if (mru != NULL && !tty_kitty_ready(&c->tty)) {
		session_mru_cancel(c);
		mru = NULL;
	}
	if (mru == NULL)
		return ((key & KEYC_MASK_KEY) == KEYC_CTRL_RELEASE);
	if ((key & KEYC_MASK_KEY) == KEYC_CTRL_RELEASE) {
		if (key & KEYC_CTRL)
			return (1);
		id = mru->entries[mru->view.choice].id;
		/* Free the view before external-switch cancellation can run. */
		session_mru_cancel(c);
		if ((s = session_find_by_id(id)) != NULL) {
			environ_update(s->options, c->environ, s->environ);
			server_client_set_session(c, s);
			server_client_set_key_table(c, NULL);
		}
		return (1);
	}
	if (key == ('\011'|KEYC_CTRL) || key == (KEYC_BTAB|KEYC_CTRL)) {
		choice = mru->view.choice;
		if (key == ('\011'|KEYC_CTRL))
			choice = (choice + 1) % mru->count;
		else
			choice = choice == 0 ? mru->count - 1 : choice - 1;
		menu_view_select(&mru->view, choice);
		c->flags |= CLIENT_REDRAWOVERLAY;
		return (1);
	}
	session_mru_cancel(c);
	return (key == '\033' || key == KEYC_FOCUS_OUT);
}
