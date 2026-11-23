#!/usr/bin/env python3
"""matterm --attach-tty against a real daemon, in real ptys (docs/tasks/attach-tty.md).

usage: attachtty_pty.py MATTERM MATTERM-DAEMON-E2E
Starts a daemon with one terminal (the e2e helper), runs matterm --attach-tty in a pty as a
phone's ssh session would, and checks: the list, taking the terminal over, typing, the size, the
taken-over state and taking it back, the leave key, quitting with the terminal restored, and the
message when there is no daemon. Exit code 0 when all pass."""
import fcntl, json, os, pty, re, select, shutil, signal, struct, subprocess, sys, tempfile, termios, time

matterm, helper_path = sys.argv[1], sys.argv[2]
failures = []


def check(ok, what):
    print(("pass  " if ok else "FAIL  ") + what, flush=True)
    if not ok:
        failures.append(what)


class Tty:
    def __init__(self, socket, rows=24, cols=80, extra=()):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(matterm, [matterm, "--attach-tty", "--daemon-socket", socket, *extra])
        self.resize(rows, cols, signal_child=False)
        self.raw = b""
        self.status = None

    def resize(self, rows, cols, signal_child=True):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try:
                    data = os.read(self.fd, 65536)
                except OSError:
                    return False
                if not data:
                    return False
                self.raw += data
        return True

    def text(self):
        # Without the escape sequences and without blanks: the drawing layer skips blanks with a
        # cursor move, so "a b" can arrive as "a", move, "b".
        plain = re.sub(rb"\x1b\[[0-9;?<]*[A-Za-z]", b"", self.raw).decode("utf8", "replace")
        return plain.replace(" ", "")

    def send(self, data):
        self.raw = b""
        os.write(self.fd, data)

    def wait_for(self, needle, seconds=10):
        needle = needle.replace(" ", "")
        end = time.time() + seconds
        while time.time() < end:
            if needle in self.text():
                return True
            if not self.pump(0.1):
                break
        return needle in self.text()

    def wait_exit(self, seconds=10):
        end = time.time() + seconds
        while time.time() < end:
            self.pump(0.05)
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.status = status
                return True
        return False

    def kill(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except OSError:
            pass


tmp = tempfile.mkdtemp(prefix="attachtty")
socket = tmp + "/d.sock"
helper = subprocess.Popen([helper_path, "--socket", socket, "--sessions", "1", "--send", "echo hello-from-daemon",
                           "--max-seconds", "120"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
ttys = []
try:
    json.loads(helper.stdout.readline())

    a = Tty(socket)
    ttys.append(a)
    check(a.wait_for("running terminals") and a.wait_for("terminal "), "the list shows the running terminal")
    check("\x1b[?1049h" in a.raw.decode("utf8", "replace"), "the alternate screen is used")
    a.send(b"\r")
    check(a.wait_for("hello-from-daemon"), "Enter takes the terminal over and the screen is rebuilt from the replay")
    check(a.wait_for("Ctrl+] leave"), "the first time, the leave key is announced")
    a.send(b"echo typed-here; stty size\r")
    check(a.wait_for("typed-here") and a.wait_for("24 80"), "typing reaches the terminal, which has the size of the screen")

    a.resize(30, 100)
    a.pump(0.5)
    a.send(b"stty size\r")
    check(a.wait_for("30 100"), "resizing the screen resizes the terminal")

    b = Tty(socket, 24, 80)
    ttys.append(b)
    check(b.wait_for("(in use)"), "the list of a second client says that the terminal is in use")
    b.send(b"\r")
    check(b.wait_for("hello-from-daemon"), "the second client takes it over")
    check(a.wait_for("Taken over by another client", 10), "the first client is told that it was taken over")
    a.send(b"zzz")
    b.pump(0.5)
    check("zzz" not in b.text(), "keys typed while taken over are not sent")
    b.send(b"echo from-b\r")
    check(b.wait_for("from-b"), "the second client types")
    a.send(b"\r")
    check(a.wait_for("from-b") and "Takenover" not in a.text()[-300:], "Enter takes it back, with what happened meanwhile")
    check(b.wait_for("Taken over by another client", 10), "and the second client is kicked in turn")

    b.send(b"\x1d")
    check(b.wait_for("Left terminal") or b.wait_for("keeps running"), "the leave key goes back to the list")
    b.send(b"q")
    check(b.wait_exit() and os.WIFEXITED(b.status) and os.WEXITSTATUS(b.status) == 0, "q ends the program with exit 0")
    check("\x1b[?1049l" in b.raw.decode("utf8", "replace"), "the terminal is restored (alternate screen left)")

    a.send(b"\x1d")
    check(a.wait_for("keeps running"), "the first client also leaves")
    a.send(b"q")
    check(a.wait_exit(), "and ends")

    # The terminal kept running: a new client sees what was typed.
    c = Tty(socket)
    ttys.append(c)
    c.wait_for("terminal ")
    c.send(b"\r")
    check(c.wait_for("from-b"), "after all clients left the terminal still runs, and a new client sees its screen")
    c.send(b"\x1d")
    c.wait_for("keeps running")
    c.send(b"\x03")
    check(c.wait_exit(), "Ctrl+C in the list ends the program")

    # SIGTERM (a closed ssh session, a kill) puts the terminal back too.
    e = Tty(socket)
    ttys.append(e)
    e.wait_for("terminal ")
    os.kill(e.pid, signal.SIGTERM)
    check(e.wait_exit() and "\x1b[?1049l" in e.raw.decode("utf8", "replace"), "SIGTERM restores the terminal and ends")

    # A dropped connection (the daemon gone): the program says so instead of hanging.
    helper.stdin.write("quit\n")
    helper.stdin.flush()
    helper.wait(timeout=20)
    d = Tty(socket)
    ttys.append(d)
    check(d.wait_for("daemon is not running"), "without a daemon the list says so")
    d.send(b"q")
    check(d.wait_exit(), "and q still ends the program")
finally:
    for t in ttys:
        t.kill()
    try:
        helper.kill()
    except OSError:
        pass
    shutil.rmtree(tmp, ignore_errors=True)

print("%d failed" % len(failures))
sys.exit(1 if failures else 0)
