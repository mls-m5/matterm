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
    # No terminal to ask on: nothing is stopped.
    s = run(attach, "--daemon-stop", stdin="y\n")
    check(s.returncode != 0 and "Not stopped" in s.stdout, "stop without a terminal to ask does nothing")
    check(sock in run(attach, "--daemon-list").stdout, "the daemon still runs")
    # With a terminal: answer y.
    import pty, time
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
