# agent2: replay, steg 1: mätning

Uppgift: `docs/tasks/replay-performance.md`. Siffror och metod står i `docs/CPU.md` (avsnittet "Replay").

## Vad jag gjorde
- `matterm-replay-bench` (`bench/replaybench.cpp`, byggs med `-DMATTERM_BUILD_BENCHMARKS=ON`): daemon i processen, en terminal
  som skrivit N MiB (ringen full), en klient som kopplar upp sig som fönstret och matmux gör; delar upp tiden i klient (transport,
  parsning, modell) och väntan; `--restore K` mäter K paneler som startas och kopplas upp.
- `bench/replay-measure.py BYGGMAPP --mib N`: själva fönstret, utan skärm (dummy-drivrutin), första panelen kopplar upp mot en
  sådan terminal (dold miljövariabel `MATTERM_ATTACH_TERMINAL`); CPU-tid, ramar och varv (`MATTERM_LOOP_STATS`).

## Resultat (före)
Klienten är inte flaskhalsen (parsning 0,05 s för 3,2 MiB). Daemonen skickade en reprisering som ett meddelande per 10 ms
(16 KiB, alltså 1,6 MiB/s) och fönstret ritade en bild per bit (217 bilder, 1,2 s CPU på 3 s). Att återställa många paneler kostar
mest att starta skalen (2 ms per panel mot daemonen, resten sker parallellt), inte repriseringen.

## Inte verifierat
Siffrorna är från en maskin utan riktig skärm (mjukvaru-renderare, `SDL_VIDEODRIVER=dummy`) och utan riktig ssh; `--daemon-host`
mättes inte separat (fakessh ger bara relativa tal och bytena går samma väg).
