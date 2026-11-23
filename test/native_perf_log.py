#!/usr/bin/env python3
"""Opt-in native logging: real daemon, SDL dummy, local and delayed SSH relay."""
import os
import shutil
import subprocess
import sys
import tempfile
import time

matterm, daemon = map(os.path.abspath, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="matterm-native-perf-") as tmp:
    socket = tmp + "/daemon.sock"
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               SDL_RENDER_DRIVER="software", XDG_CONFIG_HOME=tmp + "/config",
               XDG_RUNTIME_DIR=tmp, SHELL="/bin/sh", PS1="$ ")
    env.pop("MATTERM_PERF_LOG", None)
    server = subprocess.Popen([daemon, "--daemon-socket", socket], env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 10
        while not os.path.exists(socket):
            if server.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("daemon did not start")
            time.sleep(0.02)
        bindir = tmp + "/bin"
        os.mkdir(bindir)
        relay = os.path.join(os.path.dirname(os.path.abspath(__file__)), "perfssh.py")
        with open(bindir + "/ssh", "w") as shim:
            shim.write('#!/bin/sh\nexec "%s" "%s" "$@"\n' % (sys.executable, relay))
        os.chmod(bindir + "/ssh", 0o755)
        for mode in ("local", "remote", "off", "unwritable"):
            shutil.rmtree(tmp + "/config", ignore_errors=True)
            log = tmp + "/" + mode + ".log"
            run_env = dict(env, PATH=bindir + ":" + env["PATH"], PERF_TEST_SOCKET=socket)
            args = [matterm, "--pty-layout-smoke-test"]
            args += ["--daemon-host", "delayed-test-host"] if mode == "remote" else ["--daemon-socket", socket]
            if mode == "unwritable":
                args += ["--perf-log", tmp + "/missing/log"]
            elif mode != "off":
                args += ["--perf-log", log]
            result = subprocess.run(args, env=run_env, capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise RuntimeError(mode + ": " + result.stdout + result.stderr)
            if mode in ("off", "unwritable"):
                assert not os.path.exists(log)
                if mode == "unwritable":
                    assert "performance log:" in result.stderr, result.stderr
                continue
            with open(log) as stream:
                text = stream.read()
            for field in ("matterm-native", "summary", "verdict", "in_bytes=", "in_msg=",
                          "parse_ms=", "layout_ms=", "draw_ms=", "cpu_ms=", "rtt_n="):
                assert field in text, (mode, field, text)
            lines = [line for line in text.splitlines() if "summary" not in line and "in_bytes=" in line]
            assert lines, text
            assert any(int(line.split("in_bytes=")[1].split()[0]) > 0 for line in lines), text
            assert all(float(line.split("interval_s=")[1].split()[0]) >= 0 for line in lines), text
            if mode == "remote":
                # Delay is injected in both directions. No root/tc/netem/sshd.
                measured = [float(line.split("| rtt_n=")[1].split("max_ms=")[1].split()[0])
                            for line in lines if "| rtt_n=0 " not in line]
                assert measured and max(measured) >= 50, text
            print("native client performance log (%s): pass" % mode)
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
