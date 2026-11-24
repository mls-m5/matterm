#!/usr/bin/env python3
"""Native Window Detach preserves two daemon programs for reattachment."""
import os
import re
import subprocess
import sys
import tempfile
import time

matterm, daemon = map(os.path.abspath, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="matterm-window-detach-") as tmp:
    socket = tmp + "/daemon.sock"
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               SDL_RENDER_DRIVER="software", XDG_CONFIG_HOME=tmp + "/config",
               XDG_RUNTIME_DIR=tmp, SHELL="/bin/sh")
    env.pop("MATTERM_PERF_LOG", None)
    server = subprocess.Popen([daemon, "--daemon-socket", socket], env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 10
        while not os.path.exists(socket):
            if server.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("daemon did not start")
            time.sleep(0.02)
        args = [matterm, "--pty-layout-smoke-test", "--daemon-socket", socket]
        def run(variant, **extra):
            result = subprocess.run(args, env=dict(env, MATTERM_LAYOUT_SMOKE=variant, **extra),
                                    capture_output=True, text=True, timeout=40)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            return result.stdout
        first = run("window-detach")
        match = re.search(r"window-detach-ids:([^\s]+)", first)
        if not match:
            raise RuntimeError("missing retained terminal IDs: " + first)
        run("window-reattach", MATTERM_DETACH_IDS=match.group(1))
        print("pass native Window Cancel/Detach and two-program reattachment")
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
