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
- `display-menu -N` selects on mouse press, ignores releases and drags, and
  dismisses on a press outside the complete menu hierarchy.
- While a popup is open, wheel events outside its rectangle follow normal tmux
  routing to the pane under the pointer without closing the popup. The usual
  `WheelUpPane` or `WheelDownPane` binding receives the original coordinates;
  all other outside events are unchanged.

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
