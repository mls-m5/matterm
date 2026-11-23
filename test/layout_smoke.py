#!/usr/bin/env python3
"""Detach tab, then "Attach tab..." is offered: with a local daemon and with --daemon-host through
the fake ssh (test/fakessh.py). usage: layout_smoke.py MATTERM MATTERM-DAEMON FAKESSH [local|remote]"""
import os, shutil, stat, subprocess, sys, tempfile

matterm, daemon, fakessh, mode = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
tmp = tempfile.mkdtemp(prefix="layoutsmoke")
try:
    env = dict(os.environ)
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
               XDG_CONFIG_HOME=tmp + "/config", XDG_RUNTIME_DIR=tmp + "/run", SHELL="/bin/sh", PS1="$ ")
    os.makedirs(tmp + "/run", mode=0o700)
    os.makedirs(tmp + "/home")
    args = [matterm, "--pty-layout-smoke-test"]
    if mode == "remote":
        bin_dir = tmp + "/bin"
        os.makedirs(bin_dir)
        shim = bin_dir + "/ssh"
        with open(shim, "w") as f:
            f.write('#!/bin/sh\nexec python3 "%s" "$@"\n' % fakessh)
        os.chmod(shim, 0o755)
        env.update(PATH=bin_dir + ":" + env["PATH"], FAKE_SSH_DAEMON=daemon, FAKE_SSH_DAEMON_ON_PATH="1",
                   FAKE_SSH_HOME=tmp + "/home")
        args += ["--daemon-host", "fakehost"]
    else:
        args += ["--daemon-socket", tmp + "/run/d.sock"]
    ok = True
    # One window does both; then the only tab is detached (the window ends) and a later window checks.
    # The folders of a saved session are the daemon's, also when it is on another computer.
    ok_dir = tmp + "/okdir"
    os.makedirs(ok_dir)
    missing_dir = tmp + "/missing-on-the-other-computer"
    for variant in ("", "detach-only", "check", "cwd"):
        run_env = dict(env)
        run_env["MATTERM_LAYOUT_SMOKE"] = variant
        if variant == "":
            shutil.rmtree(tmp + "/config", ignore_errors=True)
        if variant == "detach-only":
            shutil.rmtree(tmp + "/config", ignore_errors=True)
        if variant == "cwd":
            shutil.rmtree(tmp + "/config", ignore_errors=True)
            sessions = tmp + "/config/matterm/sessions"
            os.makedirs(sessions, mode=0o700)
            os.chmod(tmp + "/config/matterm", 0o700)
            os.chmod(tmp + "/config", 0o700)
            node = ('{"split":"h","ratio":500,"a":{"pane":{"cwd":"%s"}},"b":{"pane":{"cwd":"%s"}}}'
                    % (ok_dir, missing_dir))
            with open(sessions + "/cwdtest.json", "w") as f:
                f.write('{"version":1,"name":"cwdtest","saved":1,"active":0,"tabs":[{"title":"","focus":0,"layout":%s}]}' % node)
            os.chmod(sessions + "/cwdtest.json", 0o600)
            run_env["MATTERM_SMOKE_CWD_OK"] = ok_dir
            run_env["MATTERM_SMOKE_CWD_MISSING"] = missing_dir
        result = subprocess.run(args, env=run_env, capture_output=True, text=True, timeout=90)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        good = result.returncode == 0 and "passed" in result.stdout
        print("  variant %r: %s" % (variant or "one window", "pass" if good else "FAIL"))
        ok = ok and good
    print("layout smoke (%s): %s" % (mode, "pass" if ok else "FAIL"))
    sys.exit(0 if ok else 1)
finally:
    # The daemon of the smoke test ends by itself after its idle time; make sure none is left.
    subprocess.run(["pkill", "-f", tmp], capture_output=True)
    shutil.rmtree(tmp, ignore_errors=True)
