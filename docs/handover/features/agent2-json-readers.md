# agent2: tre JSON-läsare, krav som test och en tabell (inget är migrerat)

Uppgift: `docs/tasks/json-readers.md`. Testet: `test/jsonreaders_test.cpp` (ett gränssnitt, en adapter per läsare, varje fall körs i en egen
process så att krasch eller hängning är ett resultat). Läsarna: **jsonvalue** (`src/frontend/jsonvalue.h`), **snapshot** (`Reader` i
`src/frontend/sessionsnapshot.h`, nås med en tillfällig `#define private public`), **fast-json** (submodulen, via en tillfällig adapter; inte ändrad).
Testet är rött bara om jsonvalue inte klarar allt; tabellen skrivs ut vid körning (`ctest -R json_readers -V`).

## Tabellen (pass / FAIL / n/a = läsaren har ingen skrivare)

| fall | krav | jsonvalue | snapshot | fast-json |
|---|---|---|---|---|
| R01 | giltiga dokument (inkl. true/false/null) | pass | FAIL (ingen bool/null) | pass |
| R02 | trasig syntax | pass | pass | pass |
| R03 | efterställt komma refuseras | pass | pass | FAIL (tillåts med avsikt) |
| R04 | dubbel nyckel refuseras | pass | pass | FAIL |
| R05 | tom indata | pass | pass | pass |
| R06 | text efter dokumentet | pass | pass | pass |
| R07 | dåliga tal (01, +1, .5, 1., NaN, hex) | pass | FAIL (godtar t.ex. 01) | pass |
| R08 | kommentarer | pass | pass | pass |
| R09 | ogiltig UTF-8 (strö-byte, overlong, trunkerad, surrogat) | pass | pass | FAIL (strängar lämnas råa) |
| R10 | giltig UTF-8 kommer tillbaka | pass | pass | pass |
| R11 | ensamt surrogat-escape | pass | pass | FAIL |
| R12 | surrogatpar-escape avkodas | pass | FAIL (refuseras med avsikt) | FAIL (`\u` stöds inte i `str()`) |
| R13 | escapes avkodas | pass | pass | FAIL (`å`) |
| R14 | okända/trasiga escapes refuseras | pass | pass | FAIL |
| R15 | rå kontrolltecken i sträng | pass | pass | FAIL |
| R16 | NUL-byte i texten | pass | pass | FAIL |
| R17 | nästling 20 | pass | pass | pass |
| R18 | nästling 5000 utan krasch | pass | pass | FAIL (ingen gräns) |
| R19 | dokument > 2 MiB refuseras | pass | FAIL (storleksgränsen sitter i `SessionSnapshot::parse`, inte i läsaren som adaptern når) | FAIL |
| R20 | 20000 medlemmar accepteras | pass | FAIL (gräns 20000 värden) | pass |
| R21 | medlemmarnas ordning | pass | pass | pass |
| R22 | kan skriva tillbaka | pass | n/a | pass |
| R23 | skriv + läs = samma | pass | n/a | pass |
| R24 | okända medlemmar överlever | pass | n/a | pass |
| R25 | talens text bevaras (1.0, 1e3, -0, 20 siffror) | pass | n/a | pass |
| R26 | flyttal och stora heltal accepteras | pass | FAIL (bara heltal) | pass |
| R27 | strängar med citat/backslash/kontrolltecken skrivs rätt | pass | n/a | FAIL (`dump` citerar den redan escapade råsträngen igen) |
| R28 | nyckel och värde med escapes läses lika | pass | pass | FAIL (nycklar jämförs råa) |
| | **summa** | **28/28** | **17/23** | **15/28** |

(Siffrorna är de som testet skriver ut; R29 "roten måste vara ett objekt" är en regel för inställningarna, inte för en läsare, och ligger i `settings_test`.)

## Vad som skulle krävas för att fast-json klarar resten
- **Läsning/validering** (R03, R04, R09, R11, R14, R15, R16, R18, R19): ändringar i `validate()` i biblioteket (strikt läge: inget efterställt komma, dubbla
  nycklar, UTF-8-kontroll, kontrolltecken/NUL, djup- och storleksgräns). Små men det är ett ändrat bibliotek (ägaren kan göra det uppströms),
  eller ett tunt lager här som validerar texten innan `parse_json` (men då skrivs en strikt läsare ändå).
- **Avkodning** (R12, R13, R28): `str()` måste kunna `\uXXXX` och surrogatpar, och nycklar måste jämföras avkodade.
- **Skrivning** (R27 + att behålla okända medlemmar vid ändring av ett värde): biblioteket läser ett skrivskyddat DOM och skriver med `JsonOut` (som inte
  tar ett parsat dokument); `dump()` är en debug-utskrift med ett citeringsfel. Ett redigerbart DOM med skrivare är ny kod i biblioteket.
- **Byggkrav**: fast-json kräver C++20; klientkoden (fönstret, matmux, `src/tty`) är C++17. Och bibliotekets noder är vyer in i indatatexten, som måste leva
  lika länge som dokumentet (en vanlig fälla; adaptern i testet hade den först).

## Beslutsfrågan (till ägaren, via integratören)
- **A.** `jsonvalue.h` blir den enda läsaren på klientsidan och `sessionsnapshot` flyttar till den (tar bort ~150 rader i `SessionSnapshot::Reader`; sessionsfilerna
  får bool/null/flyttal-stöd gratis men behåller sin schemakontroll). Servern behåller fast-json för config/auth: två läsare kvar, men olika program med olika krav.
- **B.** Allt på fast-json, utökat uppströms (strikt läge, `\u`, redigerbart DOM med skrivare, C++17-kompatibelt eller klienten till C++20). Då finns en läsare,
  men det är mest arbete och rör ett bibliotek som servern litar på.
- **C.** Låt det vara.
**Rekommendation: A nu** (det är klart, testat mot alla 28 krav och tar bort en dubblett i klientkoden); **B som uppföljning** om ägaren vill ha ett enda bibliotek: då kan
kravtestet här skickas uppströms som specifikation och `jsonvalue.h`:s regler bli patchen. Inget migreras före beslut.
