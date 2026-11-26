#!/usr/bin/env python3
"""Kill only a native window; reattach its named split tab to live PTYs."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time

matterm, daemon = map(os.path.abspath, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="matterm-recovery-") as tmp:
    sock = tmp + "/daemon.sock"
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               SDL_RENDER_DRIVER="software", XDG_CONFIG_HOME=tmp + "/config",
               XDG_RUNTIME_DIR=tmp, SHELL="/bin/sh")
    server = subprocess.Popen([daemon, "--daemon-socket", sock], env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    window = None
    try:
        deadline = time.monotonic() + 10
        while not os.path.exists(sock):
            assert server.poll() is None and time.monotonic() < deadline
            time.sleep(0.02)
        args = [matterm, "--pty-layout-smoke-test", "--daemon-socket", sock]
        window = subprocess.Popen(args, env=dict(env, MATTERM_LAYOUT_SMOKE="recovery-create"),
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 20
        while True:
            files = list(pathlib.Path(tmp, "config").glob("matterm/layouts/recovery-*/*.json"))
            if files:
                saved = json.loads(files[0].read_text())
                if "split" in saved["tabs"][0]["layout"]:
                    break
            assert window.poll() is None, window.communicate()
            assert time.monotonic() < deadline, files
            time.sleep(0.05)
        window.kill()
        window.communicate(timeout=10)
        result = subprocess.run(args, env=dict(env, MATTERM_LAYOUT_SMOKE="recovery-check"),
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        print("pass: named split tab recovered after SIGKILL with original live terminal IDs")
    finally:
        if window is not None and window.poll() is None:
            window.kill()
            window.communicate()
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
