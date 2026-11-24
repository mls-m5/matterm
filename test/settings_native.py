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
        for mode, variant in (("local", "settings"), ("remote", "settings"),
                              ("local", "settings-palette"), ("remote", "settings-palette")):
            shutil.rmtree(tmp + "/config", ignore_errors=True)
            run_env = dict(env, PATH=tmp + "/bin:" + env["PATH"], PERF_TEST_SOCKET=socket,
                           MATTERM_LAYOUT_SMOKE=variant)
            log = tmp + "/settings-phases-" + mode + "-" + variant + ".log"
            args = [matterm, "--pty-layout-smoke-test", "--perf-log", log]
            args += ["--daemon-host", "test-settings-host"] if mode == "remote" else ["--daemon-socket", socket]
            result = subprocess.run(args, env=run_env, capture_output=True, text=True, timeout=45)
            if result.returncode:
                raise RuntimeError(mode + ": " + result.stdout + result.stderr)
            with open(tmp + "/config/matterm/settings.json", encoding="utf-8") as file:
                saved = json.load(file)
                assert saved["theme"] == ("green" if variant == "settings-palette" else "blue"), "Settings did not edit the client file"
                assert saved["drawBatching"] is False, "Draw batching was not disabled in the local client file"
            with open(log, encoding="utf-8") as file:
                records = file.read().splitlines()
            expected = 3 if variant == "settings-palette" else 1
            for phase in ("settings_factory", "settings_first_parsed_output", "settings_first_present"):
                phases = [line for line in records if "phase=" + phase + " " in line and "status=ok" in line]
                assert len(phases) == expected, (phase, expected, phases)
                elapsed = [float(line.split("phase_ms=")[1].split()[0]) for line in phases]
                assert all(value >= 0 for value in elapsed), phases
                print("settings phase", mode, variant, phase, "ms", elapsed)
            print("pass native local Settings tab:", mode, variant)
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
