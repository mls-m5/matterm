#!/usr/bin/env python3
"""matmux in passthrough mode (docs/tasks/matmux-passthrough.md) against a real daemon, in real ptys.

usage: matmux_pty.py MATMUX MATTERM-DAEMON-E2E
Checks what the host terminal is sent: the program's own bytes, the alternate screen only when the program
uses it, the way back (restore) on every exit, takeover and ending lines, the exit code of --new."""
import json, os, pty, re, select, shutil, signal, struct, subprocess, sys, tempfile, termios, time, fcntl

matmux, helper_path = sys.argv[1], sys.argv[2]
failures = []


def check(ok, what):
    print(("pass  " if ok else "FAIL  ") + what, flush=True)
    if not ok:
        failures.append(what)


class Tty:
    def __init__(self, socket, args=(), rows=24, cols=80, cwd=None, env=None):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            if cwd:
                os.chdir(cwd)
            if env:
                os.environ.update(env)
            os.execv(matmux, [matmux, "--daemon-socket", socket, *args])
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.raw = b""
        self.status = None
        self.start = time.time()

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
        # Without escape sequences and blanks: the drawing skips blanks with a cursor move.
        plain = re.sub(rb"\x1b\[[0-9;?<]*[A-Za-z]", b"", self.raw).decode("utf8", "replace")
        return plain.replace(" ", "")

    def send(self, data):
        os.write(self.fd, data)

    def wait_for(self, needle, seconds=10):
        needle = needle.replace(" ", "")
        end = time.time() + seconds
        while time.time() < end:
            if needle in self.text():
                return True
            if not self.pump(0.05):
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

    def code(self):
        return os.WEXITSTATUS(self.status) if self.status is not None and os.WIFEXITED(self.status) else None

    def kill(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except OSError:
            pass


def count_sessions():
    helper.stdin.write("list\n")
    helper.stdin.flush()
    return len(json.loads(helper.stdout.readline())["sessions"])


def open_marked(client, marker, seconds=2.0):
    """In the list: open terminals one by one until the screen shows `marker` (the list has no search)."""
    for digit in b"123456789":
        client.raw = b""
        client.send(bytes([digit]))
        client.pump(0.1)
        client.send(b"\r")
        if client.wait_for(marker, seconds):
            return True
        client.raw = b""
        client.send(b"\x02l")
        if not client.wait_for("running terminals", 3):
            return False
        client.pump(0.2)
    return False


def find_row(client, text, key=b"x", seconds=0.8):
    """In the list: press a digit and `key` until what the list says contains `text` (no search in the list);
    leaves the question open when found."""
    for digit in b"123456789":
        client.raw = b""
        client.send(bytes([digit]))
        client.pump(0.1)
        client.send(key)
        if client.wait_for(text, seconds):
            return True
        client.send(b"n")  # Keeps whatever the question was about.
        client.pump(0.2)
    return False


tmp = tempfile.mkdtemp(prefix="matmuxpty")
socket = tmp + "/d.sock"
helper = subprocess.Popen([helper_path, "--socket", socket, "--sessions", "1", "--send", "echo hello-from-daemon",
                           "--max-seconds", "120"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
ttys = []
RESTORE_CURSOR = b"\x1b[?25h"
try:
    started = json.loads(helper.stdout.readline())
    first_id = started["sessions"][0]["id"]

    # --new -- COMMAND: a long history, then a program that goes on. Time to the first screen.
    a = Tty(socket, ("--new", "--", "sh", "-c", "seq 1 20000; echo history-end; sleep 60"))
    ttys.append(a)
    check(a.wait_for("history-end", 20), "a new terminal with a long history is shown")
    print("      time to the last line of 20000: %.2f s" % (time.time() - a.start))
    check(b"\x1b[?1049h" not in a.raw, "the host terminal is not put in the alternate screen (its scrollback stays)")
    a.send(b"\x02d")
    check(a.wait_exit() and a.code() == 0, "Ctrl+B d leaves with exit 0")
    check("keepsrunning" in a.text(), "and says that the terminal keeps running")
    check(RESTORE_CURSOR in a.raw, "and puts the host terminal back (cursor shown)")

    # Attach again from the list: the screen is there from the replay; typing passes through.
    b = Tty(socket)
    ttys.append(b)
    b.wait_for("running terminals")
    b.send(b"2")  # The first terminal row after "New terminal": the e2e session or the sleep one; take the last.
    b.pump(0.3)
    b.send(b"q")
    b.wait_exit()

    # The e2e session: history and typing.
    c = Tty(socket, ("--attach",))
    ttys.append(c)
    c.wait_for("running terminals")
    c.pump(0.3)
    list_bytes = c.raw
    check(open_marked(c, "hello-from-daemon"), "a terminal chosen in the list is shown in passthrough")
    check(b"\x1b[?1049h" in list_bytes[:300] and b"\x1b[?1049l" in c.raw + list_bytes, "the list uses the alternate screen and leaves it before the terminal")
    c.raw = b""
    c.send(b"\x02l")
    check(c.wait_for("running terminals"), "Ctrl+B l is the list again")
    c.pump(0.3)
    c.send(b"q")
    check(c.wait_exit() and c.code() == 0, "q ends")

    # A program in the alternate screen: the host enters it; leaving puts the host back.
    d = Tty(socket, ("--new", "--", "sh", "-c", "printf '\\033[?1049h\\033[2Jin-alt-screen'; sleep 60"))
    ttys.append(d)
    d.wait_for("in-alt-screen")
    d.send(b"\x02d")
    d.wait_exit()
    e = Tty(socket, ("--attach",))
    ttys.append(e)
    e.wait_for("running terminals")
    check(open_marked(e, "in-alt-screen"), "a terminal whose program is in the alternate screen is shown")
    check(b"\x1b[?1049h" in e.raw, "the host enters the alternate screen for it")
    e.raw = b""
    e.send(b"\x02d")
    check(e.wait_exit() and b"\x1b[?1049l" in e.raw, "and leaves it again when the client leaves")

    # Typing passes through, also keys.
    f = Tty(socket, ("--new",))
    ttys.append(f)
    f.wait_for("Ctrl+B")
    f.send(b"echo made-by-typing; stty size\r")
    check(f.wait_for("made-by-typing") and f.wait_for("24 80"), "typing reaches the terminal, which has the size of the screen")
    fcntl.ioctl(f.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
    os.kill(f.pid, signal.SIGWINCH)
    f.pump(0.5)
    f.send(b"stty size\r")
    check(f.wait_for("30 100"), "a resize of the host goes to the terminal")
    f.send(b"\x02\x02\x15")  # Ctrl+B twice sends one Ctrl+B; Ctrl+U kills the line it made.
    f.pump(0.3)
    f.send(b"exit 4\r")
    check(f.wait_exit() and f.code() == 4, "--new ends with the program's exit code")
    check("exitcode4" in f.text(), "and says how it ended")

    # Taken over: the first client says so and leaves with d.
    g = Tty(socket, ("--new", "--name", "shared", "--", "sh", "-c", "echo g-up; sleep 60"))
    ttys.append(g)
    g.wait_for("g-up")
    h = Tty(socket, ("--attach",))
    ttys.append(h)
    h.wait_for("shared")
    check(open_marked(h, "g-up"), "a second client takes the terminal over")
    check(g.wait_for("taken over", 10), "the client that was taken over says so")
    check(RESTORE_CURSOR in g.raw, "and puts the host terminal back")
    g.send(b"d\n")
    check(g.wait_exit(), "and ends with d")
    h.send(b"\x02d")
    h.wait_exit()

    # The list: the first column is the name of the top folder, an explicit name wins; the default of matmux is
    # the list with New terminal chosen.
    os.makedirs(tmp + "/projects/matterm-demo")
    m1 = Tty(socket, ("--new", "--", "sh", "-c", "echo m1-up; sleep 60"), cwd=tmp + "/projects/matterm-demo")
    ttys.append(m1)
    m1.wait_for("m1-up")
    m1.send(b"\x02d")
    m1.wait_exit()
    m2 = Tty(socket, ("--new", "--name", "Namedone", "--", "sh", "-c", "echo m2-up; sleep 60"), cwd=tmp + "/projects/matterm-demo")
    ttys.append(m2)
    m2.wait_for("m2-up")
    m2.send(b"\x02d")
    m2.wait_exit()
    lst = Tty(socket, ("--attach",))
    ttys.append(lst)
    check(lst.wait_for("matterm-demo") and lst.wait_for("Namedone"), "the list names a terminal by its top folder, an explicit name wins")
    check(lst.wait_for("shell") and lst.wait_for(tmp + "/projects/matterm-demo"), "then what runs and the full folder")
    lst.send(b"q")
    lst.wait_exit()
    # A program started with -- COMMAND shows in the list when it is not a shell (sh -c looks like a shell to the
    # daemon's reading of /proc).
    pr = Tty(socket, ("--new", "--name", "slumber", "--", "sleep", "61"))
    ttys.append(pr)
    pr.pump(1.0)
    pr.send(b"\x02d")
    pr.wait_exit()
    lst2 = Tty(socket, ("--attach",))
    ttys.append(lst2)
    check(lst2.wait_for("slumber") and lst2.wait_for("sleep 61"), "a terminal started with a program shows that program in the list")
    lst2.send(b"q")
    lst2.wait_exit()
    d0 = Tty(socket)  # No arguments, terminals run: the list, New terminal chosen.
    ttys.append(d0)
    check(d0.wait_for("running terminals") and d0.wait_for("New terminal"), "matmux without arguments shows the list when terminals run")
    d0.raw = b""
    d0.send(b"\r")
    before = count_sessions()
    d0.pump(0.3)
    check(d0.wait_for("Ctrl+B") and count_sessions() == before + 1, "and Enter starts a new terminal")
    d0.send(b"\x02d")
    d0.wait_exit()
    d1 = Tty(socket, ("--attach",))
    ttys.append(d1)
    d1.wait_for("running terminals")
    d1.raw = b""
    before = count_sessions()
    d1.send(b"\r")
    d1.pump(1.5)
    check(count_sessions() == before, "matmux --attach has the first terminal chosen: Enter opens it, not a new one")
    d1.send(b"\x02d")
    d1.wait_exit()

    # Ending a terminal from the list: x asks, only y ends; busy and idle ones; the own terminal is refused.
    for args in (("--new", "--name", "idleone"), ("--new", "--name", "busyone", "--", "sleep", "62")):
        t = Tty(socket, args)
        ttys.append(t)
        t.wait_for("Ctrl+B")
        t.send(b"\x02d")
        t.wait_exit()
    base = count_sessions()
    e1 = Tty(socket, ("--attach",))
    ttys.append(e1)
    e1.wait_for("running terminals")
    check(find_row(e1, 'End"idleone"'), "x on a row asks, naming the terminal")
    e1.send(b"\r")  # Enter keeps it.
    e1.pump(0.6)
    check(count_sessions() == base, "Enter keeps the terminal (the default is no)")
    e1.send(b"q")
    e1.wait_exit()
    e2 = Tty(socket, ("--attach",))
    ttys.append(e2)
    e2.wait_for("running terminals")
    check(find_row(e2, 'End"busyone"(sleep62)'), "the question names the program that runs")
    e2.send(b"y")
    check(e2.wait_for("Endedbusyone"), "y ends it and the note says so")
    e2.pump(1.0)
    check(count_sessions() == base - 1, "the busy terminal is gone from the daemon")
    e2.raw = b""
    check(find_row(e2, 'End"idleone"'), "an idle shell asks too")
    e2.send(b"y")
    check(e2.wait_for("Endedidleone"), "and is ended")
    e2.pump(1.0)
    check(count_sessions() == base - 2, "the idle terminal is gone from the daemon")
    e2.send(b"q")
    e2.wait_exit()
    own = Tty(socket, ("--attach",), env={"MATTERM_SESSION": first_id})
    ttys.append(own)
    own.wait_for("running terminals")
    check(find_row(own, "endthisprogramtoo", seconds=0.6), "the terminal matmux runs in cannot be ended from its own list")
    own.send(b"y")
    own.pump(0.5)
    check(count_sessions() >= base - 2, "and y did nothing")
    own.send(b"q")
    own.wait_exit()

    # SIGTERM restores the terminal.
    k = Tty(socket, ("--new",))
    ttys.append(k)
    k.wait_for("Ctrl+B")
    os.kill(k.pid, signal.SIGTERM)
    check(k.wait_exit() and RESTORE_CURSOR in k.raw, "SIGTERM restores the host terminal")
finally:
    for t in ttys:
        t.kill()
    try:
        helper.stdin.write("quit\n")
        helper.stdin.flush()
        helper.wait(timeout=10)
    except Exception:
        helper.kill()
    shutil.rmtree(tmp, ignore_errors=True)

print("%d failed" % len(failures))
sys.exit(1 if failures else 0)
