#!/usr/bin/env python3
"""A program that asks the terminal what it is (device attributes, cursor position, version, background colour) in raw
mode, as neovim does at start, and writes what it gets back to the file given as argument (a Python repr)."""
import os, select, sys, termios, tty

fd = 0
saved = termios.tcgetattr(fd)
tty.setraw(fd)
try:
    os.write(1, b"\x1b[c\x1b[6n\x1b[>q\x1b]11;?\x1b\\")
    got = b""
    end = __import__("time").time() + float(sys.argv[2]) if len(sys.argv) > 2 else 1.5
    while __import__("time").time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            got += os.read(fd, 1024)
finally:
    termios.tcsetattr(fd, termios.TCSADRAIN, saved)
with open(sys.argv[1], "w") as f:
    f.write(repr(got))
