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

void
menu_view_ensure_visible(struct menu_view *ml)
{
	u_int	maximum, rows;

	if (ml->choice < 0)
		return;
	rows = ml->visible;
	if (ml->visible > 2 && ml->top != 0)
		rows--;
	if (ml->visible > 1 && ml->top + rows < ml->menu->count)
		rows--;
	if (rows != 0 && (u_int)ml->choice >= ml->top &&
	    (u_int)ml->choice < ml->top + rows)
		return;

	if (ml->menu->count <= ml->visible)
		maximum = 0;
	else if (ml->visible > 1)
		maximum = ml->menu->count - ml->visible + 1;
	else
		maximum = ml->menu->count - 1;
	if ((u_int)ml->choice < ml->top)
		ml->top = ml->choice;
	else if (ml->visible > 2 &&
	    (u_int)ml->choice > ml->visible - 3)
		ml->top = ml->choice - ml->visible + 3;
	else
		ml->top = ml->choice;
	if (ml->top > maximum)
		ml->top = maximum;
}

int
menu_view_has_up(struct menu_view *ml)
{
	/* With two rows, prefer the down indicator to hiding all content. */
	return (ml->visible > 2 && ml->top != 0);
}

int
menu_view_has_down(struct menu_view *ml)
{
	u_int	rows = ml->visible;

	if (ml->visible <= 1)
		return (0);
	if (menu_view_has_up(ml))
		rows--;
	return (ml->top + rows < ml->menu->count);
}

u_int
menu_view_item_rows(struct menu_view *ml)
{
	u_int	rows = ml->visible;

	if (menu_view_has_up(ml))
		rows--;
	if (menu_view_has_down(ml))
		rows--;
	return (rows);
}

u_int
menu_view_choice_row(struct menu_view *ml, int choice)
{
	return (menu_view_has_up(ml) + choice - ml->top);
}

int
menu_view_init(struct menu_view *mv, struct menu *menu, int choice,
    u_int px, u_int py, u_int sx, u_int sy)
{
	u_int visible;

	memset(mv, 0, sizeof *mv);
	if (sx < 4 || menu->width > sx - 4 || sy < 3 ||
	    menu->count == 0 || menu->count > INT_MAX)
		return (-1);
	visible = menu->count < sy - 2 ? menu->count : sy - 2;
	mv->menu = menu; /* Borrowed; the consumer owns the items. */
	mv->visible = visible;
	mv->px = px < sx - menu->width - 4 ? px : sx - menu->width - 4;
	mv->py = py < sy - visible - 2 ? py : sy - visible - 2;
	mv->choice = (choice >= 0 && (u_int)choice < menu->count) ?
	    choice : MENU_CHOICE_NONE;
	menu_view_ensure_visible(mv);
	screen_init(&mv->s, menu->width + 4, visible + 2, 0);
	mv->s.mode &= ~MODE_CURSOR;
	return (0);
}

void
menu_view_free(struct menu_view *mv)
{
	if (mv->menu != NULL)
		screen_free(&mv->s);
	memset(mv, 0, sizeof *mv);
}

void
menu_view_select(struct menu_view *mv, int choice)
{
	if (choice < 0 || (u_int)choice >= mv->menu->count)
		mv->choice = MENU_CHOICE_NONE;
	else
		mv->choice = choice;
	menu_view_ensure_visible(mv);
}

/* Move only the viewport; each consumer decides how selection follows it. */
int
menu_view_scroll(struct menu_view *mv, int direction, u_int amount)
{
	u_int old = mv->top, maximum;

	if (mv->menu->count <= mv->visible)
		return (0);
	maximum = mv->menu->count - mv->visible + (mv->visible > 1);
	if (direction > 0)
		mv->top += amount < maximum - mv->top ? amount : maximum - mv->top;
	else
		mv->top -= amount < mv->top ? amount : mv->top;
	return ((int)mv->top - (int)old);
}

static void
menu_view_write(struct screen_write_ctx *ctx, struct menu *menu, int choice,
    u_int top, u_int visible, int arrow_up, int arrow_down,
    enum box_lines lines,
    const struct grid_cell *menu_gc, const struct grid_cell *border_gc,
    const struct grid_cell *choice_gc)
{
	struct screen		*s = ctx->s;
	struct grid_cell	 default_gc;
	const struct grid_cell	*gc = &default_gc;
	u_int			 cx, cy, i, j, row, width = menu->width;
	const char		*name;

	cx = s->cx;
	cy = s->cy;

	memcpy(&default_gc, menu_gc, sizeof default_gc);

	screen_write_box(ctx, menu->width + 4, visible + 2, lines,
	    border_gc, menu->title);

	row = 0;
	if (arrow_up) {
		if (choice == MENU_CHOICE_UP)
			gc = choice_gc;
		screen_write_cursormove(ctx, cx + 1, cy + 1, 0);
		for (j = 0; j < width + 2; j++)
			screen_write_putc(ctx, gc, ' ');
		screen_write_cursormove(ctx, cx + 2, cy + 1, 0);
		format_draw(ctx, gc, width, "#[align=centre]▲", NULL, 0);
		gc = &default_gc;
		row++;
	}

	for (i = top; row < visible - arrow_down && i < menu->count;
	    row++, i++) {
		name = menu->items[i].name;
		if (name == NULL) {
			screen_write_cursormove(ctx, cx, cy + 1 + row, 0);
			screen_write_hline(ctx, width + 4, 1, 1, lines,
			    border_gc);
			continue;
		}

		if (choice >= 0 && i == (u_int)choice && *name != '-')
			gc = choice_gc;

		screen_write_cursormove(ctx, cx + 1, cy + 1 + row, 0);
		for (j = 0; j < width + 2; j++)
			screen_write_putc(ctx, gc, ' ');

		screen_write_cursormove(ctx, cx + 2, cy + 1 + row, 0);
		if (*name == '-') {
			default_gc.attr |= GRID_ATTR_DIM;
			format_draw(ctx, gc, width, name + 1, NULL, 0);
			default_gc.attr &= ~GRID_ATTR_DIM;
			continue;
		}

		format_draw(ctx, gc, width, name, NULL, 0);
		gc = &default_gc;
	}
	if (arrow_down) {
		if (choice == MENU_CHOICE_DOWN)
			gc = choice_gc;
		screen_write_cursormove(ctx, cx + 1, cy + visible, 0);
		for (j = 0; j < width + 2; j++)
			screen_write_putc(ctx, gc, ' ');
		screen_write_cursormove(ctx, cx + 2, cy + visible, 0);
		format_draw(ctx, gc, width, "#[align=centre]▼", NULL, 0);
	}

	screen_write_cursormove(ctx, cx, cy, 0);
}

/* Preserve the upstream flat-menu drawing entry point. */
void
screen_write_menu(struct screen_write_ctx *ctx, struct menu *menu, int choice,
    enum box_lines lines, const struct grid_cell *menu_gc,
    const struct grid_cell *border_gc, const struct grid_cell *choice_gc)
{
	menu_view_write(ctx, menu, choice, 0, menu->count, 0, 0, lines,
	    menu_gc, border_gc, choice_gc);
}

void
menu_view_draw(struct menu_view *mv, enum box_lines lines,
    const struct grid_cell *menu_gc, const struct grid_cell *border_gc,
    const struct grid_cell *choice_gc)
{
	struct screen_write_ctx ctx;

	screen_write_start(&ctx, &mv->s);
	screen_write_clearscreen(&ctx, 8);
	menu_view_write(&ctx, mv->menu, mv->choice, mv->top, mv->visible,
	    menu_view_has_up(mv), menu_view_has_down(mv), lines, menu_gc,
	    border_gc, choice_gc);
	screen_write_stop(&ctx);
}
