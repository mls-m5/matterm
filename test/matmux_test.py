#!/usr/bin/env python3
"""matmux: version, list and stop against a real daemon (private socket folder), and that
the binary has no window library. Usage: matmux_test.py matmux matterm-daemon"""
import os, subprocess, sys, tempfile, shutil, re

attach, daemon = sys.argv[1], sys.argv[2]
runtime = tempfile.mkdtemp(prefix="mta")
env = dict(os.environ, XDG_RUNTIME_DIR=runtime)
ok = True

def check(cond, what):
    global ok
    print(("pass  " if cond else "FAIL  ") + what)
    ok = ok and bool(cond)

def run(*args, stdin=None):
    return subprocess.run(list(args), env=env, capture_output=True, text=True, input=stdin, timeout=30)

try:
    v = run(attach, "--version")
    check(v.returncode == 0 and re.search(r"matmux \d+\.\d+\.\d+\+", v.stdout), "--version prints format.breaking.additive+hash")
    check("SDL" not in subprocess.run(["ldd", attach], capture_output=True, text=True).stdout, "no SDL in the dynamic dependencies")
    check(run(attach, "--daemon-list").stdout.startswith("No daemon found"), "list with no daemon")
    sock = run(daemon, "--ensure", "--print-socket").stdout.strip()
    check(re.search(r"daemon-\d+\.\d+\.\d+\.sock$", sock), "the daemon listens on a versioned socket: " + sock)
    out = run(attach, "--daemon-list").stdout
    check(sock in out and "terminals" in out, "list names the socket and terminals")
    # No arguments: a new terminal with the shell (one line first when terminals already run); Ctrl+B d
    # leaves it running.
    import pty, time, select, fcntl, termios, struct

    def session(*args):
        pid, fd = pty.fork()
        if pid == 0:
            os.environ["XDG_RUNTIME_DIR"] = runtime
            os.environ["SHELL"] = "/bin/sh"
            os.execv(attach, [attach, *args])
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        return pid, fd

    def read_until(fd, needle, seconds=10):
        end, raw = time.time() + seconds, b""
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    break
                raw += chunk
                if needle in re.sub(rb"\x1b\[[0-9;?<]*[A-Za-z]", b"", raw).replace(b" ", b""):
                    return True, raw
        return False, raw

    pid, fd = session()
    ok1, raw = read_until(fd, b"Ctrl+B")
    check(ok1, "matmux without arguments starts a new terminal and attaches (status line)")
    check(b"terminals run" not in raw and b"terminal runs" not in raw, "no 'terminals run' line when none ran")
    os.write(fd, b"\x02d")
    _, st = os.waitpid(pid, 0)
    check(os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0, "Ctrl+B d leaves with exit code 0")
    check("1 terminals" in run(attach, "--daemon-list").stdout, "and the terminal keeps running")
    pid, fd = session()
    ok2, raw = read_until(fd, b"terminalruns:matmux--attach")
    check(ok2, "the next matmux says '1 terminal runs: matmux --attach' first")
    os.write(fd, b"\x02d")
    os.waitpid(pid, 0)
    check("2 terminals" in run(attach, "--daemon-list").stdout, "a second terminal was made")
    pid, fd = session("--attach")
    ok3, raw = read_until(fd, b"New terminal")
    check(ok3 or b"terminal" in raw, "matmux --attach shows the list")
    os.write(fd, b"q")
    _, st = os.waitpid(pid, 0)
    check(os.WIFEXITED(st), "q leaves the list")
    # No terminal to ask on: nothing is stopped.
    s = run(attach, "--daemon-stop", stdin="y\n")
    check(s.returncode != 0 and "Not stopped" in s.stdout, "stop without a terminal to ask does nothing")
    check(sock in run(attach, "--daemon-list").stdout, "the daemon still runs")
    # With a terminal: answer y.
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["XDG_RUNTIME_DIR"] = runtime
        os.execv(attach, [attach, "--daemon-stop"])
    time.sleep(0.8)
    asked = os.read(fd, 4000).decode()
    check("Stop the daemon on " + sock in asked and "terminals" in asked, "stop asks and names the terminals")
    os.write(fd, b"y\n")
    time.sleep(1.5)
    _, status = os.waitpid(pid, 0)
    check(os.WEXITSTATUS(status) == 0, "stop succeeded")
    check(run(attach, "--daemon-list").stdout.startswith("No daemon found") or "nothing listens" in run(attach, "--daemon-list").stdout,
          "the daemon is gone")
finally:
    subprocess.run([daemon, "--help"], capture_output=True)
    shutil.rmtree(runtime, ignore_errors=True)
sys.exit(0 if ok else 1)
