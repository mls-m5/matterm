# fakeos easter eggs (spoilers)

Not mentioned by `help`. Eggs are rows in `eggs()` (`src/fakeos/eggs.cpp`), or
files and programs in the virtual file system, or changes a program makes to the
`World` when it ends. All are deterministic and are forgotten by `reset`.

| Discovery | How |
|---|---|
| The fire | `fire` is "command not found": `/usr/games` is not in the `PATH`. `ls /usr/games`, then run `/usr/games/fire`. |
| An ember | Watch the fire for at least 8 seconds, then quit. The ashes leave `~/.ember`, which points to the cave's word. (Also hinted by `~/.plan` and `~/notes.txt`.) |
| The magic word | `xyzzy` (the first time): a voice says "Fool." and `/usr/games/plasma` appears. Later: "Nothing happens." |
| Plasma | `/usr/games/plasma` runs the gradient program (`q` quits, space changes the pattern). |
| sudo | `sudo ...` is "not in the sudoers file. This incident will be reported." |
| Sandwiches | `make me a sandwich` and `sudo make me a sandwich`. |
| `hello` | Greets you. |
| rm -rf / | Refused like GNU rm, with a hint about `--no-preserve-root` (which the pretend `rm` does not honor: the world is safe anyway, and `reset` restores it). |

Planned: clicks on drawn text through mouse reporting, more programs and
animations (milestone 3). `Egg::Trigger` has no click kind yet; programs that turn
on mouse reporting (the intro already does) receive SGR reports as keys.
