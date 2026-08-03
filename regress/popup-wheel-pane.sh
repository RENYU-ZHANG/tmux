#!/bin/sh

# Wheel events outside a popup must reach the pane under the pointer without
# closing the popup. Wheel events inside it must remain popup input.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
OUTER="$TEST_TMUX -Lpopup-wheel-outer"
INNER="$TEST_TMUX -Lpopup-wheel-inner"

$OUTER kill-server 2>/dev/null
$INNER kill-server 2>/dev/null
trap '$OUTER kill-server 2>/dev/null; $INNER kill-server 2>/dev/null' 0 1 15

$INNER -f/dev/null new-session -d -x80 -y24 || exit 1
$INNER set-option -g mouse on || exit 1
$INNER split-window -h || exit 1
$INNER bind-key -n WheelUpPane \
	"set-option -Fg @wheel-pane '#{mouse_pane}'" || exit 1
$INNER bind-key -n WheelDownPane \
	"set-option -Fg @wheel-pane '#{mouse_pane}'" || exit 1
$INNER bind-key -n F12 \
	"display-popup -E -x30 -y17 -w20 -h10 'sleep 30'" || exit 1

set -- $($INNER list-panes -F '#{pane_id}')
left=$1
right=$2
[ -n "$left" ] && [ -n "$right" ] || exit 1

$OUTER -f/dev/null new-session -d -x80 -y24 "$INNER attach-session" || exit 1
sleep 1
$OUTER send-keys F12 || exit 1
sleep 1

# Coordinates in SGR mouse input are one based. This is over the right pane
# and above the popup.
$OUTER send-keys -l "$(printf '\033')[<64;71;6M" || exit 1
sleep 1
[ "$($INNER show-option -gv @wheel-pane)" = "$right" ] || exit 1

# This is inside the popup but over the left pane. If the popup was closed by
# the first wheel event, or this event leaks through, the option will change.
$OUTER send-keys -l "$(printf '\033')[<64;36;11M" || exit 1
sleep 1
[ "$($INNER show-option -gv @wheel-pane)" = "$right" ] || exit 1

# Wheel down outside the popup resolves independently to the left pane.
$OUTER send-keys -l "$(printf '\033')[<65;11;6M" || exit 1
sleep 1
[ "$($INNER show-option -gv @wheel-pane)" = "$left" ] || exit 1

exit 0
