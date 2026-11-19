# fakeos easter eggs (spoilers)

Not mentioned by `help`. Eggs are rows in `eggs()` (`src/fakeos/eggs.cpp`), or
files and programs in the virtual file system, or changes a program makes to the
`World` when it ends. All are deterministic and are forgotten by `reset`.

| Discovery | How |
|---|---|
| The fire | Not hidden: `fire` and `gradient` are in the `PATH` (`/usr/games`) and in the motd, README and `help`. The rewards below are what is hidden. |
| An ember | Watch the fire for at least 8 seconds, then quit. The ashes leave `~/.ember`, which points to the cave's word. (Also hinted by `~/.plan` and `~/notes.txt`.) |
| The magic word | `xyzzy` (the first time): a voice says "Fool." and `/usr/games/plasma` appears. Later: "Nothing happens." |
| Plasma | `/usr/games/plasma` runs the gradient program (`q` quits, space changes the pattern). |
| sudo | `sudo ...` is "not in the sudoers file. This incident will be reported." |
| Sandwiches | `make me a sandwich` and `sudo make me a sandwich`. |
| `hello` | Greets you. |
| The pills | In plasma, click `( blue )` or `( red )`, or press `b` or `r`. Blue ends the story with a short line. Red opens the rabbit hole: the Matrix sequence (see below). |
| The shy game | `ls -a /usr/games` shows `.crumble` (hinted by `~/.plan`): a word in big letters cracks and falls into a pile. `crumble` takes other words (`/usr/games/.crumble HI`), space plays it again. |
| Editors | `vim`, `vi`, `nano`, `emacs`: "There is no editor here". |
| Quick ones | `42`, `neo`, the fork bomb `:(){ :|:& };:`, `rm -rf --no-preserve-root /`. |
| rm -rf / | Refused like GNU rm, with a hint about `--no-preserve-root` (which the pretend `rm` does not honor: the world is safe anyway, and `reset` restores it). |

Clicks: `Egg::Trigger::Click` with `match = "program/word"` fires when a left
click, through mouse reporting, lands on a word that program drew. The program
ends, the egg's text is shown on the restored screen and its `launch` (if any)
starts next. The plasma program turns mouse reporting on and offers two pills
(`( blue )` and `( red )`); `b` and `r` make the same choice from the keyboard.
Blue ends the story with a short line.

## The Matrix sequence

The trail: the motd and README point at `fire` -> watch `fire` for 8 seconds -> `~/.ember` -> `xyzzy` ->
`/usr/games/plasma` -> the red pill. Then, as one scripted program
(`src/fakeos/matrix.cpp`, timing in named constants at the top):

1. The screen goes black (0.9 s).
2. `Hello, Neo.` is typed out with a blinking cursor and stays for a while.
3. `Follow the white rabbit.` replaces it the same way.
4. Digital rain: columns of falling glyphs with a bright head and a fading tail,
   seeded, resizing with the window, for about 14 seconds, then the last drops run
   out.
5. The intro plays again (a key makes it fade out quickly) and leads to the prompt. The
   world keeps its changes; the discovery flags (`pill-red`, `matrix-seen`) stay.

A key skips one part at a time: the black, the first line, the second line, the
rain. The pills' texts live in `eggs.cpp`.
