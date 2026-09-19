#!/bin/sh

# Running a popup's command can fail: a descriptor limit reached, a fork that
# does not happen. The popup must refuse to open. Taking the job's event
# without checking takes the server, and every session in it, instead.
#
# The limit is pinned to exactly what the server already holds, so the next
# descriptor it asks for cannot be granted and running the job fails. It is
# held down only across the popup attempt and given straight back, because a
# server left starved livelocks in getdtablecount(), which globs /proc/<pid>/fd
# and so needs a descriptor of its own. That livelock is a property of the
# starvation rather than of the popup, and leaving it in place wedges the test
# instead of testing anything. Anything above the exact count leaves room for
# the job to start, and then nothing is being exercised at all.
#
# Everything here is bounded and torn down with signals on the private socket
# rather than with tmux commands, because a starved server cannot answer them.

PATH=/bin:/usr/bin
TERM=screen

command -v prlimit >/dev/null 2>&1 || exit 0

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
# Socket names carry this run's pid. A crashed server leaves the socket
# behind and the next tmux command silently starts a fresh server on it, so a
# fixed name lets one run inherit another's wreckage; it would also put a
# concurrent run of this same test on the same sockets.
OUTER="$TEST_TMUX -Lpopup-jobfail-outer-$$"
INNER="$TEST_TMUX -Lpopup-jobfail-inner-$$"

inner_pid=""
outer_pid=""
cleanup() {
	# Give the descriptors back before anything tries to talk to it, then
	# end both servers by signal: a wedged server never answers kill-server
	# and the client would wait for ever. Only the exact processes this run
	# started are signalled, by recorded pid, so a concurrent run of the
	# same test on another socket is not killed along with them.
	[ -n "$inner_pid" ] && prlimit --pid "$inner_pid" --nofile=1024:4096 2>/dev/null
	[ -n "$inner_pid" ] && kill -9 "$inner_pid" 2>/dev/null
	[ -n "$outer_pid" ] && kill -9 "$outer_pid" 2>/dev/null
	# A crashed server leaves its socket, and any later command here starts
	# a fresh one on it with a pid we never recorded. The pattern carries
	# this run's pid, so it can only match servers this run created.
	pkill -9 -f "popup-jobfail-(inner|outer)-$$" 2>/dev/null
	return 0
}
trap 'cleanup' 0 1 15

t() { timeout 10 $INNER "$@"; }

$INNER -f/dev/null new-session -d -x80 -y24 cat || exit 1
t set-option -g mouse on || exit 1
t bind-key -n F12 "display-popup -A -x30 -y17 -w20 -h10 true" || exit 1
t bind-key -n F11 "display-popup -x30 -y17 -w20 -h10 true" || exit 1

$OUTER -f/dev/null new-session -d -x80 -y24 "$INNER attach-session" || exit 1
sleep 1

# Both pids are recorded before the limit comes down, because a starved server
# cannot answer for its own pid afterwards.
inner_pid=$(t display-message -p '#{pid}')
outer_pid=$(timeout 10 $OUTER display-message -p '#{pid}')
[ -n "$inner_pid" ] || exit 1
held=$(ls /proc/"$inner_pid"/fd 2>/dev/null | wc -l)
[ "$held" -gt 0 ] || exit 0
# Liveness is checked by signal while the limit is down: every tmux command is
# a new client connection needing descriptors of its own, which cannot be
# granted here. A crash leaves no process at all, which is what this looks for.
for key in F12 F11; do
	prlimit --pid "$inner_pid" --nofile="$held":"$held" 2>/dev/null || exit 0
	timeout 10 $OUTER send-keys "$key" || exit 1
	sleep 1
	prlimit --pid "$inner_pid" --nofile=1024:4096 2>/dev/null
	kill -0 "$inner_pid" 2>/dev/null || exit 1
	sleep 1
done

# And it must be usable afterwards, not merely alive.
t list-sessions >/dev/null 2>&1 || exit 1

# A command that could not be started says so, rather than failing silently
# the way a popup that does not fit does. The message is read off the drawn
# screen of the inner client, because a starved server cannot answer a new
# client asking it anything.
t set-option -g display-time 4000 || exit 1
prlimit --pid "$inner_pid" --nofile="$held":"$held" 2>/dev/null || exit 0
timeout 10 $OUTER send-keys F12 || exit 1
sleep 1
prlimit --pid "$inner_pid" --nofile=1024:4096 2>/dev/null
sleep 1
timeout 10 $OUTER capture-pane -p | grep -qi 'failed to start popup command' || exit 1
t new-window -d || exit 1
[ "$(t list-windows | wc -l)" -ge 2 ] || exit 1

exit 0
