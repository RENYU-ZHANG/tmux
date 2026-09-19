#!/bin/sh

# A non-modal popup shares the client with the panes. A button press outside it
# selects the pane under the pointer through the ordinary binding, and typing
# then reaches that pane's program; a press inside takes the keyboard back; the
# popup stays open and does not hold the command queue. Keys that belong to
# tmux still belong to tmux. An ordinary popup keeps swallowing everything.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
OUTER="$TEST_TMUX -Lpopup-nonmodal-outer"
INNER="$TEST_TMUX -Lpopup-nonmodal-inner"

$OUTER kill-server 2>/dev/null
$INNER kill-server 2>/dev/null
trap '$OUTER kill-server 2>/dev/null; $INNER kill-server 2>/dev/null' 0 1 15

# Both panes run cat, so what a pane's program received is readable with
# capture-pane rather than inferred from a binding having run.
$INNER -f/dev/null new-session -d -x80 -y24 cat || exit 1
$INNER set-option -g mouse on || exit 1
$INNER split-window -h cat || exit 1
$INNER set-option -g @late no || exit 1
$INNER set-option -g @prefix-ran no || exit 1
$INNER set-option -g @prompt-ran no || exit 1
$INNER bind-key -n F12 \
	"display-popup -A -E -x30 -y17 -w20 -h10 cat" || exit 1
$INNER bind-key -n F11 \
	"display-popup -E -x30 -y17 -w20 -h10 cat" || exit 1
$INNER bind-key -n F10 \
	"display-popup -A -E -x30 -y17 -w20 -h10 'sleep 1'" || exit 1
$INNER bind-key -n F9 "if-shell -b 'true' \"set -g @late yes\"" || exit 1
$INNER bind-key -n F8 \
	"display-popup -AA -E -x30 -y17 -w20 -h10 cat" || exit 1
$INNER bind-key Z "set -g @prefix-ran yes" || exit 1

set -- $($INNER list-panes -F '#{pane_id}')
left=$1
right=$2
[ -n "$left" ] && [ -n "$right" ] || exit 1

$OUTER -f/dev/null new-session -d -x80 -y24 "$INNER attach-session" || exit 1
sleep 1

# The popup covers columns 30 to 49 and rows 7 to 16. Coordinates in SGR mouse
# input are one based.
click() {
	$OUTER send-keys -l "$(printf '\033')[<0;$1;$2M" || exit 1
	$OUTER send-keys -l "$(printf '\033')[<0;$1;$2m" || exit 1
	sleep 1
}

type_text() {
	$OUTER send-keys -l "$1" || exit 1
	sleep 1
}

pane_has() {
	$INNER capture-pane -p -t "$1" | grep -q "$2"
}

active_pane() {
	$INNER display-message -p '#{pane_id}'
}

$OUTER send-keys F12 || exit 1
sleep 1

# The border says which side has the keyboard, in words. That marker is the
# only reliable one, so it is read back from the drawn screen. This popup is
# twenty columns wide, so it is the short wording.
$OUTER capture-pane -p | grep -q 'keys here' || exit 1
$OUTER capture-pane -p | grep -q 'keys in pane' && exit 1

# A press outside runs the ordinary MouseDown1Pane binding, which selects that
# pane, and typing then reaches that pane's program.
click 71 6
$OUTER capture-pane -p | grep -q 'keys in pane' || exit 1
[ "$(active_pane)" = "$right" ] || exit 1
type_text alpha
pane_has "$right" alpha || exit 1
pane_has "$left" alpha && exit 1

# A press inside takes the keyboard back: the pane selection does not move,
# which also proves the popup is still open, and typing stops reaching panes.
click 36 11
$OUTER capture-pane -p | grep -q 'keys here' || exit 1
[ "$(active_pane)" = "$right" ] || exit 1
type_text beta
pane_has "$right" beta && exit 1
pane_has "$left" beta && exit 1

# A key bound behind the prefix is still a tmux key, and none of it is left in
# the pane.
click 11 6
[ "$(active_pane)" = "$left" ] || exit 1
$OUTER send-keys C-b || exit 1
sleep 1
$OUTER send-keys -l Z || exit 1
sleep 1
[ "$($INNER show-option -gv @prefix-ran)" = "yes" ] || exit 1
pane_has "$left" Z && exit 1

# A command appended to the client queue runs while the popup is open; holding
# the queue behind a waiting popup would leave this at no.
$OUTER send-keys F9 || exit 1
sleep 2
[ "$($INNER show-option -gv @late)" = "yes" ] || exit 1

# A prompt closes a non-modal popup and then behaves normally. This is the
# documented behaviour: status_prompt_set clears any overlay.
$OUTER send-keys C-b || exit 1
sleep 1
$OUTER send-keys -l ':' || exit 1
sleep 1
$OUTER send-keys -l 'set -g @prompt-ran yes' || exit 1
$OUTER send-keys Enter || exit 1
sleep 1
[ "$($INNER show-option -gv @prompt-ran)" = "yes" ] || exit 1
# The popup is gone, so a press inside its old rectangle reaches a pane again.
click 36 11
[ "$(active_pane)" = "$left" ] || exit 1

# A non-modal popup whose command exits while a pane has the keyboard closes
# cleanly and leaves the client usable.
$OUTER send-keys F10 || exit 1
sleep 1
click 71 6
[ "$(active_pane)" = "$right" ] || exit 1
sleep 3
click 36 11
[ "$(active_pane)" = "$left" ] || exit 1
type_text gamma
pane_has "$left" gamma || exit 1

# A popup opened unfocused does not take the keyboard: typing keeps reaching
# the pane that had it, with no click needed. This is what lets a surface the
# user did not ask for appear without swallowing what they are typing.
$INNER set-option -g @late no || exit 1
click 11 6
[ "$(active_pane)" = "$left" ] || exit 1
$OUTER send-keys F8 || exit 1
sleep 1
# Opened unfocused, so the very first frame already says where the keys go.
$OUTER capture-pane -p | grep -q 'keys in pane' || exit 1
type_text epsilon
pane_has "$left" epsilon || exit 1
# It is a real popup: a press inside takes the keyboard and holds it.
click 36 11
[ "$(active_pane)" = "$left" ] || exit 1
type_text zeta
pane_has "$left" zeta && exit 1
# And it is still non-modal, so a press outside gives the keyboard back.
click 71 6
[ "$(active_pane)" = "$right" ] || exit 1
type_text eta
pane_has "$right" eta || exit 1
$INNER kill-session 2>/dev/null
$INNER -f/dev/null new-session -d -x80 -y24 cat || exit 1
$INNER set-option -g mouse on || exit 1
$INNER split-window -h cat || exit 1
$INNER bind-key -n F11 \
	"display-popup -E -x30 -y17 -w20 -h10 cat" || exit 1
$INNER bind-key -n F9 "if-shell -b 'true' \"set -g @late yes\"" || exit 1
$INNER set-option -g @late no || exit 1
set -- $($INNER list-panes -F '#{pane_id}')
left=$1
right=$2
$OUTER kill-server 2>/dev/null
$OUTER -f/dev/null new-session -d -x80 -y24 "$INNER attach-session" || exit 1
sleep 1

# An ordinary popup is unchanged: it keeps every press and every key.
$OUTER send-keys F11 || exit 1
sleep 1
# Whichever pane is active, an ordinary popup must not let a press move it.
before=$(active_pane)
click 71 6
[ "$(active_pane)" = "$before" ] || exit 1
type_text delta
pane_has "$left" delta && exit 1
pane_has "$right" delta && exit 1
$OUTER send-keys F9 || exit 1
sleep 2
[ "$($INNER show-option -gv @late)" = "no" ] || exit 1

exit 0
