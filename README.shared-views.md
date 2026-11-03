# Shared drawing and views

## Input event metadata

Transport provenance belongs to `key_event.flags`, independently of the semantic
`key` and its modifiers. `KEY_EVENT_ENCODED` identifies framed input that does not
need legacy timing-based paste detection. Event producers zero-initialize the
structure; queues and saved events copy the complete event. Menus, prompts and
popups receive normal keys without depending on a particular terminal adapter.

## Drawing

The shared overlay foundation builds on upstream tmux 3.5a without local feature
patches. It owns bounded visible-range subtraction and correct glyph positioning
when an overlay clips text. Consumers supply their rectangles and stacking order;
the drawing layer does not know about submenus, session switching or input policy.

`OVERLAY_MAX_RANGES` is the shared span budget. Consumers must bound their rectangle
counts so that subtraction cannot exceed it. Existing popup/menu composition uses
the same helper rather than a second range implementation.

Run `python3 regress/overlay-ranges.py` for the sanitizer-enabled cell-oracle test.
It compiles the actual source functions without starting a server. Private visual
integration tests are additionally required for terminal drawing and clipping.

## Scrolling view

`menu-view.c` depends on the shared drawing foundation, not the nested-menu or
Ctrl+Tab features. A view borrows a flat menu's item model and owns its screen.
It bounds geometry, keeps selection visible and draws scroll indicators without
using all available rows for indicators. Free the view before its borrowed menu.
Failed initialization leaves an empty, safely freeable view.

The consumer owns enabled-item navigation, scrolling selection policy, overlay
callbacks, command execution and input interpretation. Register callbacks through
the upstream overlay API; do not replace another consumer's callback after setup.
Nested-menu stacking and MRU snapshot/release semantics are independent consumers.
The upstream `screen_write_menu` interface remains available for ordinary menus.

Run `python3 regress/menu-view.py` for geometry, visibility, overflow-safe scrolling
and lifetime checks under sanitizers, independent of either feature.
