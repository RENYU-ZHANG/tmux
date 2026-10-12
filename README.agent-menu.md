# Native scrolling and nested menus

This branch is based on tmux 3.5a and extends `display-menu` without changing
the existing flat-menu argument format.

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
- `display-menu -N` selects on mouse press, ignores releases and drags, and
  dismisses on a press outside the complete menu hierarchy.
- `display-menu -N -R` opts a root into replacement by a fresh plain right
  press anywhere, including over a submenu. The entire old hierarchy closes
  before the same press follows normal bindings at its fresh coordinates.
  The caller regenerates its target and content. Releases, drags, modifiers
  and unopted menus retain their existing behavior. This root does not hold
  the client command queue, owns its opening event, and handles input
  queued during generation against the current hierarchy. Rapid replacement
  presses reset their own double/triple classification and reach the down
  binding. `-R` requires `-N`.
- While a popup is open, wheel events outside its rectangle follow normal tmux
  routing to the pane under the pointer without closing the popup. The usual
  `WheelUpPane` or `WheelDownPane` binding receives the original coordinates;
  all other outside events are unchanged.
- `display-popup -A` opens the popup non-modally. A mouse press outside it
  follows the same normal routing the wheel already does, so the click selects
  the pane under the pointer, and every later key goes there too; a press
  inside the popup takes the keyboard back. Only a press moves the keyboard,
  so a drag and its release stay with whichever side the press gave them to.
  While a pane has the keyboard the cursor and the terminal modes come from
  that pane, and that pane's cursor is hidden while the popup covers it. Which
  side has the keyboard is said in words at the *start* of the border title and
  shown by a dim or bright border, because a border style alone is faint on
  many terminals. The wording shortens with the popup — `keys come here`,
  `keys here`, `here` — so it survives a narrow popup, and any `-T` title
  follows it and is drawn literally, so a style opened there cannot colour the
  state and a malformed one cannot discard it. With `mouse` off the keyboard
  cannot move at all, so a popup opened unfocused can then only be closed with
  `display-popup -C` or by its command exiting under `-E`.
  Border drag to move and border drag to resize are unchanged and the popup
  rectangle still masks the pane redraws underneath it. A popup opened without
  `-A` behaves exactly as before.
- `display-popup -AA` opens it non-modally **and without the keyboard**, so a
  popup the user did not ask for appears without swallowing what they are
  typing. A press inside takes the keyboard as usual. This is what allows a
  surface that appears on its own, such as an explanation beside the pointer,
  to obey the rule that no key may change meaning.
- A non-modal popup is closed by its own menu's `Close`, by `display-popup -C`,
  by the command exiting under `-E`, or by `Escape` or `C-c` when it has the
  keyboard and `-E` was not given. Its menu is reached by a right press on its
  top or left border; a right press outside goes to the pane instead.
- A non-modal popup does not hold the command that opened it: that command
  completes rather than waiting, so anything queued afterwards runs while the
  popup is open, and the command does not receive the shell command's exit
  status.
- Opening the command prompt, a confirmation or a search closes a non-modal
  popup, because `status_prompt_set` clears any overlay before setting the
  prompt. The prompt then behaves normally and no keystroke is misdirected: a
  view is lost, never a key. A surface meant to be left open should be cheap
  to reopen in the same state.
- While a non-modal popup exists, `display-menu`, `display-panes` and a second
  `display-popup` silently do nothing, because an overlay is already present.
  A right press outside the popup therefore appears to do nothing: the pane
  under the pointer takes the keyboard and its context menu does not open.

Use `-M` when a menu opened by a non-mouse key binding should also accept mouse
input. Menus opened from a mouse binding accept mouse input automatically.

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
