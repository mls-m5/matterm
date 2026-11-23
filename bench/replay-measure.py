#!/usr/bin/env python3
"""Reattach to a daemon terminal whose replay ring is full, in the real window (headless: dummy video
driver, software renderer) and in matmux, and report how long it takes and what it costs.

usage: replay-measure.py BUILD_DIR [--mib N] [--panes K]
Starts a daemon on a private socket, makes K terminals that each print about N MiB of coloured lines
(CreateTerminal), then starts `matterm` with MATTERM_ATTACH_TERMINAL=<id> (the first pane attaches to it)
and watches the process for 3 s: the CPU time used and when 95 % of it had been used, and the rounds / frames of MATTERM_LOOP_STATS. Docs: docs/CPU.md "Replay"."""
import os, shutil, signal, socket, struct, subprocess, sys, tempfile, time

build = sys.argv[1]
mib = int(sys.argv[sys.argv.index("--mib") + 1]) if "--mib" in sys.argv else 4
panes = int(sys.argv[sys.argv.index("--panes") + 1]) if "--panes" in sys.argv else 1
tmp = tempfile.mkdtemp(prefix="replaymeasure")
env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
           XDG_RUNTIME_DIR=tmp + "/run", XDG_CONFIG_HOME=tmp + "/config", SHELL="/bin/sh", PS1="$ ", HOME=tmp)
os.makedirs(tmp + "/run", mode=0o700)
sock = tmp + "/run/d.sock"
daemon = subprocess.Popen([build + "/matterm-daemon", "--daemon-socket", sock, "--idle-seconds", "0"], env=env)
for _ in range(100):
    if os.path.exists(sock):
        break
    time.sleep(0.05)


def frame(payload):
    return struct.pack(">I", len(payload)) + payload


def string(text):
    data = text.encode()
    return struct.pack(">H", len(data)) + data


def control(request):
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    s.sendall(frame(b"\x40" + struct.pack(">H", 3)))
    buffer = b""

    def read_frame():
        nonlocal buffer
        while len(buffer) < 4 or len(buffer) < 4 + struct.unpack(">I", buffer[:4])[0]:
            buffer += s.recv(65536)
        n = struct.unpack(">I", buffer[:4])[0]
        out, buffer = buffer[4:4 + n], buffer[4 + n:]
        return out
    read_frame()  # Welcome
    s.sendall(frame(request))
    reply = read_frame()
    s.close()
    return reply


def create(lines):
    command = ["/bin/sh", "-c", "seq 1 %d | sed 's/.*/\\x1b[32mline & of the log\\x1b[0m the quick brown fox jumps "
               "over the lazy dog/'; sleep 600" % lines]
    body = b"\x4a" + string("") + struct.pack(">HH", 100, 40) + bytes([len(command)]) + b"".join(string(c) for c in command) + string("")
    reply = control(body)
    assert reply[0] == 0xc5, reply
    n = struct.unpack(">H", reply[1:3])[0]
    return reply[3:3 + n].decode()


def ring_end(session):
    reply = control(b"\x41")
    assert reply[0] in (0xc1, 0xc7)
    n = struct.unpack(">H", reply[1:3])[0]
    pos = 3
    for _ in range(n):
        l = struct.unpack(">H", reply[pos:pos + 2])[0]
        sid = reply[pos + 2:pos + 2 + l].decode()
        pos += 2 + l + 1
        end = struct.unpack(">Q", reply[pos:pos + 8])[0]
        pos += 8
        if reply[0] == 0xc7:
            l = struct.unpack(">H", reply[pos:pos + 2])[0]
            pos += 2 + l
        if sid == session:
            return end
    return 0


def cpu(pid):
    with open("/proc/%d/stat" % pid) as f:
        fields = f.read().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


try:
    ids = [create(mib * 1048576 // 95) for _ in range(panes)]
    last = [0] * panes
    for _ in range(300):
        time.sleep(0.2)
        now = [ring_end(i) for i in ids]
        if all(n > 0 and n == l for n, l in zip(now, last)):
            break
        last = now
    print("ring: %.2f MiB in each of %d terminal(s)" % (last[0] / 1048576, panes))
    window_env = dict(env, MATTERM_ATTACH_TERMINAL=ids[0], MATTERM_LOOP_STATS="1")
    started = time.time()
    window = subprocess.Popen([build + "/matterm", "--daemon-socket", sock], env=window_env, stderr=subprocess.PIPE,
                              stdout=subprocess.DEVNULL)
    # CPU time used so far, every 20 ms for 3 s; "done" = when 95 % of what was used in those 3 s was used.
    samples = []
    while time.time() - started < 3.0:
        time.sleep(0.02)
        samples.append((time.time() - started, cpu(window.pid)))
        if "--trace" in sys.argv:
            print("  t=%.2f cpu=%.2f s" % samples[-1])
    total_cpu = samples[-1][1]
    settled = next(t for t, c in samples if c >= 0.95 * total_cpu)
    window.send_signal(signal.SIGTERM)
    try:
        err = window.communicate(timeout=10)[1].decode()
    except subprocess.TimeoutExpired:
        window.kill()
        err = ""
    stats = [l for l in err.splitlines() if "loop rounds" in l]
    print("window: %.2f s until 95%% of the CPU of the first 3 s was used (%.2f s of CPU in 3 s); %s" % (
        settled, total_cpu, stats[0] if stats else "no loop stats"))
finally:
    daemon.terminate()
    daemon.wait()
    shutil.rmtree(tmp, ignore_errors=True)
