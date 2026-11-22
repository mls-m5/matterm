#!/usr/bin/env python3
"""Synthetic Claude-Code-like output: streamed coloured text above, and a live block at the bottom that is
redrawn in place (cursor up, erase line, rewrite) with a spinner, 10-30 updates a second."""
import sys, time, random, os
out = sys.stdout
W = os.get_terminal_size().columns if sys.stdout.isatty() else 100
spin = "|/-\\"
words = "the quick brown fox jumps over a lazy dog while reading the file and running tests".split()
random.seed(1)
def esc(s): out.write(s)
def line(n):
    c = 31 + n % 7
    return "\x1b[%dm%s\x1b[0m \x1b[1m%s\x1b[0m %s" % (c, "●", " ".join(random.choice(words) for _ in range(random.randint(5, 18))), "\x1b[38;5;%dm(%d)\x1b[0m" % (100 + n % 100, n))
BLOCK = 5
esc("\x1b[?25l")
for _ in range(BLOCK): esc("\n")
n = 0
t0 = time.time()
rate = float(os.environ.get("RATE", "20"))
dur = float(os.environ.get("DUR", "60"))
i = 0
while time.time() - t0 < dur:
    # live block: move up, rewrite each line
    esc("\x1b[%dA" % BLOCK)
    for k in range(BLOCK):
        esc("\r\x1b[2K")
        if k == 0:
            esc("\x1b[38;5;208m%s\x1b[0m Thinking… (%ds · ↑ %d tokens · esc to interrupt)" % (spin[i % 4], int(time.time() - t0), i * 7))
        elif k == BLOCK - 2:
            esc("\x1b[48;5;236m\x1b[38;5;250m╭" + "─" * (min(W, 100) - 4) + "╮\x1b[0m")
        elif k == BLOCK - 1:
            esc("\x1b[48;5;236m\x1b[38;5;250m│ > \x1b[0m\x1b[7m \x1b[0m")
        esc("\n" if k < BLOCK - 1 else "")
    esc("\r")
    i += 1
    # a streamed line sometimes goes above the block
    if i % 4 == 0:
        esc("\x1b[%dA\r\x1b[2K%s\n" % (BLOCK - 1, line(n)))
        for _ in range(BLOCK - 1): esc("\n")
        n += 1
    out.flush()
    time.sleep(1.0 / rate)
esc("\x1b[?25h\n")
