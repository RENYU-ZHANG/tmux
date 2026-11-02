"""Private raw pane used by kitty-mru.py; no network or external transport."""
import os
import sys
import tty

tty.setraw(0)
with open(sys.argv[1], "wb", buffering=0) as output:
    os.write(1, b"\x1b[?2004hREADY")
    if len(sys.argv) > 2:
        os.write(1, bytes.fromhex(sys.argv[2]))
    while True:
        data = os.read(0, 4096)
        if not data:
            break
        output.write(data)
