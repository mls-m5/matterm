#!/usr/bin/env python3
"""Transactional same-pane attach: real daemon, split layout, failures and takeover."""
import os
import subprocess
import sys
import tempfile
import time

matterm, daemon = map(os.path.abspath, sys.argv[1:3])
for case in ("empty", "typed", "busy", "unknown", "fail", "cancel", "timeout", "stale", "closed", "takeover"):
    with tempfile.TemporaryDirectory(prefix="matterm-attach-here-") as tmp:
        socket = tmp + "/daemon.sock"
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                   SDL_RENDER_DRIVER="software", XDG_CONFIG_HOME=tmp + "/config",
                   XDG_RUNTIME_DIR=tmp, SHELL="/bin/sh", MATTERM_LAYOUT_SMOKE="attachhere-" + case)
        env.pop("MATTERM_PERF_LOG", None)
        server = subprocess.Popen([daemon, "--daemon-socket", socket], env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 10
            while not os.path.exists(socket):
                if server.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("daemon did not start")
                time.sleep(0.02)
            result = subprocess.run([matterm, "--pty-layout-smoke-test", "--daemon-socket", socket],
                                    env=env, capture_output=True, text=True, timeout=30)
            if result.returncode:
                raise RuntimeError(case + ": " + result.stdout + result.stderr)
            print("pass real-daemon attach-here transaction:", case)
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
