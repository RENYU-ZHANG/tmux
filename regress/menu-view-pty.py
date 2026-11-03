#!/usr/bin/env python3
"""Private visual menu tests; requires the agent-tmux-capture Python package.

PYTHONPATH=/path/to/agent-sessions python3 regress/menu-view-pty.py BINARY EVIDENCE
Every operation uses a newly-created socket and empty private configuration.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import tempfile
import time

from agent_sessions.screen_capture import PrivateServer

parser = argparse.ArgumentParser()
parser.add_argument("binary", type=lambda p: Path(p).resolve(strict=True))
parser.add_argument("evidence", type=Path)
parser.add_argument("--term", default="xterm-256color")
args = parser.parse_args()
args.evidence.mkdir(parents=True, exist_ok=True)
os.environ.pop("TMUX", None)
os.environ.pop("TMUX_PANE", None)


def capture(client, name):
    time.sleep(.15)
    screen = client.snapshot()
    (args.evidence / (name + ".html")).write_text(screen.html())
    (args.evidence / (name + ".json")).write_text(json.dumps(list(screen.rows)))
    return screen.rows


with tempfile.TemporaryDirectory(prefix="tmux-menu-view-pty-") as directory:
    with PrivateServer(tmux=str(args.binary), root=Path(directory)) as server:
        server.environment["TERM"] = args.term
        server.environment["LC_ALL"] = "C.UTF-8"
        server.new_session("views", "cat", width=80, height=12)
        server.run("set", "-g", "status", "off")
        server.run("set", "-s", "escape-time", "10")
        entries = []
        for i in range(30):
            entries += [f"Item{i:02}", "", f"set -g @chosen {i}"]
        server.run("bind", "-n", "F11", "display-menu", "-x", "0", "-y", "0", *entries)
        nested = ["Branch", "", "--submenu", "Leaf", "", "set -g @chosen leaf",
                  "--end", "Last", "", "set -g @chosen last"]
        server.run("bind", "-n", "F10", "display-menu", "-L", "-N", "-M",
                   "-x", "0", "-y", "0", *nested)
        with server.client("views", width=80, height=12) as client:
            for height in (12, 4, 3):
                client.resize(80, height)
                client.send(b"\x1b[23~")
                rows = capture(client, f"long-{height}")
                assert any("Item00" in row for row in rows), rows
                client.key("end")
                rows = capture(client, f"last-{height}")
                assert any("Item29" in row for row in rows), rows
                client.key("home")
                rows = capture(client, f"first-{height}")
                assert any("Item00" in row for row in rows), rows
                client.key("end")
                client.key("enter")
                time.sleep(.1)
                assert server.run("show", "-gqv", "@chosen").stdout.strip() == "29"
            client.resize(80, 12)
            client.send(b"\x1b[21~")
            client.key("home")
            client.key("right")
            rows = capture(client, "submenu")
            assert any("Leaf" in row for row in rows), rows
            client.key("enter")
            time.sleep(.1)
            assert server.run("show", "-gqv", "@chosen").stdout.strip() == "leaf"
            # Redraw beneath an open submenu, including wide glyph boundaries.
            writer = "import os,time; [(os.write(1,('汉字0123456789'*10+'\\r\\n').encode()),time.sleep(.04)) for _ in range(25)];time.sleep(20)"
            server.run("respawn-pane", "-k", "exec python3 -c " + shlex.quote(writer))
            client.send(b"\x1b[21~")
            client.key("home")
            client.key("right")
            before = capture(client, "occlusion-before")
            protected = [row[:20] for row in before[:4]]
            for _ in range(3):
                server.run("refresh-client")
                after = capture(client, "occlusion-after")
                assert [row[:20] for row in after[:4]] == protected, (before, after)
            client.key("escape")
        (args.evidence / "result.json").write_text(json.dumps({
            "passed": True, "term": args.term,
            "isolation": server.isolation,
        }, indent=2))
print("private menus: long/one-row/two-row, nested selection and occlusion passed")
