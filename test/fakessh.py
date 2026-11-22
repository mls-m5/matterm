#!/usr/bin/env python3
"""A stand-in for ssh in sshtunnel_test: no network, no sshd.

  fakessh.py [options] -- HOST COMMAND...   runs COMMAND here (matterm-daemon is $FAKE_SSH_DAEMON)
  fakessh.py [options] -N -L LOCAL:REMOTE -- HOST   listens on the Unix socket LOCAL and relays every
                                                      connection to the Unix socket REMOTE until killed
Every call appends its arguments (one line, JSON) to $FAKE_SSH_LOG. $FAKE_SSH_FAIL makes the command form
fail with that message; $FAKE_SSH_NOFORWARD makes the forward form exit at once with that message.
"""
import json, os, signal, socket, sys, threading, select

args = sys.argv[1:]
log = os.environ.get("FAKE_SSH_LOG")
if log:
    with open(log, "a") as f:
        f.write(json.dumps(args) + "\n")

forward = None
i = 0
host = None
command = []
while i < len(args):
    a = args[i]
    if a == "--":
        host = args[i + 1]
        command = args[i + 2:]
        break
    if a in ("-o", "-L", "-p", "-i", "-l", "-F"):
        if a == "-L":
            forward = args[i + 1]
        i += 2
        continue
    i += 1

if forward is None:
    if os.environ.get("FAKE_SSH_FAIL"):
        sys.stderr.write(os.environ["FAKE_SSH_FAIL"] + "\n")
        sys.exit(255)
    if command and command[0] == "matterm-daemon":
        os.execv(os.environ["FAKE_SSH_DAEMON"], [os.environ["FAKE_SSH_DAEMON"]] + command[1:])
    sys.stderr.write("fakessh: unknown command %r\n" % command)
    sys.exit(127)

if os.environ.get("FAKE_SSH_NOFORWARD"):
    sys.stderr.write(os.environ["FAKE_SSH_NOFORWARD"] + "\n")
    sys.exit(255)

local, remote = forward.split(":", 1)
try:
    os.unlink(local)
except FileNotFoundError:
    pass
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(local)
server.listen(16)
signal.signal(signal.SIGTERM, lambda *_: os._exit(0))

def relay(a, b):
    try:
        while True:
            r, _, _ = select.select([a, b], [], [])
            for s in r:
                data = s.recv(65536)
                if not data:
                    return
                (b if s is a else a).sendall(data)
    except OSError:
        pass
    finally:
        a.close(); b.close()

while True:
    c, _ = server.accept()
    r = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        r.connect(remote)
    except OSError:
        c.close(); continue
    threading.Thread(target=relay, args=(c, r), daemon=True).start()
