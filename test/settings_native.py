#!/usr/bin/env python3
"""Settings palette action launches a local process even in a remote window."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

matterm, daemon = map(os.path.abspath, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="matterm-settings-native-") as tmp:
    socket = tmp + "/daemon.sock"
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               SDL_RENDER_DRIVER="software", XDG_CONFIG_HOME=tmp + "/config",
               XDG_RUNTIME_DIR=tmp, SHELL="/bin/sh", MATTERM_LAYOUT_SMOKE="settings")
    env.pop("MATTERM_PERF_LOG", None)
    server = subprocess.Popen([daemon, "--daemon-socket", socket], env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 10
        while not os.path.exists(socket):
            if server.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("daemon did not start")
            time.sleep(0.02)
        os.mkdir(tmp + "/bin")
        relay = os.path.join(os.path.dirname(os.path.abspath(__file__)), "perfssh.py")
        with open(tmp + "/bin/ssh", "w") as shim:
            shim.write('#!/bin/sh\nexec "%s" "%s" "$@"\n' % (sys.executable, relay))
        os.chmod(tmp + "/bin/ssh", 0o755)
        for mode in ("local", "remote"):
            shutil.rmtree(tmp + "/config", ignore_errors=True)
            run_env = dict(env, PATH=tmp + "/bin:" + env["PATH"], PERF_TEST_SOCKET=socket)
            args = [matterm, "--pty-layout-smoke-test"]
            args += ["--daemon-host", "test-settings-host"] if mode == "remote" else ["--daemon-socket", socket]
            result = subprocess.run(args, env=run_env, capture_output=True, text=True, timeout=45)
            if result.returncode:
                raise RuntimeError(mode + ": " + result.stdout + result.stderr)
            with open(tmp + "/config/matterm/settings.json", encoding="utf-8") as file:
                assert json.load(file)["theme"] == "blue", "Settings did not edit the client file"
            print("pass native local Settings tab:", mode)
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
