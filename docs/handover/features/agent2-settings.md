# agent2: inställningsfil, teman och datornamn (fönstrets del)

Uppgift: `docs/tasks/settings.md`, delen agent2. Texten och tabellerna: `docs/TABS_PLAN.md` avsnitt 17. TUI:n (`--settings`) är agent3:s.

## Vad som finns
- **`settings.json`** (`$XDG_CONFIG_HOME/matterm/`, annars `~/.config/matterm/`): strikt JSON, atomisk skrivning (tempfil + rename, 0600,
  mappar 0700), okända nycklar bevaras, trasig fil rapporteras och de gamla värdena ligger kvar, värden som inte passar blir varningar
  och standardvärdet används. Nycklar: `theme`, `computers`, `tabActivity`, `windowFrame`, `scrollbackRows`, `confirmClose`.
- **Fönstret följer filen**: titt ungefär varje sekund (en `stat`), omläsning när tid eller storlek ändrats; paletten har "Reload settings".
  Fel visas i fönstertiteln. Kommandoraden vinner där den angivits (tab-activity, frame).
- **Teman** (bara mörka): Grey (som idag), Blue, Green, Red, Amber. Ramen (tabbraden, kanten) får temats färg och terminalernas
  bakgrund en svag ton (5 %); programmens färger rörs inte. Temana skiljer sig bara i nyans (ungefär greys ljusstyrka); testerna kräver
  nyanser med avstånd, läsbar text (kontrast) och mörk ram.
- **Tema per dator**: lokalt = användarens tema; `--daemon-host NAMN` = override i `computers`, annars automatiskt av namnet (samma namn ger alltid
  samma tema, aldrig det lokala). **Datorns namn skrivs som dämpad text i tabbradens lucka** (mellan sista fliken och Dela-knappen), inte i flikens namn; placeringen är en liten
  funktion (`placeComputerName`).

## Så provar du
1. Skriv `{"theme":"blue"}` i `~/.config/matterm/settings.json`, starta matterm: blå ram. Ändra filen medan fönstret kör: ändras inom ca 1 s.
2. `matterm --daemon-host server`: andra temat och "server" i tabbradens lucka. `{"computers":{"server":"amber"}}` byter.
3. Skriv trasig JSON: titeln säger det, färgerna ligger kvar. "Reload settings" i paletten läser direkt.
4. `ctest -R 'json_value|settings|themes|workspace'`.

## Frågor till ägaren (standardval, lätta att ändra)
1. Färgerna (RGB för varje tema) är mina förslag: jag har inte kunnat titta på en skärm. Titta på dem och säg till.
(Beslutat: automatiken är en hash av namnet och krockar får finnas; `windowFrame`/`scrollbackRows` gäller vid nästa start/nya terminaler.)

## Inte verifierat
Hur det ser ut på en riktig skärm (ingen här); bara att teman, kontrast och ljusstyrkesteg stämmer i tester, och att fönstret startar,
följer filen och avslutas rent under SDL:s dummy-drivrutin. Ingen `--daemon-host` med riktig ssh; temat per dator testas på
regelnivå och med `workspaceview_test` (text i flik, ramfärg).
