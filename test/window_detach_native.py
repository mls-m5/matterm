#!/usr/bin/env python3
"""Native scoped Detach preserves daemon programs and tab layouts for reattach."""
import json
import os
import re
import subprocess
import sys
import tempfile
import time

matterm, daemon = map(os.path.abspath, sys.argv[1:3])
for scope in ("window", "pane", "tab"):
    with tempfile.TemporaryDirectory(prefix="matterm-scoped-detach-") as tmp:
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
                    raise RuntimeError(scope + ": " + result.stdout + result.stderr)
                return result.stdout
            first = run(scope + "-detach")
            match = re.search(r"window-detach-ids:([^\s]+)", first)
            if not match:
                raise RuntimeError("missing retained terminal IDs: " + first)
            extra = {"MATTERM_DETACH_IDS": match.group(1)}
            if scope == "window":
                directory = os.path.join(env["XDG_CONFIG_HOME"], "matterm", "layouts")
                paths = [os.path.join(directory, name) for name in os.listdir(directory) if name.endswith(".json")]
                if len(paths) != 2:
                    raise RuntimeError("expected two saved window tab layouts")
                layouts = [json.load(open(path, encoding="utf-8")) for path in paths]
                text = json.dumps(layouts)
                if not all(terminal_id in text for terminal_id in match.group(1).split(",")):
                    raise RuntimeError("saved layouts do not contain all daemon session IDs")
            if scope == "tab":
                layout = re.search(r"scoped-detach-layout:([^\s]+)", first)
                if not layout:
                    raise RuntimeError("missing retained layout: " + first)
                extra["MATTERM_REATTACH_LAYOUT"] = layout.group(1)
            run("window-reattach", **extra)
            print("pass native scoped Cancel/Detach and live program reattachment:", scope)
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
