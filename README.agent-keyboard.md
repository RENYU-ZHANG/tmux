# Ctrl-release session switcher

Candidate implementation; not installed or activated.

The opt-in server option `extended-keys-kitty` negotiates Kitty keyboard input
with each outer terminal. Ordinary clients and the default configuration retain
their existing behavior. This is an outer-terminal adapter, not pane-side Kitty
protocol support. Panes continue to receive their existing legacy or xterm
extended-key encoding.

The adapter requests flags 31: disambiguation, event types, alternate keys,
all keys, and associated text. Text and shifted-key information preserve keyboard
layouts, Caps Lock, punctuation, Unicode and composed text. Lock modifiers do
not change shortcut matching. Control shortcuts retain the unshifted primary
or base-layout key and their Shift modifier; shifted text does not replace the
shortcut's identity. Ordinary releases and standalone modifiers are
consumed; the final Ctrl release remains available to the session switcher.
Negotiation occurs before a gesture, never after its first Tab. The saved outer
keyboard state is restored on detach/suspend and when the option is disabled.
Restoration returns shared legacy encodings to their original input decoders;
distinctive Kitty input still in flight is adapted without leaking releases.
Unsupported terminals remain on their existing input path.
Focus reporting is enabled while the adapter owns the keyboard mode and restored
to tmux's configured focus policy when disabled. A terminal that does not confirm
all five flags is restored and cannot open the switcher. Legacy tmux cannot
represent Super, Hyper, Meta or unmapped functional keys; those events are consumed
without dispatching an unrelated ordinary shortcut.

`switch-client -m` opens a client-local session menu in most-recently-attached
order, with the current session first and the next session selected. `-mp`
starts in the reverse direction. Ctrl+Tab and Ctrl+Shift+Tab move through the
same fixed snapshot with wraparound. Navigation does not switch sessions or
update recency. Releasing the final held Ctrl key selects the highlighted session
once. Releasing only one of two held Ctrl keys does not select it.
Final Ctrl release still confirms when Super, Hyper or Meta remains held,
although ordinary shortcuts using those modifiers cannot be represented.

Escape, focus loss, resize, detach, an external session change, or disabling
the option cancels without selecting. Other input cancels and follows the
ordinary input path. Existing prompts and overlays are not replaced. A selected
session is resolved again by immutable ID at confirmation; deletion cannot
redirect selection to another session with the same name. Deleted entries may
remain in the snapshot until the gesture ends, but cannot be activated.

Example opt-in configuration (for a candidate private server only until native
activation is approved):

```tmux
set -s extended-keys-kitty on
bind -n C-Tab switch-client -m
bind -n C-BTab switch-client -mp
```

Verification covers real Terminal-to-SSH release delivery, negotiation and
restoration, normal/modified/Unicode keys, repeats, paste, mouse, fragmented and
malformed sequences, fixed-order/reverse/wrap/quick gestures, both Ctrl keys,
client isolation, cancellation, deleted sessions and legacy clients. The selectable feature depends only on upstream 3.5a plus the shared drawing and
scrolling-view foundations described in README.shared-views.md. Nested menus,
exterior-popup wheel routing and status animation are not prerequisites. The
separate deployed-profile build checks compatibility; inactive changes stay excluded.

The decoder design references the public Kitty protocol and tmux PR #5615
(head `3739dfeab3d20e9db4097a09ff5e63607b788df6`). Unlike that proposal, this
feature retains Ctrl-release events and requests associated text to adapt input
to the existing 3.5a key representation.

Run `python3 regress/kitty-mru.py /absolute/path/to/candidate-tmux` for the private
PTY regression suite. `KITTY_TEST_TERM=screen-256color` selects the second terminal
profile. Every test creates its own socket, empty configuration and raw receiver
panes; the synthetic outer terminal negotiates and checks restoration. The suite
does not replace physical Windows Terminal testing or the required native review.
