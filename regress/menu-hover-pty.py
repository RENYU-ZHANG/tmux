#!/usr/bin/env python3
"""Clear exterior hover without closing native context menus.

PYTHONPATH=/path/to/agent-sessions python3 regress/menu-hover-pty.py BINARY EVIDENCE
Uses only a new private socket, empty configuration and inert menu commands.
"""
import argparse
import json
import os
from pathlib import Path
import tempfile

from agent_sessions.screen_capture import PrivateServer

parser = argparse.ArgumentParser()
parser.add_argument("binary", type=lambda p: Path(p).resolve(strict=True))
parser.add_argument("evidence", type=Path)
parser.add_argument("--term", default="xterm-256color")
parser.add_argument("--reopen", action="store_true")
args = parser.parse_args()
args.evidence.mkdir(parents=True, exist_ok=True)
os.environ.pop("TMUX", None)
os.environ.pop("TMUX_PANE", None)


def capture(client, name):
    screen = client.snapshot()
    (args.evidence / (name + ".html")).write_text(screen.html())
    (args.evidence / (name + ".json")).write_text(json.dumps({
        "rows": list(screen.rows),
        "highlighted": [(y, x) for y, x, cell in screen.cells()
                        if cell.attributes.bg == "1"],
    }))
    return screen


def position(screen, label):
    for y, row in enumerate(screen.rows):
        if label in row:
            return row.index(label) + 1, y + 1  # SGR coordinates are one-based.
    raise AssertionError((label, screen.rows))


def highlighted(screen):
    return any(cell.attributes.bg == "1" for _, _, cell in screen.cells())


def hover(client, label):
    client.mouse("move", "left", *position(client.snapshot(), label))


with tempfile.TemporaryDirectory(prefix="tmux-menu-hover-") as directory:
    with PrivateServer(tmux=str(args.binary), root=Path(directory)) as server:
        server.environment["TERM"] = args.term
        server.environment["LC_ALL"] = "C.UTF-8"
        server.new_session("hover", "cat", width=80, height=24)
        server.run("set", "-g", "status", "off")
        server.run("set", "-g", "mouse", "on")
        server.run("set", "-g", "menu-style", "default")
        server.run("set", "-g", "menu-border-style", "default")
        server.run("set", "-g", "menu-selected-style", "bg=red")
        server.run("set", "-s", "escape-time", "10")
        flat = ["First", "", "set -g @chosen first",
                "Second", "", "set -g @chosen second"]
        nested = ["Branch", "", "--submenu", "Deeper", "", "--submenu",
                  "Leaf", "", "set -g @chosen leaf", "--end", "--end",
                  "Last", "", "set -g @chosen last"]
        server.run("bind", "-n", "F11", "display-menu", "-N", "-M",
                   "-x", "15", "-y", "6", *flat)
        server.run("bind", "-n", "F10", "display-menu", "-L", "-N", "-M",
                   *(["-R"] if args.reopen else []),
                   "-x", "15", "-y", "6", *nested)
        long_menu = []
        for i in range(40):
            long_menu += [f"Item{i:02}", "", f"set -g @chosen {i}"]
        server.run("bind", "-n", "F9", "display-menu", "-N", "-M",
                   "-x", "15", "-y", "0", *long_menu)
        with server.client("hover", width=80, height=24) as client:
            client.key("f11")
            for side, point in (("left", (1, 8)), ("right", (80, 8)),
                                ("above", (20, 1)), ("below", (20, 24))):
                hover(client, "First")
                assert highlighted(capture(client, f"flat-{side}-inside"))
                client.mouse("move", "left", *point)
                screen = capture(client, f"flat-{side}-outside")
                assert not highlighted(screen), side
                position(screen, "First")  # Still open.
            for action, button in (("release", "left"), ("press", "wheel-up"),
                                   ("press", "wheel-down")):
                hover(client, "First")
                assert highlighted(client.snapshot())
                client.mouse(action, button, 80, 24)
                screen = capture(client, f"flat-outside-{action}-{button}")
                assert not highlighted(screen)
                position(screen, "First")
                assert not server.run("show", "-gqv", "@chosen").stdout.strip()
            # Enter with no choice retains its existing cancel semantics.
            client.key("enter")
            assert not server.run("show", "-gqv", "@chosen").stdout.strip()
            assert all("First" not in row for row in client.snapshot().rows)
            # Re-entry, direct click, and keyboard selection remain usable.
            client.key("f11")
            hover(client, "Second")
            assert highlighted(capture(client, "flat-reentry"))
            client.mouse("press", "left", *position(client.snapshot(), "First"))
            assert server.run("show", "-gqv", "@chosen").stdout.strip() == "first"
            client.mouse("release", "left", 1, 1)
            server.run("set", "-gu", "@chosen")
            client.key("f11")
            hover(client, "Second")
            client.mouse("move", "left", 80, 24)
            client.key("down")
            assert highlighted(capture(client, "keyboard-after-exit"))
            client.key("enter")
            assert server.run("show", "-gqv", "@chosen").stdout.strip() == "first"
            server.run("set", "-gu", "@chosen")
            # A whole nested hierarchy loses stale choices, not its boxes.
            client.key("f10")
            # Start a real held-button gesture on a submenu, then leave it.
            client.mouse("press", "left", *position(client.snapshot(), "Branch"))
            position(client.snapshot(), "Deeper")
            assert highlighted(capture(client, "nested-pressed"))
            for action in ("drag", "release"):
                client.mouse(action, "left", 80, 24)
                screen = capture(client, f"nested-outside-{action}")
                assert not highlighted(screen)
                for label in ("Branch", "Deeper"):
                    position(screen, label)
                assert not server.run("show", "-gqv", "@chosen").stdout.strip()
            hover(client, "Branch")
            hover(client, "Deeper")
            hover(client, "Leaf")
            assert highlighted(capture(client, "nested-inside"))
            client.mouse("move", "left", 80, 24)
            screen = capture(client, "nested-outside")
            assert not highlighted(screen)
            for label in ("Branch", "Deeper", "Leaf"):
                position(screen, label)
            client.key("right")
            assert not server.run("show", "-gqv", "@chosen").stdout.strip()
            hover(client, "Leaf")
            assert highlighted(capture(client, "nested-reentry"))
            client.mouse("press", "left", *position(client.snapshot(), "Leaf"))
            assert server.run("show", "-gqv", "@chosen").stdout.strip() == "leaf"
            client.mouse("release", "left", 1, 1)
            server.run("set", "-gu", "@chosen")
            # Viewport arrows also use negative choice values and must clear.
            client.key("f9")
            hover(client, "▼")
            assert highlighted(capture(client, "scroll-arrow-inside"))
            client.mouse("move", "left", 80, 12)
            screen = capture(client, "scroll-arrow-outside")
            assert not highlighted(screen)
            position(screen, "Item00")
            client.key("end")
            screen = capture(client, "scroll-keyboard-end")
            position(screen, "Item39")
            assert highlighted(screen)
            client.mouse("press", "left", 80, 12)
            assert not server.run("show", "-gqv", "@chosen").stdout.strip()
            assert all("Item39" not in row for row in client.snapshot().rows)
        (args.evidence / "result.json").write_text(json.dumps({
            "passed": True, "term": args.term, "isolation": server.isolation,
        }, indent=2))
print("menu hover: exterior clearing, retained hierarchy, re-entry and keyboard passed")
