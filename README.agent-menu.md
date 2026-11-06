# Native scrolling and nested menus

This branch is based on tmux 3.5a and extends `display-menu` without changing
the existing flat-menu argument format.

Menu levels now consume the shared flat viewport in
[Shared drawing and views](README.shared-views.md#shared-drawing-and-views).
The extraction preserves this deployed profile's menu, exterior-wheel and
animation policies. It is also verified as a separate menu feature over upstream
plus the shared foundations, without those sibling features.

## Features

- Menus taller than the client use a viewport instead of failing to open.
- A long viewport shows `▲` as its first row only when content is hidden
  above and `▼` as its last row only when content is hidden below. Pressing an
  arrow scrolls one row without dismissing the menu; choosing a normal leaf
  keeps the usual dismissal behavior.
- The mouse wheel scrolls the menu under the pointer by three rows.
- Page Up, Page Down, Home, End, Up, and Down keep the selection visible.
- `display-menu -L` accepts nested menu data and renders cascading submenus.
- Right or Enter opens a submenu, Left returns to its parent, and context-menu
  pointer motion opens the submenu under the pointer.
- Each parent has one visible child branch. Opening a sibling first closes and
  repaints the previous child and all of its descendants.
- Every visible level contributes to one cumulative occlusion mask. Pane,
  border, status, and forced full-client redraws continue outside the menu but
  cannot overwrite any menu cell; when menu levels overlap, each deeper level
  also clips the lower levels being redrawn. The same rule composes a native
  menu opened over a popup with the popup rectangle. A wide glyph that
  straddles an occlusion boundary follows the existing visible-space policy for
  the complete glyph; its continuation cell cannot leak into the overlay, and
  the logical cursor still advances by its complete width so later visible
  cells retain their source columns.
- `display-menu -N` selects on mouse press and dismisses on a press outside the
  complete menu hierarchy. Releases and drags neither select nor dismiss.
  Pointer events outside all visible menu boxes clear every row highlight,
  including motion, drag, release and wheel events. These events leave the
  hierarchy open; an outside press still dismisses it. Moving back onto a row
  or navigating with the keyboard selects again; Enter with no selected row
  retains its cancel behavior.
- While a popup is open, wheel events outside its rectangle follow normal tmux
  routing to the pane under the pointer without closing the popup. The usual
  `WheelUpPane` or `WheelDownPane` binding receives the original coordinates;
  all other outside events are unchanged.

Use `-M` when a menu opened by a non-mouse key binding should also accept mouse
input. Menus opened from a mouse binding accept mouse input automatically.

`regress/menu-hover-pty.py BINARY EVIDENCE` exercises pointer exit on all four
sides, exterior drag/release/wheel, nested-menu re-entry, scrolling indicators
and keyboard recovery on a private server. Run with `PYTHONPATH` pointing to
`agent-sessions`, in both
`--term xterm-256color` and `--term tmux-256color`. Captured HTML and cell
assertions use a high-contrast selected style to distinguish a cleared highlight
from an unchanged box. This deployed profile does not select optional `-R`.

## Nested argument format

With `-L`, a command value of `--submenu` starts a child menu and a name value
of `--end` closes it:

```sh
tmux display-menu -L -N \
  'Parent' p '--submenu' \
    'Child' c 'display-message child' \
    'Deeper' d '--submenu' \
      'Leaf' l 'display-message leaf' \
    '--end' \
  '--end' \
  'Root leaf' r 'display-message root'
```

`--submenu` and `--end` are reserved only when `-L` is present. Nesting is
limited to 16 visible levels. The original `name key command ...` grammar is
unchanged when `-L` is absent.
# Source-only baseline repair

## Source-only native animation prototype

Based on the deployed fbb52380 feature profile plus the tiny-menu repair, this
prototype adds `status-animation-interval`, a session millisecond option with
default 0. `set -g status-animation-interval 200` enables a demand-driven client
event only when the rendered status evaluates `#{status_animation_frame}`.
That token is a monotonic frame number, suitable for a modulo-four spinner.
All nested formats in a render use the client's single clock sample. The
source-only Session label publisher chooses it when the option is nonzero,
otherwise retaining its one-second `%s` expression; legacy two-second formats
keep their existing expression. No option or name is written per frame.

Animation has its own redraw reason. Coalesced ordinary/force/job-completion
reasons keep normal semantics. A pure tick reads existing matching format-job
output, never starts/kills jobs or changes cache state, and blanks a cache miss
or changed expanded command. Pure ticks omit terminal title/path and pane
border format evaluation. Existing mode bookkeeping, status styles/ranges,
overlay occlusion and tty backpressure remain in their owning paths. Prompts,
messages, status-off, idle/no token, control/suspended/detached clients and
destruction stop demand; normal visible rendering revives it.

This is an opt-in source experiment, not an installed feature. The extension
touches format jobs/callbacks, status event lifecycle, option metadata/change
handling, redraw reason flags and ancillary formatting. Those are custom fork
coupling points to inspect and replay on an upstream update. It adds no new
renderer, provider scan or external driver. Private Linux PTY cadence and CPU
measurements do not establish physical Windows paint timing. Non-modal popup
and close-journal commits remain excluded.

Private source checks are in my-env `tests/test-tmux-status-animation.py`:
explicit candidate, scenario, evidence directory and optional explicit baseline;
set `AGENT_SESSIONS_TEST_CHECKOUT` to the capture implementation. Use the same
static-library/OSC52 build recipe for candidate and repaired baseline. Retain
positive frame and job logs, lifecycle and overlay/input scenarios, and CPU/
output costs before drawing a feasibility conclusion.

Tiny menus with two content rows retain at least one item row. They omit the
up indicator at that height, retaining the down indicator while more items
remain. Keyboard navigation and wheel scrolling still reach earlier items;
selection, drawing and mouse hit-testing use the same row budget. One-row and
ordinary viewports keep their existing behavior. This repair is based on the
deployed fbb52380 feature set; it excludes non-modal popups and close journals.
