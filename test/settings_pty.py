#!/usr/bin/env python3
"""Real tty settings entry points: no display, daemon or SSH required."""
import json
import fcntl
import struct
import os
import select
import signal
import subprocess
import sys
import tempfile
import termios
import time


def check(ok, message):
    if not ok:
        raise AssertionError(message)


class SettingsProcess:
    def __init__(self, binary, directory):
        self.master, self.slave = os.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        self.before = termios.tcgetattr(self.slave)
        self.output = bytearray()
        self.env = dict(os.environ, XDG_CONFIG_HOME=directory, HOME=directory,
                        TERM="xterm-256color", SDL_VIDEODRIVER="does-not-exist")
        # Remote options must never cause a connection: this edits the client.
        self.child = subprocess.Popen([binary, "--daemon-host", "must-not-connect.invalid", "--settings"],
                                      stdin=self.slave, stdout=self.slave, stderr=self.slave,
                                      env=self.env, start_new_session=True)
        self.path = os.path.join(directory, "matterm", "settings.json")

    def pump(self):
        if select.select([self.master], [], [], 0.05)[0]:
            try:
                self.output.extend(os.read(self.master, 65536))
            except OSError:
                pass

    def until(self, predicate):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            self.pump()
            if predicate():
                return
            if self.child.poll() is not None:
                break
        raise AssertionError("settings timed out: " + repr(bytes(self.output[-2000:])))

    def send(self, data):
        os.write(self.master, data)

    def theme_is(self, theme):
        try:
            with open(self.path, encoding="utf-8") as file:
                return json.load(file)["theme"] == theme
        except (OSError, ValueError, KeyError):
            return False

    def finish(self):
        self.until(lambda: self.child.poll() is not None)
        check(self.child.returncode == 0, "settings did not exit successfully")
        check(termios.tcgetattr(self.slave) == self.before, "raw mode leaked")
        check(b"\x1b[?1049l" in self.output, "alternate screen was not restored")
        check(b"\x1b[?1000h" in self.output and b"\x1b[?1006h" in self.output,
              "SGR click mouse was not enabled")
        check(b"\x1b[?1000l" in self.output and b"\x1b[?1006l" in self.output,
              "mouse reporting was not restored")

    def close(self):
        if self.child.poll() is None:
            self.child.kill()
            self.child.wait(timeout=5)
        os.close(self.master)
        os.close(self.slave)


for binary in sys.argv[1:]:
    with tempfile.TemporaryDirectory(prefix="matterm-settings-") as directory:
        proc = SettingsProcess(binary, directory)
        try:
            proc.until(lambda: b"Settings" in proc.output)
            # 80x24 layout: first control at (26,6), Apply at (26,18), zero-based.
            # Release/motion/outside clicks must not apply pending blue.
            proc.send(b"\x1b[<0;27;7M\x1b[B")
            time.sleep(0.2)
            proc.send(b"\x1b[<0;27;19m\x1b[<32;27;19M\x1b[<0;75;19M")
            for _ in range(5):
                proc.pump()
            check(not proc.theme_is("blue"), "non-action mouse event saved pending edit")
            proc.send(b"\x1b[<0;27;19M")
            proc.until(lambda: proc.theme_is("blue"))
            # Open blue -> pending green, cancel, reopen blue -> apply grey.
            # Two ESC bytes guarantee an Escape event even if writes coalesce;
            # Alt+Enter is still Enter. Ignoring cancellation would save green.
            proc.send(b"\r\x1b[B\x1b\x1b\r\x1b[A\r")
            proc.until(lambda: proc.theme_is("grey"))
            proc.send(b"\x11")
            proc.finish()
            check(proc.theme_is("grey"), "Escape did not cancel only the unfinished edit")
            with open(proc.path, encoding="utf-8") as file:
                settings = json.load(file)
            check(settings["computerNamePlacement"] == "center", "defaults not saved")
        finally:
            proc.close()
        terminated = SettingsProcess(binary, directory)
        try:
            terminated.until(lambda: b"Settings" in terminated.output)
            terminated.child.send_signal(signal.SIGTERM)
            terminated.finish()
        finally:
            terminated.close()
        no_tty = subprocess.run([binary, "--settings"], input=b"", capture_output=True,
                                env=proc.env, timeout=5)
        check(no_tty.returncode != 0 and b"terminal" in no_tty.stderr,
              "non-tty settings should fail clearly without SDL/SSH initialization")
        for incompatible in (["--new"], ["--name", "test"]):
            result = subprocess.run([binary, "--settings"] + incompatible, input=b"",
                                    capture_output=True, env=proc.env, timeout=5)
            check(b"mode of its own" in result.stderr, "Settings bypassed command validation: " + repr(incompatible))
        if os.path.basename(binary) == "matterm":
            result = subprocess.run([binary, "--settings", "--pty-layout-smoke-test"], input=b"",
                                    capture_output=True, env=proc.env, timeout=5)
            check(b"mode of its own" in result.stderr, "Settings bypassed native smoke-mode validation")
    print("pass settings tty save/cancel/restore/local-only:", binary)
