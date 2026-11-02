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
#include <limits.h>
#include <string.h>
#include "tmux.h"

/* Outer-terminal adapter only; pane keyboard modes are unchanged. */
#define KITTY_QUERY 1
#define KITTY_VERIFY 2
#define KITTY_READY 3
#define KITTY_FAILED 4
#define KITTY_MAX_SEQUENCE 4096

static void
tty_kitty_restore(struct tty *tty)
{
	char buf[64];

	if (tty->flags & TTY_KITTY_SET) {
		xsnprintf(buf, sizeof buf, "\033[=%u;1u", tty->kitty_saved_flags);
		tty_raw(tty, buf);
		if (!options_get_number(global_options, "focus-events"))
			tty_raw(tty, "\033[?1004l");
		tty->flags &= ~TTY_KITTY_SET;
	}
	/* INPUT remains set only to recognize Kitty-only input still in flight. */
}

static void
tty_kitty_timeout(__unused int fd, __unused short events, void *data)
{
	struct tty *tty = data;

	tty_kitty_restore(tty);
	tty->kitty_state = KITTY_FAILED;
}

static void
tty_kitty_timer(struct tty *tty)
{
	struct timeval tv = { .tv_sec = 2 };

	if (event_initialized(&tty->kitty_timer))
		evtimer_del(&tty->kitty_timer);
	evtimer_set(&tty->kitty_timer, tty_kitty_timeout, tty);
	evtimer_add(&tty->kitty_timer, &tv);
}

void
tty_kitty_stop(struct tty *tty)
{
	if (event_initialized(&tty->kitty_timer))
		evtimer_del(&tty->kitty_timer);
	tty_kitty_restore(tty);
	tty->kitty_state = 0;
	tty->flags &= ~(TTY_KITTY_PASTE|TTY_KITTY_DISCARD);
}

void
tty_kitty_update(struct tty *tty)
{
	if (!options_get_number(global_options, "extended-keys-kitty")) {
		tty_kitty_stop(tty);
		return;
	}
	if (~tty->flags & TTY_STARTED || tty->kitty_state != 0 ||
	    tty->kitty_queries != 0)
		return;
	tty->kitty_state = KITTY_QUERY;
	tty->kitty_queries = 1;
	/* This query follows smcup; flags belong to that terminal screen. */
	tty_puts(tty, "\033[?u");
	tty_kitty_timer(tty);
}

int
tty_kitty_ready(struct tty *tty)
{
	return (tty->kitty_state == KITTY_READY &&
	    options_get_number(global_options, "extended-keys-kitty"));
}

/* A bounded decimal, with empty subparameters represented by zero. */
static int
tty_kitty_number(const char **p, const char *end, u_int *n)
{
	u_int digit;

	*n = 0;
	while (*p < end && **p >= '0' && **p <= '9') {
		digit = *(*p)++ - '0';
		if (*n > (UINT_MAX - digit) / 10)
			return (-1);
		*n = *n * 10 + digit;
	}
	return (0);
}

int
tty_kitty_query(struct tty *tty, const char *buf, size_t len, size_t *size)
{
	const char *p, *end;
	u_int flags;
	size_t i;

	if (tty->kitty_queries == 0 || strncmp(buf, "\033[?", len < 3 ? len : 3))
		return (-1);
	if (len < 3)
		return (1);
	for (i = 3; i < len && i < 16; i++) {
		if (buf[i] < '0' || buf[i] > '9')
			break;
	}
	if (i == len && i < 16)
		return (1);
	if (i == 3 || i == 16 || buf[i] != 'u')
		return (-1);
	p = buf + 3;
	end = buf + i;
	if (tty_kitty_number(&p, end, &flags) != 0)
		return (-1);
	*size = i + 1;
	tty->kitty_queries = 0;
	if (event_initialized(&tty->kitty_timer))
		evtimer_del(&tty->kitty_timer);
	if (tty->kitty_state == KITTY_QUERY) {
		tty->kitty_saved_flags = flags;
		tty->flags |= TTY_KITTY_SET|TTY_KITTY_INPUT;
		tty->kitty_state = KITTY_VERIFY;
		tty->kitty_queries = 1;
		/* No queued push/pop: stop cannot race an unsent mode change. */
		tty_raw(tty, "\033[=31;1u\033[?1004h\033[?u");
		tty_kitty_timer(tty);
	} else if (tty->kitty_state == KITTY_VERIFY) {
		if ((flags & TTY_KITTY_FLAGS) == TTY_KITTY_FLAGS)
			tty->kitty_state = KITTY_READY;
		else
			tty_kitty_timeout(-1, 0, tty);
	} else if (tty->kitty_state == 0)
		tty_kitty_update(tty); /* Drained a reply from before stop. */
	return (0);
}

static key_code
tty_kitty_codepoint(u_int n)
{
	struct utf8_data ud;
	utf8_char uc;

	if (n > 0x10ffff || (n >= 0xd800 && n <= 0xdfff))
		return (KEYC_UNKNOWN);
	if (n < 0x80)
		return (n);
	if (utf8_fromwc(n, &ud) != UTF8_DONE ||
	    utf8_from_data(&ud, &uc) != UTF8_DONE)
		return (KEYC_UNKNOWN);
	return (uc);
}

static key_code
tty_kitty_function(u_int n, char final)
{
	static const key_code tilde[] = {
		KEYC_UNKNOWN, KEYC_HOME, KEYC_IC, KEYC_DC, KEYC_END,
		KEYC_PPAGE, KEYC_NPAGE, KEYC_HOME, KEYC_END, KEYC_UNKNOWN,
		KEYC_UNKNOWN, KEYC_F1, KEYC_F2, KEYC_F3, KEYC_F4, KEYC_F5,
		KEYC_UNKNOWN, KEYC_F6, KEYC_F7, KEYC_F8, KEYC_F9, KEYC_F10,
		KEYC_UNKNOWN, KEYC_F11, KEYC_F12
	};
	static const key_code keypad[] = {
		KEYC_KP_ZERO, KEYC_KP_ONE, KEYC_KP_TWO, KEYC_KP_THREE,
		KEYC_KP_FOUR, KEYC_KP_FIVE, KEYC_KP_SIX, KEYC_KP_SEVEN,
		KEYC_KP_EIGHT, KEYC_KP_NINE, KEYC_KP_PERIOD, KEYC_KP_SLASH,
		KEYC_KP_STAR, KEYC_KP_MINUS, KEYC_KP_PLUS, KEYC_KP_ENTER,
		'=', ',', KEYC_LEFT, KEYC_RIGHT, KEYC_UP, KEYC_DOWN,
		KEYC_PPAGE, KEYC_NPAGE, KEYC_HOME, KEYC_END, KEYC_IC,
		KEYC_DC, KEYC_KP_FIVE
	};
	if (final == '~')
		return (n < nitems(tilde) ? tilde[n] : KEYC_UNKNOWN);
	if (final != 'u') {
		if (n != 1 && n != 0)
			return (KEYC_UNKNOWN);
		switch (final) {
		case 'A': return (KEYC_UP);
		case 'B': return (KEYC_DOWN);
		case 'C': return (KEYC_RIGHT);
		case 'D': return (KEYC_LEFT);
		case 'E': return (KEYC_KP_FIVE);
		case 'F': return (KEYC_END);
		case 'H': return (KEYC_HOME);
		case 'P': return (KEYC_F1);
		case 'Q': return (KEYC_F2);
		case 'S': return (KEYC_F4);
		}
		return (KEYC_UNKNOWN);
	}
	if (n == 127)
		return (KEYC_BSPACE);
	if (n >= 57364 && n <= 57375)
		return (KEYC_F1 + n - 57364);
	if (n >= 57376 && n <= 57387)
		return ((KEYC_F1 + n - 57376)|KEYC_SHIFT);
	if (n >= 57388 && n <= 57398)
		return ((KEYC_F1 + n - 57388)|KEYC_CTRL);
	if (n >= 57399 && n < 57399 + nitems(keypad))
		return (keypad[n - 57399]);
	if (n >= 57344 && n <= 63743)
		return (KEYC_UNKNOWN);
	return (tty_kitty_codepoint(n));
}

/* Distinguish in-flight Kitty syntax from the legacy encodings it shares. */
static int
tty_kitty_only(const char *buf, size_t size, char final)
{
	const char *p = buf + 2, *end = buf + size;
	u_int number;

	if (memchr(p, ':', end - p) != NULL)
		return (1); /* Alternate keys or event types. */
	if (tty_kitty_number(&p, end, &number) != 0)
		return (0);
	if (number >= 57344 && number <= 63743 &&
	    (final == '\0' || final == 'u'))
		return (1); /* Functional keys and standalone modifiers. */
	if (final != 'u')
		return (0);
	if (p == end)
		return (1); /* Bare codepoint; legacy CSI-u includes modifiers. */
	return (*p == ';' && memchr(p + 1, ';', end - p - 1) != NULL);
}

/* Return 0 consumed, 1 Kitty partial, 2 ambiguous partial, -1 not a Kitty key. */
int
tty_kitty_keys(struct tty *tty, const char *buf, size_t len, size_t *size,
    key_code *keys, u_int *count)
{
	const char *p, *end;
	size_t i;
	u_int number, shifted = 0, base = 0, mods = 1, event = 1;
	u_int text[TTY_KITTY_MAX_KEYS], ntext = 0, n;
	key_code key, modifiers = KEYC_KITTY;
	char final;

	*count = 0;
	/* A new escape sequence resynchronizes after a truncated old one. */
	if ((tty->flags & TTY_KITTY_DISCARD) && buf[0] == '\033')
		tty->flags &= ~TTY_KITTY_DISCARD;
	if (tty->flags & TTY_KITTY_DISCARD) {
		for (i = 0; i < len; i++) {
			if (buf[i] == '\033') {
				tty->flags &= ~TTY_KITTY_DISCARD;
				break;
			}
			if ((buf[i] < '0' || buf[i] > '9') &&
			    buf[i] != ':' && buf[i] != ';') {
				tty->flags &= ~TTY_KITTY_DISCARD;
				i++;
				break;
			}
		}
		*size = i;
		return (0);
	}
	if (~tty->flags & TTY_KITTY_INPUT || buf[0] != '\033')
		return (-1);
	if (len == 1)
		return (1);
	if (buf[1] != '[')
		return (-1);
	for (i = 2; i < len && i < KITTY_MAX_SEQUENCE; i++) {
		if ((buf[i] < '0' || buf[i] > '9') &&
		    buf[i] != ':' && buf[i] != ';')
			break;
	}
	/* An incomplete CSI prefix cannot yet distinguish Kitty from legacy. */
	if (i == len && i < KITTY_MAX_SEQUENCE) {
		if ((~tty->flags & TTY_KITTY_SET) &&
		    !tty_kitty_only(buf, i, '\0'))
			return (2); /* Buffer, but retain legacy fallback on timeout. */
		return (1);
	}
	/*
	 * Once the terminal mode is restored, ordinary CSI-u, modifyOtherKeys
	 * and function keys belong to their original decoders. Only distinctive
	 * Kitty syntax may still need adaptation (notably queued key releases).
	 */
	if (~tty->flags & TTY_KITTY_SET) {
		final = i < len && i < KITTY_MAX_SEQUENCE ? buf[i] : '\0';
		if (!tty_kitty_only(buf, i, final))
			return (-1);
	}
	if (i == KITTY_MAX_SEQUENCE) {
		*size = i;
		tty->flags |= TTY_KITTY_DISCARD;
		return (0); /* Bound memory and work even for unterminated input. */
	}
	final = buf[i];
	if (strchr("u~ABCDEFHPQS", final) == NULL)
		return (-1);
	/* Bracketed paste delimiters belong to the normal key tree. */
	if (final == '~' && i == 5 &&
	    (!memcmp(buf + 2, "200", 3) || !memcmp(buf + 2, "201", 3)))
		return (-1);
	*size = i + 1;
	p = buf + 2;
	end = buf + i;
	if (tty_kitty_number(&p, end, &number) != 0)
		return (0);
	if (p < end && *p == ':') {
		p++;
		if (tty_kitty_number(&p, end, &shifted) != 0)
			return (0);
		if (p < end && *p == ':') {
			p++;
			if (tty_kitty_number(&p, end, &base) != 0)
				return (0);
		}
	}
	if (p < end && *p == ';') {
		p++;
		if (tty_kitty_number(&p, end, &mods) != 0)
			return (0);
		if (mods == 0)
			mods = 1;
		if (p < end && *p == ':') {
			p++;
			if (tty_kitty_number(&p, end, &event) != 0)
				return (0);
		}
	}
	if (p < end && *p == ';') {
		do {
			p++;
			if (p == end || ntext == nitems(text) ||
			    tty_kitty_number(&p, end, &text[ntext]) != 0)
				return (0);
			ntext++;
		} while (p < end && *p == ':');
	}
	if (p != end || event < 1 || event > 3 || mods > 256)
		return (0);
	mods--;
	if (mods & 1)
		modifiers |= KEYC_SHIFT;
	if (mods & 2)
		modifiers |= KEYC_META|KEYC_IMPLIED_META;
	if (mods & 4)
		modifiers |= KEYC_CTRL;
	if (final == 'u' && (number == 57442 || number == 57448) && event == 3) {
		/* Gesture lifetime still matters with unrepresentable keys held. */
		keys[(*count)++] = KEYC_CTRL_RELEASE | modifiers;
		return (0);
	}
	/* Super/Hyper/Meta ordinary shortcuts have no legacy representation. */
	if (mods & 0x38)
		return (0);
	if (event == 3 || (final == 'u' && number >= 57441 && number <= 57454))
		return (0);
	/* Associated text is authoritative, including layout and Caps Lock. */
	if (ntext != 0 && !(mods & 6)) {
		for (n = 0; n < ntext; n++) {
			key = tty_kitty_codepoint(text[n]);
			if (key == KEYC_UNKNOWN || text[n] < 32 || text[n] == 127)
				return (0);
			keys[n] = key | KEYC_KITTY;
		}
		*count = ntext;
		return (0);
	}
	/*
	 * Control shortcuts retain the primary (or base-layout) key and Shift
	 * modifier. Shifted text must not change their binding or pane encoding.
	 * Without Control, Shift/Alt typing uses the shifted character as usual.
	 */
	if (final == 'u' && base != 0 && (mods & 6))
		number = base;
	if (final == 'u' && !(mods & 4) && (mods & 1) && shifted != 0)
		number = shifted;
	key = tty_kitty_function(number, final);
	if (key == KEYC_UNKNOWN)
		return (0);
	if (key >= KEYC_UP && key <= KEYC_RIGHT)
		key |= KEYC_CURSOR;
	if (key >= KEYC_KP_SLASH && key <= KEYC_KP_PERIOD)
		key |= KEYC_KEYPAD;
	if (key == '\011' && (modifiers & KEYC_SHIFT)) {
		key = KEYC_BTAB;
		modifiers &= ~KEYC_SHIFT;
	} else if (final == 'u' && number >= 32 && number < 57344 && !(mods & 4))
		modifiers &= ~KEYC_SHIFT;
	keys[(*count)++] = key | modifiers;
	return (0);
}
