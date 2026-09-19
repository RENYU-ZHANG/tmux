#!/bin/sh

# A popup command can exit while the popup stays open, which leaves no job.
# The popup's own menu still offers paste and the two make-pane items; acting
# on them must do nothing rather than dereference the job that has gone.
# A crash here takes the whole server, and every session in it, with it.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
# Per-run socket names: a crashed server leaves its socket behind and the next
# command starts a fresh server on it, and a fixed name would also collide
# with a concurrent run of this same test.
OUTER="$TEST_TMUX -Lpopup-nojob-outer-$$"
INNER="$TEST_TMUX -Lpopup-nojob-inner-$$"

$OUTER kill-server 2>/dev/null
$INNER kill-server 2>/dev/null
trap '$OUTER kill-server 2>/dev/null; $INNER kill-server 2>/dev/null' 0 1 15

$INNER -f/dev/null new-session -d -x80 -y24 cat || exit 1
$INNER set-option -g mouse on || exit 1
$INNER set-buffer 'something to paste' || exit 1
# No -E, so the popup stays open after true exits and pd->job becomes NULL.
$INNER bind-key -n F12 \
	"display-popup -A -x30 -y17 -w20 -h10 true" || exit 1
$INNER bind-key -n F11 \
	"display-popup -x30 -y17 -w20 -h10 true" || exit 1

$OUTER -f/dev/null new-session -d -x80 -y24 "$INNER attach-session" || exit 1
sleep 1

# The popup covers columns 30 to 49 and rows 7 to 16, so its top border is row
# 8 in the one-based coordinates SGR mouse input uses.
menu_then() {
	$OUTER send-keys -l "$(printf '\033')[<2;36;8M" || exit 1
	sleep 1
	$OUTER send-keys -l "$1" || exit 1
	sleep 1
}

for open_key in F12 F11; do
	for item in p h v; do
		$OUTER send-keys "$open_key" || exit 1
		sleep 1
		menu_then "$item"
		# The only thing that matters: the server is still there.
		$INNER list-sessions >/dev/null 2>&1 || exit 1
		$INNER display-popup -C 2>/dev/null
		sleep 1
	done
done

# And the server is still usable afterwards, not merely alive.
$INNER new-window -d || exit 1
[ "$($INNER list-windows | wc -l)" -ge 2 ] || exit 1

exit 0
