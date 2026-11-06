/* $OpenBSD$ */

/*
 * Copyright (c) 2019 Nicholas Marriott <nicholas.marriott@gmail.com>
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

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

struct menu_data {
	struct cmdq_item	*item;
	int			 flags;

	struct grid_cell	 style;
	struct grid_cell	 border_style;
	struct grid_cell	 selected_style;
	enum box_lines		 border_lines;

	struct cmd_find_state	 fs;
	struct menu		*root;
	struct menu_view	 levels[MENU_MAX_LEVELS];
	u_int			 depth;

	menu_choice_cb		 cb;
	void			*data;
};

struct menu_draw_data {
	struct menu_data	*md;
	u_int		 depth;
};

void
menu_add_items(struct menu *menu, const struct menu_item *items,
    struct cmdq_item *qitem, struct client *c, struct cmd_find_state *fs)
{
	const struct menu_item	*loop;

	for (loop = items; loop->name != NULL; loop++)
		menu_add_item(menu, loop, qitem, c, fs);
}

void
menu_add_item(struct menu *menu, const struct menu_item *item,
    struct cmdq_item *qitem, struct client *c, struct cmd_find_state *fs)
{
	menu_add_submenu(menu, item, NULL, qitem, c, fs);
}

void
menu_add_submenu(struct menu *menu, const struct menu_item *item,
    struct menu *submenu, struct cmdq_item *qitem, struct client *c,
    struct cmd_find_state *fs)
{
	struct menu_item	*new_item;
	const char		*key = NULL, *cmd, *suffix = "";
	char			*s, *trimmed, *name;
	u_int			 width, max_width;
	int			 line, has_submenu;
	size_t			 keylen, slen;

	line = (item == NULL || item->name == NULL || *item->name == '\0');
	if (line && menu->count == 0)
		return;
	if (line && menu->items[menu->count - 1].name == NULL)
		return;

	menu->items = xreallocarray(menu->items, menu->count + 1,
	    sizeof *menu->items);
	menu->submenus = xreallocarray(menu->submenus, menu->count + 1,
	    sizeof *menu->submenus);
	menu->submenus[menu->count] = submenu;
	new_item = &menu->items[menu->count++];
	memset(new_item, 0, sizeof *new_item);

	if (line)
		return;
	has_submenu = (submenu != NULL);

	if (fs != NULL)
		s = format_single_from_state(qitem, item->name, c, fs);
	else
		s = format_single(qitem, item->name, c, NULL, NULL, NULL);
	if (*s == '\0') { /* no item if empty after format expanded */
		menu->count--;
		return;
	}
	max_width = c->tty.sx - 4;
	if (has_submenu && max_width > 2)
		max_width -= 2;

	slen = strlen(s);
	if (*s != '-' && item->key != KEYC_UNKNOWN && item->key != KEYC_NONE) {
		key = key_string_lookup_key(item->key, 0);
		keylen = strlen(key) + 3; /* 3 = space and two brackets */

		/*
		 * Add the key if it is shorter than a quarter of the available
		 * space or there is space for the entire item text and the
		 * key.
		 */
		if (keylen <= max_width / 4)
			max_width -= keylen;
		else if (keylen >= max_width || slen >= max_width - keylen)
			key = NULL;
	}

	if (slen > max_width) {
		max_width--;
		suffix = ">";
	}
	trimmed = format_trim_right(s, max_width);
	if (key != NULL && has_submenu) {
		xasprintf(&name, "%s%s#[default] #[align=right](%s) >",
		    trimmed, suffix, key);
	} else if (key != NULL) {
		xasprintf(&name, "%s%s#[default] #[align=right](%s)",
		    trimmed, suffix, key);
	} else if (has_submenu) {
		xasprintf(&name, "%s%s#[default] #[align=right]>",
		    trimmed, suffix);
	} else
		xasprintf(&name, "%s%s", trimmed, suffix);
	free(trimmed);

	new_item->name = name;
	free(s);

	cmd = item->command;
	if (cmd != NULL) {
		if (fs != NULL)
			s = format_single_from_state(qitem, cmd, c, fs);
		else
			s = format_single(qitem, cmd, c, NULL, NULL, NULL);
	} else
		s = NULL;
	new_item->command = s;
	new_item->key = item->key;

	width = format_width(new_item->name);
	if (*new_item->name == '-')
		width--;
	if (width > menu->width)
		menu->width = width;
}

struct menu *
menu_create(const char *title)
{
	struct menu	*menu;

	menu = xcalloc(1, sizeof *menu);
	menu->title = xstrdup(title);
	menu->width = format_width(title);

	return (menu);
}

void
menu_free(struct menu *menu)
{
	u_int	i;

	for (i = 0; i < menu->count; i++) {
		free((void *)menu->items[i].name);
		free((void *)menu->items[i].command);
		if (menu->submenus[i] != NULL)
			menu_free(menu->submenus[i]);
	}
	free(menu->items);
	free(menu->submenus);

	free((void *)menu->title);
	free(menu);
}

static int
menu_item_enabled(struct menu *menu, int choice)
{
	const char	*name;

	if (choice < 0 || choice >= (int)menu->count)
		return (0);
	name = menu->items[choice].name;
	return (name != NULL && *name != '-');
}

static int
menu_find_choice(struct menu *menu, int choice, int direction)
{
	int	start;

	if (menu->count == 0)
		return (-1);
	if (choice < 0 || choice >= (int)menu->count)
		choice = (direction > 0 ? 0 : menu->count - 1);
	start = choice;
	for (;;) {
		if (menu_item_enabled(menu, choice))
			return (choice);
		choice += direction;
		if (choice < 0)
			choice = menu->count - 1;
		else if (choice >= (int)menu->count)
			choice = 0;
		if (choice == start)
			return (-1);
	}
}

static int
menu_move_choice(struct menu *menu, int choice, int direction, u_int amount,
    int wrap)
{
	int	 next;
	u_int	 moved = 0;

	if (choice < 0 || choice >= (int)menu->count) {
		choice = (direction > 0 ? 0 : menu->count - 1);
		return (menu_find_choice(menu, choice, direction));
	}
	while (moved < amount) {
		next = choice;
		for (;;) {
			next += direction;
			if (next < 0 || next >= (int)menu->count) {
				if (!wrap)
					return (choice);
				next = (direction > 0 ? 0 : menu->count - 1);
			}
			if (next == choice)
				return (choice);
			if (menu_item_enabled(menu, next))
				break;
		}
		choice = next;
		moved++;
	}
	return (choice);
}

static int
menu_level_init(struct menu_data *md, struct menu_view *ml,
    struct menu *menu, int choice, u_int px, u_int py, struct client *c)
{
	if (menu_view_init(ml, menu,
	    choice < 0 ? MENU_CHOICE_NONE : menu_find_choice(menu, choice, 1),
	    px, py, c->tty.sx, c->tty.sy) != 0)
		return (-1);
	if (~md->flags & MENU_NOMOUSE)
		ml->s.mode |= (MODE_MOUSE_ALL|MODE_MOUSE_BUTTON);
	ml->s.mode &= ~MODE_CURSOR;
	return (0);
}

static void
menu_pop(struct menu_data *md, struct client *c, u_int depth)
{
	int	changed = 0;

	while (md->depth > depth) {
		menu_view_free(&md->levels[md->depth]);
		md->depth--;
		changed = 1;
	}
	/* Repaint the pane/status cells uncovered by the removed child boxes. */
	if (changed)
		server_redraw_client(c);
}

static void
menu_redraw(struct client *c)
{
	c->flags |= CLIENT_REDRAWOVERLAY;
}

static int
menu_open_submenu(struct menu_data *md, struct client *c)
{
	struct menu_view	*parent = &md->levels[md->depth], *child;
	struct menu		*submenu;
	u_int			 px, py, width, visible;

	if (!menu_item_enabled(parent->menu, parent->choice))
		return (0);
	submenu = parent->menu->submenus[parent->choice];
	if (submenu == NULL || md->depth + 1 >= MENU_MAX_LEVELS)
		return (0);

	menu_pop(md, c, md->depth);
	width = submenu->width + 4;
	if (parent->px + parent->menu->width + 4 + width <= c->tty.sx)
		px = parent->px + parent->menu->width + 4;
	else if (parent->px >= width)
		px = parent->px - width;
	else
		px = c->tty.sx - width;
	visible = submenu->count;
	if (visible + 2 > c->tty.sy)
		visible = c->tty.sy - 2;
	py = parent->py + 1 +
	    menu_view_choice_row(parent, parent->choice);
	if (py + visible + 2 > c->tty.sy)
		py = c->tty.sy - visible - 2;

	child = &md->levels[md->depth + 1];
	if (menu_level_init(md, child, submenu, 0, px, py, c) != 0)
		return (0);
	md->depth++;
	menu_redraw(c);
	return (1);
}

static int
menu_level_at(struct menu_data *md, u_int x, u_int y)
{
	struct menu_view	*ml;
	int			 depth;

	for (depth = md->depth; depth >= 0; depth--) {
		ml = &md->levels[depth];
		if (x >= ml->px && x < ml->px + ml->menu->width + 4 &&
		    y >= ml->py && y < ml->py + ml->visible + 2)
			return (depth);
	}
	return (-1);
}

static int
menu_mouse_choice(struct menu_view *ml, u_int x, u_int y)
{
	u_int	row, item_rows;

	if (x < ml->px || x >= ml->px + ml->menu->width + 4 ||
	    y < ml->py + 1 || y >= ml->py + 1 + ml->visible)
		return (MENU_CHOICE_NONE);
	row = y - ml->py - 1;
	if (menu_view_has_up(ml)) {
		if (row == 0)
			return (MENU_CHOICE_UP);
		row--;
	}
	item_rows = menu_view_item_rows(ml);
	if (menu_view_has_down(ml) && row == item_rows)
		return (MENU_CHOICE_DOWN);
	if (row >= item_rows || ml->top + row >= ml->menu->count)
		return (MENU_CHOICE_NONE);
	return (ml->top + row);
}

static int
menu_key_range(key_code key, key_code first, key_code last)
{
	key &= KEYC_MASK_KEY;
	return (key >= first && key <= last);
}

static int
menu_mouse_motion(struct key_event *event)
{
	return (menu_key_range(event->key, KEYC_MOUSEMOVE_PANE,
	    KEYC_MOUSEMOVE_BORDER) ||
	    (MOUSE_DRAG(event->m.b) && MOUSE_RELEASE(event->m.b)));
}

static int
menu_mouse_release(struct key_event *event)
{
	return (menu_key_range(event->key, KEYC_MOUSEUP1_PANE,
	    KEYC_MOUSEUP11_BORDER) ||
	    menu_key_range(event->key, KEYC_MOUSEDRAGEND1_PANE,
	    KEYC_MOUSEDRAGEND11_BORDER) || MOUSE_RELEASE(event->m.b));
}

static int
menu_mouse_drag(struct key_event *event)
{
	return (menu_key_range(event->key, KEYC_MOUSEDRAG1_PANE,
	    KEYC_MOUSEDRAG11_BORDER) || MOUSE_DRAG(event->m.b));
}

static int
menu_mouse_wheel(struct key_event *event)
{
	return (menu_key_range(event->key, KEYC_WHEELUP_PANE,
	    KEYC_WHEELDOWN_BORDER) || MOUSE_WHEEL(event->m.b));
}

static int
menu_mouse_wheel_down(struct key_event *event)
{
	key_code	key = event->key & KEYC_MASK_KEY;

	if (key >= KEYC_WHEELDOWN_PANE && key <= KEYC_WHEELDOWN_BORDER)
		return (1);
	return (MOUSE_BUTTONS(event->m.b) == MOUSE_WHEEL_DOWN);
}

static int
menu_scroll(struct menu_view *ml, int direction, u_int amount)
{
	u_int	first, last, rows;
	int	choice, delta;

	delta = menu_view_scroll(ml, direction, amount);
	if (delta == 0)
		return (0);
	if (ml->choice >= 0) {
		choice = ml->choice + delta;
		first = ml->top;
		rows = menu_view_item_rows(ml);
		if (rows == 0) {
			ml->choice = MENU_CHOICE_NONE;
			return (1);
		}
		last = ml->top + rows - 1;
		if (choice < (int)first)
			choice = first;
		else if (choice > (int)last)
			choice = last;
		while (choice >= (int)first && choice <= (int)last &&
		    !menu_item_enabled(ml->menu, choice))
			choice += (direction > 0 ? 1 : -1);
		if (choice < (int)first || choice > (int)last) {
			choice = (direction > 0 ? first : last);
			while (choice >= (int)first && choice <= (int)last &&
			    !menu_item_enabled(ml->menu, choice))
				choice += (direction > 0 ? 1 : -1);
		}
		if (choice < (int)first || choice > (int)last)
			ml->choice = MENU_CHOICE_NONE;
		else
			ml->choice = choice;
	}
	return (1);
}

struct screen *
menu_mode_cb(__unused struct client *c, void *data, u_int *cx, u_int *cy)
{
	struct menu_data	*md = data;
	struct menu_view	*ml = &md->levels[md->depth];

	*cx = ml->px + 2;
	if (ml->choice == MENU_CHOICE_NONE)
		*cy = ml->py;
	else if (ml->choice == MENU_CHOICE_UP)
		*cy = ml->py + 1;
	else if (ml->choice == MENU_CHOICE_DOWN)
		*cy = ml->py + ml->visible;
	else
		*cy = ml->py + 1 +
		    menu_view_choice_row(ml, ml->choice);

	return (&ml->s);
}

/* Return parts of the input range which are not obstructed by the menu. */
void
menu_check_cb(__unused struct client *c, void *data, u_int px, u_int py,
    u_int nx, struct overlay_ranges *r)
{
	struct menu_data	*md = data;
	struct menu_view	*ml;
	u_int			 i;

	memset(r, 0, sizeof *r);
	r->px[0] = px;
	r->nx[0] = nx;
	for (i = 0; i <= md->depth; i++) {
		ml = &md->levels[i];
		server_client_overlay_range_subtract(ml->px, ml->py,
		    ml->menu->width + 4, ml->visible + 2, py, r);
	}
}

/* Return parts not obstructed by menu levels above the one being drawn. */
static void
menu_draw_check_cb(__unused struct client *c, void *data, u_int px, u_int py,
    u_int nx, struct overlay_ranges *r)
{
	struct menu_draw_data	*dd = data;
	struct menu_view	*ml;
	u_int			 i;

	memset(r, 0, sizeof *r);
	r->px[0] = px;
	r->nx[0] = nx;
	for (i = dd->depth + 1; i <= dd->md->depth; i++) {
		ml = &dd->md->levels[i];
		server_client_overlay_range_subtract(ml->px, ml->py,
		    ml->menu->width + 4, ml->visible + 2, py, r);
	}
}

void
menu_draw_cb(struct client *c, void *data,
    __unused struct screen_redraw_ctx *rctx)
{
	struct menu_data	*md = data;
	struct tty		*tty = &c->tty;
	struct screen		*s;
	struct menu_view	*ml;
	u_int			 depth, i;
	overlay_check_cb	 checkcb = c->overlay_check;
	void			*checkdata = c->overlay_data;
	struct menu_draw_data	 dd = { md, 0 };

	for (depth = 0; depth <= md->depth; depth++) {
		/* Allow this level to draw, but preserve every level above it. */
		dd.depth = depth;
		c->overlay_check = menu_draw_check_cb;
		c->overlay_data = &dd;
		ml = &md->levels[depth];
		s = &ml->s;
		menu_view_draw(ml, md->border_lines, &md->style,
		    &md->border_style, &md->selected_style);

		for (i = 0; i < screen_size_y(s); i++) {
			tty_draw_line(tty, s, 0, i, ml->menu->width + 4,
			    ml->px, ml->py + i, &grid_default_cell, NULL);
		}
	}

	c->overlay_check = checkcb;
	c->overlay_data = checkdata;
}

void
menu_free_cb(__unused struct client *c, void *data)
{
	struct menu_data	*md = data;
	u_int			 i;

	if (md->item != NULL)
		cmdq_continue(md->item);

	if (md->cb != NULL)
		md->cb(md->root, UINT_MAX, KEYC_NONE, md->data);

	for (i = 0; i <= md->depth; i++)
		menu_view_free(&md->levels[i]);
	menu_free(md->root);
	free(md);
}

int
menu_key_cb(struct client *c, void *data, struct key_event *event)
{
	struct menu_data		*md = data;
	struct menu_view		*ml = &md->levels[md->depth];
	struct menu			*menu = ml->menu;
	struct mouse_event		*m = &event->m;
	u_int				 i, amount;
	int				 count = menu->count, old = ml->choice;
	int				 level;
	const char			*name = NULL;
	const struct menu_item		*item;
	struct cmdq_state		*state;
	enum cmd_parse_status		 status;
	char				*error;

	if (KEYC_IS_MOUSE(event->key)) {
		if (md->flags & MENU_NOMOUSE) {
			if (MOUSE_BUTTONS(m->b) != MOUSE_BUTTON_1)
				return (1);
			return (0);
		}
		/* All exterior pointer events end hover, not the hierarchy. */
		if ((md->flags & MENU_CONTEXT) &&
		    menu_level_at(md, m->x, m->y) < 0) {
			for (i = 0; i <= md->depth; i++) {
				if (md->levels[i].choice != MENU_CHOICE_NONE) {
					md->levels[i].choice = MENU_CHOICE_NONE;
					menu_redraw(c);
				}
			}
		}
		if (menu_mouse_wheel(event)) {
			level = menu_level_at(md, m->x, m->y);
			if (level >= 0) {
				menu_pop(md, c, level);
				ml = &md->levels[md->depth];
				if (ml->choice < MENU_CHOICE_NONE)
					ml->choice = MENU_CHOICE_NONE;
				if (menu_scroll(ml,
				    menu_mouse_wheel_down(event) ? 1 : -1, 3))
					menu_redraw(c);
			}
			return (0);
		}
		if (md->flags & MENU_CONTEXT) {
			if (menu_mouse_motion(event)) {
				level = menu_level_at(md, m->x, m->y);
				if (level < 0)
					return (0);
				menu_pop(md, c, level);
				ml = &md->levels[md->depth];
				ml->choice = menu_mouse_choice(ml, m->x, m->y);
				if (ml->choice >= 0 &&
				    menu_item_enabled(ml->menu, ml->choice) &&
				    ml->menu->submenus[ml->choice] != NULL)
					menu_open_submenu(md, c);
				else
					menu_redraw(c);
				return (0);
			}
			if (menu_mouse_release(event) || menu_mouse_drag(event))
				return (0);
			level = menu_level_at(md, m->x, m->y);
			if (level < 0)
				return (1);
			menu_pop(md, c, level);
			ml = &md->levels[md->depth];
			menu = ml->menu;
			ml->choice = menu_mouse_choice(ml, m->x, m->y);
			if (ml->choice == MENU_CHOICE_NONE ||
			    MOUSE_BUTTONS(m->b) != MOUSE_BUTTON_1)
				return (0);
			goto chosen;
		}
		if (m->x < ml->px ||
		    m->x > ml->px + 4 + menu->width ||
		    m->y < ml->py + 1 ||
		    m->y > ml->py + ml->visible) {
			if (~md->flags & MENU_STAYOPEN) {
				if (MOUSE_RELEASE(m->b))
					return (1);
			} else {
				if (!MOUSE_RELEASE(m->b) &&
				    !MOUSE_WHEEL(m->b) &&
				    !MOUSE_DRAG(m->b))
					return (1);
			}
			if (ml->choice != MENU_CHOICE_NONE) {
				ml->choice = MENU_CHOICE_NONE;
				c->flags |= CLIENT_REDRAWOVERLAY;
			}
			return (0);
		}
		if (~md->flags & MENU_STAYOPEN) {
			if (MOUSE_RELEASE(m->b))
				goto chosen;
		} else {
			if (!MOUSE_WHEEL(m->b) && !MOUSE_DRAG(m->b))
				goto chosen;
		}
		ml->choice = menu_mouse_choice(ml, m->x, m->y);
		if (ml->choice != old)
			c->flags |= CLIENT_REDRAWOVERLAY;
		return (0);
	}
	for (i = 0; i < (u_int)count; i++) {
		name = menu->items[i].name;
		if (name == NULL || *name == '-')
			continue;
		if (event->key == menu->items[i].key) {
			ml->choice = i;
			goto chosen;
		}
	}
	switch (event->key & ~KEYC_MASK_FLAGS) {
	case KEYC_UP:
	case 'k':
		ml->choice = menu_move_choice(menu, ml->choice, -1, 1, 1);
		menu_view_ensure_visible(ml);
		menu_redraw(c);
		return (0);
	case KEYC_BSPACE:
		if (~md->flags & MENU_TAB)
			break;
		return (1);
	case '\011': /* Tab */
		if (~md->flags & MENU_TAB)
			break;
		if (ml->choice == count - 1)
			return (1);
		/* FALLTHROUGH */
	case KEYC_DOWN:
	case 'j':
		ml->choice = menu_move_choice(menu, ml->choice, 1, 1, 1);
		menu_view_ensure_visible(ml);
		menu_redraw(c);
		return (0);
	case KEYC_PPAGE:
	case 'b'|KEYC_CTRL:
		amount = (ml->visible > 1 ? ml->visible - 1 : 1);
		ml->choice = menu_move_choice(menu, ml->choice, -1, amount, 0);
		menu_view_ensure_visible(ml);
		menu_redraw(c);
		return (0);
	case KEYC_NPAGE:
		amount = (ml->visible > 1 ? ml->visible - 1 : 1);
		ml->choice = menu_move_choice(menu, ml->choice, 1, amount, 0);
		menu_view_ensure_visible(ml);
		menu_redraw(c);
		return (0);
	case 'g':
	case KEYC_HOME:
		ml->choice = menu_find_choice(menu, 0, 1);
		menu_view_ensure_visible(ml);
		menu_redraw(c);
		return (0);
	case 'G':
	case KEYC_END:
		ml->choice = menu_find_choice(menu, count - 1, -1);
		menu_view_ensure_visible(ml);
		menu_redraw(c);
		return (0);
	case 'f'|KEYC_CTRL:
		break;
	case KEYC_RIGHT:
		if (ml->choice == MENU_CHOICE_NONE)
			return (0);
		goto chosen;
	case KEYC_LEFT:
		if (md->depth != 0) {
			menu_pop(md, c, md->depth - 1);
			menu_redraw(c);
		}
		return (0);
	case '\r':
		goto chosen;
	case '\033': /* Escape */
	case 'c'|KEYC_CTRL:
	case 'g'|KEYC_CTRL:
	case 'q':
		return (1);
	}
	return (0);

chosen:
	if (ml->choice == MENU_CHOICE_NONE)
		return (1);
	if (ml->choice == MENU_CHOICE_UP ||
	    ml->choice == MENU_CHOICE_DOWN) {
		old = ml->choice;
		ml->choice = MENU_CHOICE_NONE;
		if (menu_scroll(ml,
		    old == MENU_CHOICE_DOWN ? 1 : -1, 1))
			menu_redraw(c);
		return (0);
	}
	item = &menu->items[ml->choice];
	if (item->name == NULL || *item->name == '-') {
		if (md->flags & MENU_STAYOPEN)
			return (0);
		return (1);
	}
	if (menu->submenus[ml->choice] != NULL) {
		menu_open_submenu(md, c);
		return (0);
	}
	if (md->cb != NULL) {
	    md->cb(ml->menu, ml->choice, item->key, md->data);
	    md->cb = NULL;
	    return (1);
	}

	if (md->item != NULL)
		event = cmdq_get_event(md->item);
	else
		event = NULL;
	state = cmdq_new_state(&md->fs, event, 0);

	if (item->command == NULL) {
		cmdq_free_state(state);
		return (1);
	}
	status = cmd_parse_and_append(item->command, NULL, c, state, &error);
	if (status == CMD_PARSE_ERROR) {
		cmdq_append(c, cmdq_get_error(error));
		free(error);
	}
	cmdq_free_state(state);

	return (1);
}

static void
menu_set_style(struct client *c, struct grid_cell *gc, const char *style,
    const char *option)
{
	struct style	 sytmp;
	struct options	*o = c->session->curw->window->options;

	memcpy(gc, &grid_default_cell, sizeof *gc);
	style_apply(gc, o, option, NULL);
	if (style != NULL) {
		style_set(&sytmp, &grid_default_cell);
		if (style_parse(&sytmp, gc, style) == 0) {
			gc->fg = sytmp.gc.fg;
			gc->bg = sytmp.gc.bg;
		}
	}
	gc->attr = 0;
}

struct menu_data *
menu_prepare(struct menu *menu, int flags, int starting_choice,
    struct cmdq_item *item, u_int px, u_int py, struct client *c,
    enum box_lines lines, const char *style, const char *selected_style,
    const char *border_style, struct cmd_find_state *fs, menu_choice_cb cb,
    void *data)
{
	struct menu_data	*md;
	struct menu_view	*ml;
	int			 choice = -1;
	struct options		*o = c->session->curw->window->options;

	if (c->tty.sx < menu->width + 4 || c->tty.sy < 3 || menu->count == 0)
		return (NULL);

	if (lines == BOX_LINES_DEFAULT)
		lines = options_get_number(o, "menu-border-lines");

	md = xcalloc(1, sizeof *md);
	ml = &md->levels[0];
	md->item = item;
	md->flags = flags;
	md->border_lines = lines;
	md->root = menu;
	md->depth = 0;

	menu_set_style(c, &md->style, style, "menu-style");
	menu_set_style(c, &md->selected_style, selected_style,
	    "menu-selected-style");
	menu_set_style(c, &md->border_style, border_style, "menu-border-style");

	if (fs != NULL)
		cmd_find_copy_state(&md->fs, fs);
	if ((md->flags & MENU_NOMOUSE) && starting_choice >= 0) {
		if (starting_choice >= (int)menu->count)
			starting_choice = menu->count - 1;
		choice = starting_choice;
	}
	if (menu_level_init(md, ml, menu, choice, px, py, c) != 0) {
		free(md);
		return (NULL);
	}

	md->cb = cb;
	md->data = data;
	return (md);
}

int
menu_display(struct menu *menu, int flags, int starting_choice,
    struct cmdq_item *item, u_int px, u_int py, struct client *c,
    enum box_lines lines, const char *style, const char *selected_style,
    const char *border_style, struct cmd_find_state *fs, menu_choice_cb cb,
    void *data)
{
	struct menu_data	*md;

	md = menu_prepare(menu, flags, starting_choice, item, px, py, c, lines,
	    style, selected_style, border_style, fs, cb, data);
	if (md == NULL)
		return (-1);
	server_client_set_overlay(c, 0, menu_check_cb, menu_mode_cb, menu_draw_cb,
	    menu_key_cb, menu_free_cb, NULL, md);
	return (0);
}
