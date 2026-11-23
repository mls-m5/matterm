# agent2: matmux passthrough, ögonblicksbilden (snapshot) och återställningen

Uppgift: `docs/tasks/matmux-passthrough.md`, delen agent2. Läget i matmux själv (levande slinga, ingångar, `--rendered`) är agent3:s.

## Vad som finns
`src/tty/matterm/snapshot.h`:
- `tty::snapshot(modell, lägen, alternativ)` → bytes att skriva till värdterminalen vid attach: modellens scrollback som vanliga rader
  (de hamnar i värdens scrollback), sedan skärmen som exakt `rows` rader (så att det synliga området blir lika, med samma
  radjustering, oavsett vad värden visade förut: raderna rensas till radslut), sedan rullningsområde, origin-läge, markör (plats, form,
  synlighet) och de inmatningslägen programmet bett om (applikationsmarkör/-knappsats, bracketed paste, fokusrapporter, mus: spårning och kodning).
  Ligger programmet i alternativskärmen: värden går in i den och skärmen ritas där (ingen scrollback).
  Mjuka radbrytningar behålls (raden skrivs utan radslut), så värdens egen omflödning av långa rader fungerar.
- `tty::hostRestore(iAlternativskärm)` → bytes för att sätta tillbaka värden: lämna alternativskärmen, stäng av alla rapporter
  (mus i alla former, paste, fokus, applikationsmarkör/-knappsats), nollställ rullningsområde, origin, teckenuppsättning, SGR och
  markörform, visa markören, ny rad.
- `TerminalModel::historyCell/historyRowWrapped`: åtkomst till de sparade raderna (behövdes).

## Tester (`tty_snapshot`)
Programmets utdata matas genom en modell, snapshoten skrivs in i en andra modell och de måste vara lika: skärm, markör, sparade rader
(inklusive mjuk radbrytning), rullningsområde, origin, lägen, alternativskärm. Fall: skal-session med historia, färger/attribut av alla
slag (indexerade, RGB, standard, färgade blanka), markör, långa rader, rullningsområde + origin, alla musläggen/kodningar, alternativskärm,
en värd som visade annat förut, breda tecken, utan/med gräns på scrollback, tom och enradig terminal, återställningen. Plus ett
slumptest (seedat; 2000 omgångar kördes, 150 i testsviten) med blandad utdata inklusive alternativskärm.

## Kända gränser (också i headern)
Modellen känner inte pennan (aktuell SGR), tabbstopp, teckenuppsättningar, autowrap-läge eller väntande radbrytning i sista kolumnen:
efter snapshoten gäller värdens standard tills programmet sätter om dem. Modellen har inga breda tecken (en kodpunkt per cell), så rader
med dem skrivs som modellen håller dem. Programmet i alternativskärmen: primärskärmen bakom återställs inte (efter att programmet lämnat
visar värden det den visade före attach). Skrivs med fulla färger/index, utan reduktion till värdens förmåga (programmet skriver likadant).

## Inte verifierat
Ingen riktig värdterminal (GNOME Terminal, Termux): bara matterms egen modell som mottagare. Ingen pty-test av snapshoten ensam; den
hänger ihop med agent3:s läge (tid till första bild mäts där).

## Mätt
`matterm-replay-bench --mib 4` (Release): replay 3,2 MiB på 0,13 s, snapshot av modellen med 10000 sparade rader + skärm: 813 KB på 16 ms.
Alltså är tid till första bild vid attach i praktiken replay-tiden (se `docs/CPU.md`), inte snapshoten.
