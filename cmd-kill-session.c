/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
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
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Destroy session, detaching all clients attached to it and destroying any
 * windows linked only to this session.
 *
 * Note this deliberately has no alias to make it hard to hit by accident.
 */

static enum cmd_retval	cmd_kill_session_exec(struct cmd *, struct cmdq_item *);

/* Publish the complete target set before changing any Session. No hook or
 * subprocess can race this boundary: the server destroys the recorded set
 * in this same command invocation before returning success to the client.
 * The file is fsynced and atomically renamed; it survives server-process
 * loss. Whole-machine reboot recovery is deliberately a separate policy.
 */
static int
cmd_kill_session_record(struct cmdq_item *item, struct session *target, int all)
{
	static char		*closed;
	const char		*directory;
	struct session		*s;
	struct stat		 sb;
	struct evbuffer		*buffer;
	char			 filename[64], temporary[64], *next;
	size_t			 size, done;
	ssize_t			 count;
	int			 dfd = -1, fd = -1, saved;

	directory = options_get_string(global_options, "session-close-journal");
	if (*directory == '\0')
		return (0);
	buffer = evbuffer_new();
	if (buffer == NULL)
		fatalx("out of memory");
	evbuffer_add_printf(buffer, "tmux-session-close/1\n%s\n", server_instance);
	if (closed != NULL)
		evbuffer_add(buffer, closed, strlen(closed));
	RB_FOREACH(s, sessions, &sessions) {
		if ((all && s != target) || (!all && s == target))
			evbuffer_add_printf(buffer, "%u\n", s->id);
	}
	size = EVBUFFER_LENGTH(buffer);
	next = xmalloc(size + 1);
	memcpy(next, EVBUFFER_DATA(buffer), size);
	next[size] = '\0';
	snprintf(filename, sizeof filename, "%s.closed", server_instance);
	snprintf(temporary, sizeof temporary, ".%s.tmp", server_instance);
	if (*directory != '/') {
		errno = EINVAL;
		goto fail;
	}
	dfd = open(directory, O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
	if (dfd == -1)
		goto fail;
	if (fstat(dfd, &sb) != 0)
		goto fail;
	if (sb.st_uid != getuid() || (sb.st_mode & 077) != 0) {
		errno = EACCES;
		goto fail;
	}
	/* An interrupted unpublished write belongs only to this exact instance. */
	if (unlinkat(dfd, temporary, 0) != 0 && errno != ENOENT)
		goto fail;
	fd = openat(dfd, temporary, O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW, 0600);
	if (fd == -1)
		goto fail;
	for (done = 0; done < size; done += count) {
		count = write(fd, next + done, size - done);
		if (count == -1 && errno == EINTR) {
			count = 0;
			continue;
		}
		if (count <= 0) {
			if (count == 0)
				errno = EIO;
			goto fail;
		}
	}
	/* Check directory syncing before the publication boundary, so a failure
	 * leaves the previous record and every target unchanged. */
	if (fsync(fd) != 0 || fsync(dfd) != 0)
		goto fail;
	if (close(fd) != 0) {
		fd = -1;
		goto fail;
	}
	fd = -1;
	if (renameat(dfd, temporary, dfd, filename) != 0)
		goto fail;
	close(dfd);
	free(closed);
	/* Keep only the IDs after the two header lines. */
	closed = xstrdup(strchr(strchr(next, '\n') + 1, '\n') + 1);
	free(next);
	evbuffer_free(buffer);
	return (0);

fail:
	saved = errno;
	if (fd != -1)
		close(fd);
	if (dfd != -1) {
		unlinkat(dfd, temporary, 0);
		close(dfd);
	}
	free(next);
	evbuffer_free(buffer);
	cmdq_error(item, "session close journal: %s (no sessions closed)",
	    strerror(saved));
	return (-1);
}

const struct cmd_entry cmd_kill_session_entry = {
	.name = "kill-session",
	.alias = NULL,

	.args = { "aCt:", 0, 0, NULL },
	.usage = "[-aC] " CMD_TARGET_SESSION_USAGE,

	.target = { 't', CMD_FIND_SESSION, 0 },

	.flags = 0,
	.exec = cmd_kill_session_exec
};

static enum cmd_retval
cmd_kill_session_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct session		*s = target->s, *sloop, *stmp;
	struct winlink		*wl;

	if (!args_has(args, 'C') &&
	    cmd_kill_session_record(item, s, args_has(args, 'a')) != 0)
		return (CMD_RETURN_ERROR);

	if (args_has(args, 'C')) {
		RB_FOREACH(wl, winlinks, &s->windows) {
			wl->window->flags &= ~WINDOW_ALERTFLAGS;
			wl->flags &= ~WINLINK_ALERTFLAGS;
		}
		server_redraw_session(s);
	} else if (args_has(args, 'a')) {
		RB_FOREACH_SAFE(sloop, sessions, &sessions, stmp) {
			if (sloop != s) {
				server_destroy_session(sloop);
				session_destroy(sloop, 1, __func__);
			}
		}
	} else {
		server_destroy_session(s);
		session_destroy(s, 1, __func__);
	}
	return (CMD_RETURN_NORMAL);
}
