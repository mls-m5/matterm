#!/usr/bin/env python3
"""A missing remote daemon asks the original GUI; refusal never installs.

Uses fakessh.py and a private HOME; never contacts or changes a real host.
"""
import os
import pathlib
import socket
import subprocess
import sys
import tempfile


def main():
    binary, fake_ssh = sys.argv[1:3]
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        (root / "ssh").symlink_to(pathlib.Path(fake_ssh).resolve())
        home = root / "remote-home"
        home.mkdir()
        parent, child = socket.socketpair()
        reader = socket.socket(fileno=os.dup(parent.fileno()))
        parent.close()
        os.dup2(child.fileno(), 3)
        reader.settimeout(25)
        env = dict(os.environ, PATH=directory + os.pathsep + "/usr/bin:/bin",
                   FAKE_SSH_HOME=str(home), XDG_CONFIG_HOME=str(root / "config"))
        with subprocess.Popen(
            [binary, "--daemon-host", "example.invalid", "--server-ready-fd", "3"],
            env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, pass_fds=(3,),
        ) as process:
            os.close(3)
            child.close()
            data = bytearray()
            while b"\nI " not in b"\n" + bytes(data):
                chunk = reader.recv(4096)
                if not chunk:
                    raise AssertionError((data, process.communicate(timeout=10)[1]))
                data.extend(chunk)
            assert b"Copy ours" in data and b"example.invalid" in data, data
            reader.sendall(b"n")
            while True:
                chunk = reader.recv(4096)
                if not chunk:
                    break
                data.extend(chunk)
            stderr = process.communicate(timeout=25)[1]
        reader.close()
        assert process.returncode != 0, (data, stderr)
        assert b"E --daemon-host:" in data, (data, stderr)
        assert b"R\n" not in data, data
        installed = home / ".local" / "share" / "matterm" / "bin"
        assert not installed.exists() or not list(installed.iterdir())


if __name__ == "__main__":
    main()
