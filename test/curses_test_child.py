"""Controlled PTY fixture, not a user-facing program or terminal dependency."""
import curses


def run(screen):
    screen.keypad(True)
    try:
        curses.curs_set(0)
    except curses.error:
        pass  # The conservative vt100 terminfo has no cursor-visibility entry.
    screen.border()
    screen.addstr(1, 2, "CURSES_SESSION")
    screen.addstr(3, 2, "scrolling and DEC graphics fixture")
    screen.refresh()
    if screen.getch() != ord("q"):
        raise RuntimeError("unexpected key")


curses.wrapper(run)
