#!/usr/bin/env python3
"""Private rendering regression for programmatic keyboard-warning dismissal.

PYTHONPATH=/path/to/agent-sessions python3 regress/kitty-warning-timer.py BINARY
The JSON report retains the warning, replacement and post-deadline screens.
"""
import json
import os
from pathlib import Path
import sys

from agent_sessions.screen_capture import PrivateServer, report

binary = str(Path(sys.argv[1]).resolve(strict=True))
os.environ.pop("TMUX", None)
os.environ.pop("TMUX_PANE", None)
replacement = "Keep this message until a key"
with PrivateServer(tmux=binary) as server:
    server.new_session("warning", "cat", width=80, height=12)
    server.run("set", "-g", "display-time", "1000")
    server.run("set", "-g", "status-left", "")
    server.run("set", "-g", "status-right", "")
    with server.client("warning", width=80, height=12) as client:
        server.run("set", "-s", "extended-keys-kitty", "on")
        assert client.wait_for("Ctrl+Tab unavailable; reattach to retry")
        captures = {"warning": client.snapshot()}
        server.run("set", "-s", "extended-keys-kitty", "off")
        server.run("display-message", "-d", "0", replacement)
        assert client.wait_for(replacement)
        captures["replacement"] = client.snapshot()
        client.wait_stable(quiet=1.2, timeout=3)
        captures["after-old-timer"] = client.snapshot()
        print(json.dumps(report(client.timeline, captures, client.regions(),
                                header={"tmux": binary, **server.isolation})))
        assert replacement in captures["after-old-timer"].rows[-1]
