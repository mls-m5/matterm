"""Test DSR parsing/timeout and terminal restoration without a graphical terminal."""
import errno
import os
import pty
import select
import subprocess
import sys
import termios
import time


def run(reply):
    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    child = subprocess.Popen(
        [sys.argv[1], "color", "8192", "17", "--barrier"],
        stdin=slave, stdout=slave, stderr=slave, close_fds=True,
    )
    output = bytearray()
    pending = bytearray()
    queries = 0
    deadline = time.monotonic() + 6
    try:
        while time.monotonic() < deadline:
            if select.select([master], [], [], 0.05)[0]:
                try:
                    data = os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        break
                    raise
                output.extend(data)
                pending.extend(data)
                while b"\x1b[6n" in pending:
                    index = pending.index(b"\x1b[6n")
                    del pending[:index + 4]
                    queries += 1
                    if reply:
                        # Fragment the reply to exercise incremental recognition.
                        os.write(master, b"\x1b[12;")
                        os.write(master, b"34R")
                if len(pending) > 4096:
                    del pending[:-4]
            if child.poll() is not None:
                break
        if child.poll() is None:
            raise AssertionError("benchmark did not terminate")
        assert termios.tcgetattr(slave) == original, "terminal mode not restored"
        if reply:
            assert child.returncode == 0, output
            assert queries == 2, queries
            assert b"dsr_processed_ms=" in output, output
            assert b"display_complete=unknown" in output, output
        else:
            assert child.returncode != 0, output
            assert b"timed out" in output, output
    finally:
        if child.poll() is None:
            child.kill()
        child.wait()
        os.close(master)
        os.close(slave)


run(True)
run(False)
