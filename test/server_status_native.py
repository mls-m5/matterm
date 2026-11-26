#!/usr/bin/env python3
"""Failure before SDL must be reported to the palette's status tab.

No SSH server, GUI display, or installation is used.
"""
import os
import pathlib
import socket
import subprocess
import sys
import tempfile


def main():
    with tempfile.TemporaryDirectory() as directory:
        fake_ssh = pathlib.Path(directory, "ssh")
        fake_ssh.write_text("#!/bin/sh\necho 'fake SSH unavailable' >&2\nexit 42\n")
        fake_ssh.chmod(0o755)
        parent, child = socket.socketpair()
        # Keep a separate reference to the read end so fd 3 can be passed
        # directly (subprocess closes new fds created inside preexec_fn).
        reader = socket.socket(fileno=os.dup(parent.fileno()))
        parent.close()
        os.dup2(child.fileno(), 3)
        reader.settimeout(20)
        env = dict(os.environ, PATH=directory + os.pathsep + os.environ.get("PATH", ""))

        with subprocess.Popen(
            [sys.argv[1], "--daemon-host", "example.invalid", "--server-ready-fd", "3"],
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            pass_fds=(3,),
        ) as process:
            os.close(3)
            child.close()
            data = bytearray()
            while True:
                chunk = reader.recv(4096)
                if not chunk:
                    break
                data.extend(chunk)
            stderr = process.communicate(timeout=20)[1]
        reader.close()
        assert process.returncode != 0, stderr
        assert b"P Connecting to example.invalid via SSH" in data, (data, stderr)
        assert b"E --daemon-host:" in data, data
        assert b"\nR\n" not in data, data

        # Success is acknowledged only after the GUI actually presents its
        # first frame (software SDL renderer; no desktop or SSH required).
        parent, child = socket.socketpair()
        reader = socket.socket(fileno=os.dup(parent.fileno()))
        parent.close()
        os.dup2(child.fileno(), 3)
        reader.settimeout(20)
        runtime = pathlib.Path(directory, "runtime")
        runtime.mkdir(mode=0o700)
        gui_env = dict(env, SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software",
                       SDL_AUDIODRIVER="dummy", MATTERM_ASSUME_HIT_TEST="1",
                       XDG_CONFIG_HOME=str(pathlib.Path(directory, "config")),
                       XDG_RUNTIME_DIR=str(runtime))
        with subprocess.Popen(
            [sys.argv[1], "--no-daemon", "--server-ready-fd", "3", "--", "/bin/sleep", "10"],
            env=gui_env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            pass_fds=(3,),
        ) as process:
            os.close(3)
            child.close()
            try:
                assert b"R\n" in reader.recv(4096)
            finally:
                process.terminate()
                process.communicate(timeout=20)
        reader.close()


if __name__ == "__main__":
    main()
