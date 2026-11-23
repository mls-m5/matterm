#!/usr/bin/env python3
"""Minimal fake ssh for native perf test: report an owned daemon, relay with delay.

Not an SSH implementation; supports only SshTunnel's discovery and -L test calls.
"""
import os
import select
import signal
import socket
import sys
import threading
import time

args = sys.argv[1:]
if "-L" not in args:
    print(os.environ["PERF_TEST_SOCKET"])
    sys.exit(0)
local, _ = args[args.index("-L") + 1].split(":", 1)
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(local)
server.listen(16)
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))


def relay(client):
    with client, socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
        try:
            peer.connect(os.environ["PERF_TEST_SOCKET"])
            while True:
                ready, _, _ = select.select([client, peer], [], [])
                for source in ready:
                    data = source.recv(65536)
                    if not data:
                        return
                    time.sleep(0.03)
                    (peer if source is client else client).sendall(data)
        except OSError:
            return


while True:
    client, _ = server.accept()
    threading.Thread(target=relay, args=(client,), daemon=True).start()
