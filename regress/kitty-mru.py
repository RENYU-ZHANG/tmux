#!/usr/bin/env python3
"""End-to-end private PTY tests. Usage: python3 regress/kitty-mru.py ./tmux

All commands target a newly-created private socket with an empty config and
TMUX/TMUX_PANE removed. The terminal peer is synthetic; panes only log bytes.
No real tmux, provider, clipboard, credentials or network transport is used.
"""
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import shlex
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
RECEIVER = Path(__file__).with_name("kitty-receiver.py")


class Peer:
    def __init__(self, fixture, session="a", respond=True, flags=5, supported=31,
                 verify=True, width=100, height=28):
        self.fixture = fixture
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", height, width, 0, 0))
        self.name = os.ttyname(slave)
        self.process = subprocess.Popen(
            fixture.argv("attach-session", "-t", session),
            stdin=slave, stdout=slave, stderr=slave, env=fixture.env,
            start_new_session=True,
        )
        os.close(slave)
        self.flags = flags
        self.respond = respond
        self.supported = supported
        self.verify = verify
        self.buffer = b""
        self.output = b""
        self.changes = []
        self.queries = 0
        fixture.peers.append(self)

    def send(self, data):
        os.write(self.master, data)

    def pump(self, duration=0.03):
        until = time.monotonic() + duration
        while time.monotonic() < until:
            if not select.select([self.master], [], [], max(0, until-time.monotonic()))[0]:
                break
            try:
                data = os.read(self.master, 65536)
            except OSError:
                break
            self.output += data
            self.buffer += data
            # Retain a partial mode/query sequence across PTY reads.
            pattern = rb"\x1b\[(?:\?u|=([0-9]+);1u)"
            while (match := re.search(pattern, self.buffer)) is not None:
                self.buffer = self.buffer[match.end():]
                if match.group(1) is not None:
                    self.flags = int(match.group(1)) & self.supported
                    self.changes.append(self.flags)
                else:
                    self.queries += 1
                    if self.respond and (self.queries != 2 or self.verify):
                        self.send(f"\x1b[?{self.flags}u".encode())
            self.buffer = self.buffer[-64:]


class Fixture(unittest.TestCase):
    def argv(self, *args):
        return [BINARY, "-S", str(self.socket), "-f", "/dev/null", *args]

    def cmd(self, *args, check=True):
        result = subprocess.run(self.argv(*args), env=self.env, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        if check and result.returncode:
            self.fail(result.stderr.decode(errors="replace"))
        return result.stdout.decode().strip()

    def wait(self, predicate, timeout=2):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            for peer in self.peers:
                peer.pump(0.01)
            if predicate():
                return
        self.fail(f"timed out waiting for private state; a bytes={self.log()!r}")

    def log(self, session="a"):
        path = self.root / (session + ".bin")
        return path.read_bytes() if path.exists() else b""

    def add_session(self, name):
        command = shlex.join([sys.executable, str(RECEIVER), str(self.root / (name + ".bin"))])
        self.cmd("new-session", "-d", "-s", name, command)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tmux-kitty-regress-")
        self.root = Path(self.temp.name)
        self.socket = self.root / "socket"
        self.env = {k: v for k, v in os.environ.items() if k not in ("TMUX", "TMUX_PANE")}
        self.env.update(TERM=os.environ.get("KITTY_TEST_TERM", "xterm-256color"), LC_ALL="C.UTF-8",
                        XDG_CONFIG_HOME=str(self.root / "config"),
                        XDG_STATE_HOME=str(self.root / "state"))
        self.peers = []
        self.server_pid = None
        self.addCleanup(self.cleanup)
        self.add_session("a")
        self.server_pid = int(self.cmd("display-message", "-p", "#{pid}"))
        for name in ("b", "c", "d"):
            self.add_session(name)
        self.cmd("set", "-s", "extended-keys-kitty", "on")
        self.cmd("bind", "-n", "C-Tab", "switch-client", "-m")
        self.cmd("bind", "-n", "C-BTab", "switch-client", "-mp")
        self.peer = Peer(self)
        self.wait(lambda: self.peer.queries == 2 and self.peer.flags == 31)
        self.wait(lambda: all((self.root / (n + ".bin")).exists() for n in "abcd"))
        self.wait(lambda: b"READY" in self.peer.output)

    def cleanup(self):
        if self.server_pid is not None:
            proc = Path(f"/proc/{self.server_pid}/cmdline")
            if proc.exists():
                cmdline = proc.read_bytes()
                if b"tmux" not in cmdline:
                    raise AssertionError(f"unexpected recorded server PID: {cmdline!r}")
                self.cmd("kill-server", check=False)
        for peer in self.peers:
            try:
                peer.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                cmdline = Path(f"/proc/{peer.process.pid}/cmdline").read_bytes()
                if str(self.socket).encode() not in cmdline:
                    raise AssertionError("unexpected recorded client PID")
                peer.process.terminate()
                peer.process.wait(timeout=3)
            os.close(peer.master)
        self.temp.cleanup()

    def current(self, peer=None):
        tty = (peer or self.peer).name
        rows = self.cmd("list-clients", "-F", "#{client_tty} #{session_name}").splitlines()
        return dict(row.split(" ", 1) for row in rows)[tty]

    def switch(self, name, peer=None):
        self.cmd("switch-client", "-c", (peer or self.peer).name, "-t", name)

    def order(self):
        # Stable expected recency: a, b, c, d (current first).
        for name in ("d", "c", "b", "a"):
            self.switch(name)

    def send(self, data):
        self.peer.send(data)
        self.peer.pump()

    def release(self, right=False, other=False):
        self.send(f"\x1b[{57448 if right else 57442};{133 if other else 129}:3u".encode())

    def test_typing(self):
        self.send(b"\x1b[97u\x1b[97;1:2u\x1b[97;1:3u\x1b[13u")
        self.send(b"\x1b[97:65;2;65u\x1b[49:33;2;33u\x1b[97;65;65u")
        self.send(b"\x1b[233;;233u\x1b[0;;20320:22909u\x1b[99;5u\x1b[120;3u")
        expected = b"aa\rA!A" + "é你好".encode() + b"\x03\x1bx"
        self.wait(lambda: self.log() == expected)
        self.send(b"\x1b[57441;2u\x1b[57441;1:3u\x1b[97;9u\x1b[97;1:3u")
        self.release()
        self.assertEqual(self.log(), expected)

    def test_function_keys(self):
        self.send(b"\x1b[1;1:2A\x1b[1;1:3A\x1b[2~\x1b[3;1:2~\x1b[15~")
        self.wait(lambda: self.log() == b"\x1b[A\x1b[2~\x1b[3~\x1b[15~")

    def test_paste_literal_protocol(self):
        data = b"\x1b[97u\x1b[?5u\x1b[9;5u\x1b[57442;1:3u" + "你好".encode()
        self.send(b"\x1b[200~" + data + b"\x1b[201~")
        self.wait(lambda: self.log() == b"\x1b[200~" + data + b"\x1b[201~")
        self.assertEqual(self.current(), "a")

    def test_fixed_order_reverse_and_final_release(self):
        self.order()
        self.send(b"\x1b[9;133u" * 3 + b"\x1b[9;134u")
        self.assertEqual(self.current(), "a")
        self.release(other=True)
        self.assertEqual(self.current(), "a")
        self.release(right=True)
        self.wait(lambda: self.current() == "c")
        self.assertEqual(self.log(), b"")

    def test_quick_batch_and_repeat(self):
        self.order()
        self.send(b"\x1b[9;133u\x1b[57442;129:3u")
        self.wait(lambda: self.current() == "b")
        self.send(b"\x1b[9;133u\x1b[57442;129:3u")
        self.wait(lambda: self.current() == "a")
        self.send(b"\x1b[9;133u\x1b[9;133:2u\x1b[57442;129:3u")
        self.wait(lambda: self.current() == "c")

    def test_wrap_reverse_start(self):
        self.order()
        self.send(b"\x1b[9;134u\x1b[57442;129:3u")
        self.wait(lambda: self.current() == "d")
        self.order()
        self.send(b"\x1b[9;133u" * 5 + b"\x1b[57442;129:3u")
        self.wait(lambda: self.current() == "b")

    def test_long_list_two_content_rows(self):
        # Test steady small geometry, not an asynchronous resize cancellation.
        self.cmd("detach", "-t", self.peer.name)
        self.peer.process.wait(timeout=3)
        self.cmd("set", "-g", "status", "off")
        self.peer = Peer(self, width=42, height=4)
        self.wait(lambda: self.peer.queries == 2 and self.peer.flags == 31)
        names = ["a", "b", "c", "d"] + [f"extra{i:02}" for i in range(36)]
        for name in names[4:]:
            self.add_session(name)
        for name in reversed(names):
            self.switch(name)
        self.send(b"\x1b[9;133u" * 38)
        self.assertEqual(self.current(), "a")
        self.release()
        try:
            self.wait(lambda: self.current() == names[38])
        except AssertionError:
            self.fail(f"long-list expected {names[38]}, got {self.current()}; "
                      f"client={self.cmd('list-clients', '-F', '#{client_width}x#{client_height}')} "
                      f"tail={self.peer.output[-1500:]!r}")

    def test_one_session_view_lifetime(self):
        for name in ("b", "c", "d"):
            self.cmd("kill-session", "-t", name)
        for _ in range(10):
            self.send(b"\x1b[9;133u\x1b[9;134u\x1b[57442;129:3u")
            self.assertEqual(self.current(), "a")
        self.assertEqual(self.log(), b"")

    def test_cancel_and_forward(self):
        self.order()
        self.send(b"\x1b[9;133u\x1b[120u\x1b[57442;129:3u")
        self.wait(lambda: self.log() == b"x")
        self.assertEqual(self.current(), "a")
        self.send(b"\x1b[9;133u\x1b[27u\x1b[57442;129:3u")
        self.assertEqual(self.current(), "a")
        self.assertEqual(self.log(), b"x")

    def test_focus_resize_external_switch(self):
        self.order()
        self.send(b"\x1b[9;133u\x1b[O\x1b[57442;129:3u")
        self.assertEqual(self.current(), "a")
        self.send(b"\x1b[I\x1b[9;133u")
        fcntl.ioctl(self.peer.master, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 110, 0, 0))
        self.peer.process.send_signal(signal.SIGWINCH)
        self.peer.pump()
        self.release()
        self.assertEqual(self.current(), "a")
        self.send(b"\x1b[9;133u")
        self.switch("d")
        self.release()
        self.assertEqual(self.current(), "d")

    def test_deleted_target_not_reused(self):
        self.order()
        self.send(b"\x1b[9;133u")
        self.cmd("kill-session", "-t", "b")
        self.add_session("b")
        self.release()
        self.assertEqual(self.current(), "a")

    def test_clients_independent(self):
        peer2 = Peer(self, "d")
        self.wait(lambda: peer2.queries == 2)
        self.order()
        self.send(b"\x1b[9;133u")
        peer2.send(b"\x1b[57442;129:3u")
        peer2.pump()
        self.assertEqual(self.current(), "a")
        self.release()
        self.wait(lambda: self.current() == "b")
        self.assertEqual(self.current(peer2), "d")

    def test_fragmented_and_malformed(self):
        for data in (b"\x1b[", b"97:", b"65;2;", b"65u"):
            self.send(data)
        self.wait(lambda: self.log() == b"A")
        self.send(b"\x1b[99999999999999999999u\x1b[97;1:8u\x1b[97;;1114112u\x1b[122u")
        self.wait(lambda: self.log() == b"Az")
        self.send(b"\x1b[" + b"9" * 5000 + b"u\x1b[113u")
        self.wait(lambda: self.log() == b"Azq")

    def test_disable_restores_and_cancels(self):
        self.order()
        self.send(b"\x1b[9;133u")
        self.cmd("set", "-s", "extended-keys-kitty", "off")
        self.wait(lambda: self.peer.flags == 5)

        self.release()
        self.assertEqual(self.current(), "a")
        self.send(b"legacy")
        self.wait(lambda: self.log() == b"legacy")
        self.cmd("set", "-s", "extended-keys-kitty", "on")
        self.wait(lambda: self.peer.queries == 4 and self.peer.flags == 31)
        self.cmd("detach", "-t", self.peer.name)
        self.wait(lambda: self.peer.flags == 5)

    def test_truncated_sequence_resynchronizes(self):
        self.send(b"\x1b[97;1:")
        self.peer.pump(0.3)
        self.send(b"3u\x1b[98u")
        self.wait(lambda: self.log() == b"b")
        self.send(b"\x1b[97;1:")
        self.peer.pump(0.3)
        self.send(b"\x1b[99u")
        self.wait(lambda: self.log() == b"bc")

    def test_unsupported_peer(self):
        peer2 = Peer(self, "d", respond=False)
        self.wait(lambda: peer2.queries == 1)
        peer2.send(b"normal\r")
        self.wait(lambda: self.log("d") == b"normal\r")
        self.assertEqual(peer2.changes, [])

    def test_missing_flags_restore(self):
        peer2 = Peer(self, "d", supported=11, flags=1)
        self.wait(lambda: peer2.queries == 2 and peer2.flags == 1)
        self.assertEqual(peer2.changes, [11, 1])
        result = subprocess.run(self.argv("switch-client", "-c", peer2.name, "-m"),
                                env=self.env, capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"not ready", result.stderr)

    def test_verify_timeout_restore(self):
        peer2 = Peer(self, "d", verify=False)
        self.wait(lambda: peer2.queries == 2)
        self.wait(lambda: peer2.flags == 5, timeout=3)
        # A delayed response cannot reactivate the failed negotiation.
        peer2.send(b"\x1b[?31u")
        peer2.pump()
        self.assertEqual(peer2.flags, 5)
        self.assertEqual(peer2.queries, 2)

    def test_suspend_resume(self):
        self.order()
        self.send(b"\x1b[9;133u")
        self.cmd("suspend-client", "-t", self.peer.name)
        self.wait(lambda: self.peer.flags == 5)
        cmdline = Path(f"/proc/{self.peer.process.pid}/cmdline").read_bytes()
        self.assertIn(str(self.socket).encode(), cmdline)
        self.peer.process.send_signal(signal.SIGCONT)
        self.wait(lambda: self.peer.queries == 4 and self.peer.flags == 31)
        self.release()
        self.assertEqual(self.current(), "a")

    def test_existing_menu_prompt_and_popup(self):
        self.cmd("bind", "-n", "F5", "display-menu", "Item", "x", "set -g @selected yes")
        self.send(b"\x1b[15~")
        self.release()
        self.send(b"\x1b[120u")
        self.wait(lambda: self.cmd("show", "-gv", "@selected", check=False) == "yes")
        self.cmd("bind", "-n", "F6", "command-prompt", "set -g @answer '%%'")
        self.send(b"\x1b[17~")
        self.send(b"\x1b[97:65;2;65u\x1b[13u")
        self.wait(lambda: self.cmd("show", "-gv", "@answer", check=False) == "A")
        popup = shlex.join([sys.executable, str(RECEIVER), str(self.root / "popup.bin")])
        self.cmd("bind", "-n", "F7", "display-popup", "-w", "40", "-h", "10", popup)
        self.send(b"\x1b[18~")
        self.wait(lambda: (self.root / "popup.bin").exists())
        self.send(b"\x1b[9;133u")  # Existing popup owns the key.
        self.release()
        self.assertEqual(self.current(), "a")
        self.send(b"\x1b[27u")
        self.send(b"\x1b[122u")
        self.wait(lambda: self.log() == b"z")

    def test_mouse_cancels_and_routes(self):
        self.cmd("set", "-g", "mouse", "on")
        self.cmd("bind", "-n", "MouseDown1Pane", "set", "-g", "@mouse", "pressed")
        self.cmd("bind", "-n", "MouseDrag1Pane", "set", "-g", "@drag", "dragged")
        self.send(b"\x1b[9;133u\x1b[<0;5;5M\x1b[<32;6;5M\x1b[<0;6;5m")
        self.wait(lambda: self.cmd("show", "-gv", "@mouse", check=False) == "pressed")
        self.wait(lambda: self.cmd("show", "-gv", "@drag", check=False) == "dragged")
        self.release()
        self.assertEqual(self.current(), "a")

    def test_control_and_alt_shortcuts(self):
        self.cmd("bind", "-n", "C-a", "set", "-g", "@control", "yes")
        self.cmd("bind", "-n", "M-X", "set", "-g", "@altshift", "yes")
        self.send(b"\x1b[97;133u\x1b[120:88;132u")
        self.wait(lambda: self.cmd("show", "-gv", "@control", check=False) == "yes")
        self.wait(lambda: self.cmd("show", "-gv", "@altshift", check=False) == "yes")
        self.assertEqual(self.log(), b"")

    def test_ctrl_shift_shortcut_alternates(self):
        # Alternate text must not change an existing Control shortcut's key.
        for binding, sequences in (
            ("C-S-c", (b"\x1b[99;6u", b"\x1b[99:67;6u",
                       b"\x1b[1089:1057:99;6u", b"\x1b[1089::99;198u")),
            ("C-M-S-c", (b"\x1b[99;8u", b"\x1b[99:67;8u",
                         b"\x1b[1089:1057:99;8u")),
            ("C-S-=", (b"\x1b[61;6u", b"\x1b[61:43;6u")),
        ):
            self.cmd("bind", "-n", binding, "set", "-g", "@shortcut", "yes")
            for sequence in sequences:
                with self.subTest(binding=binding, sequence=sequence):
                    self.cmd("set", "-gu", "@shortcut")
                    self.send(sequence)
                    self.wait(lambda: self.cmd("show", "-gqv", "@shortcut") == "yes")
            self.cmd("unbind", "-n", binding)
        self.assertEqual(self.log(), b"")

    def assert_restored_legacy_input(self, peer, session, late=True):
        # This synthetic batch tests bindings, not tmux's typing-speed paste heuristic.
        self.cmd("set", "-g", "assume-paste-time", "0")
        self.cmd("bind", "-n", "F3", "set", "-g", "@restored", "yes")
        # In-flight Kitty releases are still consumed, not sent to the pane.
        if late:
            peer.send(b"\x1b[57442;1:3u\x1b[97;1:3u")
        # Both existing extended-key encodings and the ordinary key tree work.
        peer.send(b"\x1b[27;5;97~\x1b[97;5u\x1bORlegacy")
        self.wait(lambda: self.log(session) == b"\x01\x01legacy")
        self.wait(lambda: self.cmd("show", "-gqv", "@restored") == "yes")
        # A fragmented late release must not leak or eat following legacy data.
        if late:
            peer.send(b"\x1b[57448;1:")
            peer.pump(.03)
            peer.send(b"3u")
        peer.send(b"\x1b[27;5;99~")
        self.wait(lambda: self.log(session) == b"\x01\x01legacy\x03")
        if late:
            literal = b"\x1b[200~\x1b[57442;1:3u\x1b[27;5;97~\x1b[201~"
            peer.send(literal)
            self.wait(lambda: self.log(session) == b"\x01\x01legacy\x03" + literal)
            self.assert_restored_fragmented_input(peer, session)

    def assert_restored_fragmented_input(self, peer, session):
        # Early CSI prefixes are ambiguous until the rest of the key arrives.
        # The gap exceeds legacy escape-time, but stays within the CSI budget.
        self.cmd("set", "-s", "escape-time", "10")
        for sequence, expected in (
            (b"\x1b[57442;1:3u", b""),
            (b"\x1b[57448;1:3u", b""),
            (b"\x1b[97;1:3u", b""),
            (b"\x1b[27;5;97~", b"\x01"),
            (b"\x1b[97;5u", b"\x01"),
        ):
            for split in range(2, len(sequence)):
                with self.subTest(sequence=sequence, split=split):
                    prior = self.log(session)
                    peer.send(sequence[:split])
                    peer.pump(.04)
                    peer.send(sequence[split:] + b"Z")
                    self.wait(lambda: self.log(session) != prior)
                    self.assertEqual(self.log(session), prior + expected + b"Z")

    def test_legacy_input_without_negotiation(self):
        peer = Peer(self, "d", respond=False)
        self.wait(lambda: peer.queries == 1)
        self.assert_restored_legacy_input(peer, "d", late=False)

    def test_legacy_input_after_disable(self):
        self.cmd("set", "-s", "extended-keys-kitty", "off")
        self.wait(lambda: self.peer.flags == 5)
        self.assert_restored_legacy_input(self.peer, "a")

    def test_legacy_input_after_failed_negotiation(self):
        peer = Peer(self, "d", supported=11, flags=0)
        self.wait(lambda: peer.queries == 2 and peer.flags == 0)
        self.assert_restored_legacy_input(peer, "d")

    def test_legacy_input_after_verify_timeout(self):
        peer = Peer(self, "d", verify=False, flags=0)
        self.wait(lambda: peer.queries == 2)
        self.wait(lambda: peer.flags == 0, timeout=3)
        peer.send(b"\x1b[?31u")  # Late reply must not reactivate Kitty mode.
        self.assert_restored_legacy_input(peer, "d")
        self.assertEqual(peer.flags, 0)

    def test_restored_ambiguous_prefix_timeout(self):
        peer = Peer(self, "d", flags=0)
        self.wait(lambda: peer.queries == 2 and peer.flags == 31)
        self.cmd("set", "-s", "extended-keys-kitty", "off")
        self.wait(lambda: peer.flags == 0)
        self.cmd("set", "-s", "escape-time", "10")
        self.cmd("set", "-g", "assume-paste-time", "0")
        # Alt+[ followed by ordinary text may resemble an incomplete key.
        # After the CSI budget expires it remains text, including the next key.
        for prefix in (b"\x1b[", b"\x1b[1", b"\x1b[57", b"\x1b[97;5"):
            with self.subTest(prefix=prefix):
                prior = self.log("d")
                peer.send(prefix)
                peer.pump(.4)
                peer.send(b"z")
                self.wait(lambda: self.log("d") == prior + prefix + b"z")
        # Proven Kitty syntax still drains a timed-out late release and tail.
        prior = self.log("d")
        peer.send(b"\x1b[97;1:")
        peer.pump(.4)
        peer.send(b"3uQ")
        self.wait(lambda: self.log("d") == prior + b"Q")

    def test_final_ctrl_release_with_unmapped_modifiers(self):
        for modifier, press in ((8, 57444), (16, 57445), (32, 57446)):
            for locks in (0, 192):
                with self.subTest(modifier=modifier, locks=locks):
                    self.order()
                    self.send(b"\x1b[9;5u" +
                              f"\x1b[{press};{modifier + locks + 5}u".encode())
                    # Releasing one of two held Controls still cannot confirm.
                    self.send(f"\x1b[57442;{modifier + locks + 5}:3u".encode())
                    self.assertEqual(self.current(), "a")
                    self.send(f"\x1b[57448;{modifier + locks + 1}:3u".encode())
                    try:
                        self.wait(lambda: self.current() == "b")
                    except AssertionError:
                        self.fail(f"final Ctrl release selected {self.current()}; "
                                  f"flags={self.peer.flags}, queries={self.peer.queries}; "
                                  f"tail={self.peer.output[-1500:]!r}")
                    self.send(f"\x1b[{press};{locks + 1}:3u".encode())
        self.assertEqual(self.log(), b"")

    def test_pane_extended_keys(self):
        self.cmd("set", "-s", "extended-keys", "always")
        self.cmd("set", "-s", "extended-keys-format", "csi-u")
        command = shlex.join([sys.executable, str(RECEIVER), str(self.root / "e.bin"),
                              b"\x1b[>4;2m".hex()])
        self.cmd("new-session", "-d", "-s", "e", command)
        self.switch("e")
        self.wait(lambda: self.cmd("display", "-p", "-t", "e:", "#{pane_key_mode}") == "Ext 2")
        self.send(b"\x1b[97:65;6u\x1b[13;5u\x1b[120;3u")
        self.wait(lambda: self.log("e") == b"\x1b[97;6u\x1b[13;5u\x1b[120;3u")

    def test_stress_quick_gestures(self):
        self.order()
        self.send((b"\x1b[9;133u\x1b[57442;129:3u") * 101)
        self.wait(lambda: self.current() == "b")
        self.assertEqual(self.log(), b"")


if __name__ == "__main__":
    unittest.main()
