# agent2: delat terminallager (`src/tty/`)

Kontrakt: `docs/tasks/attach-tty.md`, del 1 och 2. Det här är grunden för `--attach-tty` (agent3 bygger läget
ovanpå) och används redan av demons `--tty`, `mattrix` och `mattrix-chat`.

## Vad ändrades

Allt som ritar till och läser från en vanlig terminal ligger nu på ett ställe, utan matterm-typer i
gränssnittet. Det är tänkt att kunna bli ett litet bibliotek senare ("ett bättre ncurses": cellbuffer in,
valfri terminal ut, indata avkodad).

| Fil (`src/tty/`) | Innehåll |
|---|---|
| `color.h` | `Rgb`, `Caps` (truecolor / 256 / alternativskärm / unicode), färgreduktion till 256 och 16 |
| `canvas.h` | `Cell` (färg, attribut, terminalens egna färger, palettindex) och `Canvas`: ritar du om bara det som ändrats får du minsta möjliga ANSI-uppdatering. Hanterar dubbelbreda tecken (CJK, emoji) och nollbreda |
| `width.h` | `cellWidth()`: 0, 1 eller 2 kolumner |
| `screen.h` | `Screen`: en hel skärm. `enter()`/`leave()` (alternativskärm, mus, klistra in, fokus), markör (form, blink), `present()` ger bara skillnaden, `resize()`, `redraw()` |
| `keys.h` | `KeyDecoder`: tangenter, musrapporter (SGR), inklistring |
| `posix/terminal.h` | `detectCaps()`, `writeAll()`, `RawMode`, `windowSize()` |
| `posix/session.h` | `Session`: rått läge + skärm + storleksändring, och terminalen återställs vid vanlig exit, `exit()`, SIGINT/SIGTERM/SIGHUP, krasch (SIGSEGV/SIGABRT m.fl.) och ^Z / SIGCONT |
| `matterm/cells.h` | Den enda platsen som känner matterms `FChar`/`CursorState`: `drawCells()` och `toCursor()` |

Gamla namnen (`fakeos::AsciiCanvas`, `fakeos::KeyDecoder`, `fakeos::Rgb` …) finns kvar som alias, så
demons program och `fakeos_*`-testerna är oförändrade.

Medvetna val (ändra gärna):

- **Terminalens egna färger och palettindex går igenom.** En cell med "standardfärg" eller index 0–15 skickas
  som `39`/`49` resp. `30–37/90–97`, så telefonens/terminalens tema avgör och texten blir läsbar i vilket
  tema som helst. RGB skickas som RGB (reduceras om terminalen inte klarar det).
- **Inverterad, svag och dold text** skickas som attribut och bakas inte in i färgerna.
- **Terminal utan UTF-8** (t.ex. `LANG=C` över ssh): alla tecken över ASCII ritas som `?`.
- **Uppdateringar paketeras** i "synchronized output" (`?2026`) och markören döljs under ritningen, så det inte
  flimrar. Terminaler som inte kan det ignorerar koderna.
- Den matterm-modell som finns har ännu inga dubbelbreda celler (ett tecken per cell). Lagret hanterar dem
  när de kommer: tecknet tar två kolumner och nästa cell täcks. Idag ritas ett CJK-tecken därför över sin
  grannes plats.

## Så kontrollerar du det

1. `ctest --test-dir build -R 'tty|fakeos|demo_tty|mattrix'` (alla gröna).
2. `build/matterm-demo --tty` och `build/mattrix`: ska se ut som förut. Starta ett program på
   alternativskärmen (`fire`), avsluta med `q`: terminalen är som den var. Tryck ^C/^Z/`fg`.
3. `kill -TERM <pid>` på en körande `mattrix`: prompten kommer tillbaka med fungerande markör och echo.
4. Ändra fönstrets storlek medan `mattrix` kör: ritas om i nya storleken.

## Tester

- `tty_canvas`, `tty_keys`, `tty_width`, `tty_screen`: rena (celler in, byte ut; byten matas in i matterms
  egen `TerminalModel` och cellerna jämförs).
- `tty_posix`: förmågor, robust skrivning, rått läge på icke-terminal.
- `tty_matterm`: matterms cellbuffer → terminal → modell är identisk, inklusive indexerade/standardfärger.
- `tty_session`: under en riktig pty: alternativskärm, storleksändring, SIGTERM, SIGSEGV, abort, stopp och
  fortsätt.

## Inte verifierat

- Inga riktiga terminalprogram (xterm, Termux, tmux, Windows Terminal …) utom pty:n och matterms egen
  `TerminalModel` som mottagare. Särskilt: hur olika terminaler ritar dubbelbreda tecken och emoji, och att
  `?2026` verkligen tas emot rent.
- Ingen riktig telefon och ingen riktig sshd.
- Bredd-tabellen i `width.cpp` är en kompakt egen tabell, inte full Unicode (grafem-kluster, ZWJ-sekvenser
  räknas teckenvis).
- Windows: `src/tty/posix` är POSIX-bara; ren del och `matterm/` kompileras överallt men har inte provats där.
