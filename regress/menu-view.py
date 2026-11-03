#!/usr/bin/env python3
"""Exercise actual viewport functions without either feature or a server."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / "menu-view.c").read_text()
names = ("ensure_visible", "has_up", "has_down", "item_rows", "choice_row",
         "init", "free", "select", "scroll")
functions = []
for name in names:
    match = re.search(r"(?:void|int|u_int)\nmenu_view_" + name +
                      r"\([^{}]*\n\{.*?^\}", source, re.M | re.S)
    if match is None:
        raise RuntimeError("missing viewport function: " + name)
    functions.append(match.group())
view = re.search(r"struct menu_view \{.*?^\};", (root / "tmux.h").read_text(), re.M | re.S).group()
program = r'''
#include <sys/types.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MENU_CHOICE_NONE -1
#define MODE_CURSOR 1
struct menu { u_int count, width; };
struct screen { int mode; void *allocation; };
VIEW
static void screen_init(struct screen *s, u_int x, u_int y, u_int history) {
    (void)history; assert(x >= 4 && y >= 3);
    s->allocation = malloc(1); assert(s->allocation); s->mode = MODE_CURSOR;
}
static void screen_free(struct screen *s) { free(s->allocation); }
FUNCTIONS
int main(void) {
    struct menu m = {0, 10}; struct menu_view v;
    unsigned int count, height, choice, rows, checks = 0;
    assert(menu_view_init(&v, &m, 0, 0, 0, 80, 24) == -1);
    menu_view_free(&v);
    m.count = 3;
    assert(menu_view_init(&v, &m, 0, 0, 0, 3, 24) == -1);
    assert(menu_view_init(&v, &m, 0, 0, 0, 80, 2) == -1);
    for (count = 1; count <= 200; count++) {
        m.count = count;
        for (height = 1; height <= 24; height++) {
            assert(menu_view_init(&v, &m, 0, UINT_MAX, UINT_MAX, 80, height+2) == 0);
            assert(v.px + m.width + 4 <= 80 && v.py + v.visible + 2 <= height+2);
            for (choice = 0; choice < count; choice++) {
                menu_view_scroll(&v, 1, UINT_MAX);
                menu_view_select(&v, choice);
                rows = menu_view_item_rows(&v);
                assert(rows >= 1);
                assert(v.top <= choice && choice < v.top + rows);
                assert(menu_view_choice_row(&v, choice) < v.visible);
                menu_view_scroll(&v, -1, UINT_MAX);
                menu_view_select(&v, choice);
                rows = menu_view_item_rows(&v);
                assert(v.top <= choice && choice < v.top + rows);
                checks++;
            }
            menu_view_select(&v, -1); assert(v.choice == -1);
            menu_view_select(&v, count); assert(v.choice == -1);
            menu_view_free(&v); menu_view_free(&v);
        }
    }
    printf("viewport geometry, visibility, scrolling and lifetime: %u checks passed\n", checks);
}
'''.replace("VIEW", view).replace("FUNCTIONS", "\n".join(functions))
with tempfile.TemporaryDirectory(prefix="tmux-view-unit-") as directory:
    test = Path(directory) / "view.c"
    test.write_text(program)
    binary = Path(directory) / "view"
    subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
