# agent2: replay, steg 2: skicka i klump, rita sällan

Uppgift: `docs/tasks/replay-performance.md`. Före/efter-tabell: `docs/CPU.md`.

## Vad ändrades
- **Daemonen** skickar en reprisering upp till fönstret (256 KiB) per pump i stället för ett meddelande. Webbservern delar koden
  och får samma vinst. Test: `server_registry` ("A replay is sent up to the window at once").
- **Fönstret och matmux** ritar högst var 100:e ms medan mer än 32 KiB utdata kommer per varv (en reprisering, `cat` av en stor
  fil). Tangent, storleksändring och expose ritar direkt. Modellen har hela innehållet; bilden efter en flod är densamma.
  Test: `tty_attach` (två nya fall). Fönstret: ingen enhetstest (SDL-slingan), mäts med `bench/replay-measure.py`.

## Resultat
Reprisering av 3,2 MiB: 2,07 s till 0,17 s i benchen; fönstret 217 till 13 bilder och 1,17 till 0,17 s CPU på 3 s.

## Så provar du
`ctest -R 'server_registry|tty_attach'`; koppla upp mot en terminal med mycket historik och se att den är klar på en bråkdels
sekund i stället för att rulla.

## Inte verifierat
Riktig skärm/GPU och riktig ssh. Ritgränsen 100 ms är ett val (ändra `kFloodDrawMs` / "flooded"): under en flod syns alltså
högst tio bilder i sekunden.
