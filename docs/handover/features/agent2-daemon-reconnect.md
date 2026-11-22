# agent2: återanslutning för `--daemon-host`

Uppgift: `docs/tasks/daemon-reconnect.md`. Detaljer och teknik: `docs/REMOTE_DAEMON.md`, avsnittet "Reconnect".

## Vad ändrades

När ssh-tunneln till en fjärr-daemon dör (nätverk, laptop i viloläge) stängs inte panelerna längre.
Fönstret startar om tunneln och kopplar tillbaka varje panel till sin terminal.

- Panelerna behåller sin skärm och blir "disconnected" medan tunneln är nere.
- Fönstret försöker själv igen: direkt, efter 1, 2, 5, 10 s och sedan var 30:e s, högst 8 försök.
- När tunneln svarar kopplas varje avkopplad panel tillbaka med sitt sessions-id och sin replay-offset.
  Daemonen spelar upp exakt det som skrevs under tiden, inget saknas och inget dubbleras
  (testat: 80 numrerade rader kommer en gång vardera, i ordning).
- Efter sista misslyckade försöket skriver fönstret varför, panelerna säger
  "disconnected from HOST, press Enter to retry", och inget mer försöks av sig självt.
- Allt sker på en egen tråd, så fönstret fryser inte (testat: `tick()` tar under 200 ms även med en
  ssh som tar 1,5 s per anrop). Stänger man fönstret mitt i ett försök avbryts ssh-anropet direkt.

## Hur det används

- Automatiskt, inget att göra.
- Manuellt: kommandopaletten, posten **Reconnect** (visas bara med `--daemon-host`), eller **Enter** i en
  avkopplad panel. Det räknar om från noll och byter också ut en ssh som lever men hänger sig.
- En panel som en annan fönster tagit över ("Replaced") rörs inte.

## Så kontrollerar du det

Kräver en riktig maskin med sshd, vilket jag inte hade:

1. `matterm --daemon-host dinhost`, kör något som skriver kontinuerligt, t.ex.
   `i=0; while true; do echo L$i; i=$((i+1)); sleep 1; done`.
2. Hitta ssh-processen med `-N -L` (`pgrep -af 'ssh.*-N.*-L'`) och döda den (`kill -9 PID`), eller dra ut
   nätverkssladden en stund.
3. Förväntat: en notis "The connection to dinhost ended ... Reconnecting", sedan "Connected to dinhost
   again", panelen fortsätter där den var och det som skrevs under uppehållet fylls på utan hål.
4. Gör det med nätverket avstängt längre än ca 2 minuter: efter sista försöket ska panelen säga
   "press Enter to retry". Slå på nätverket, tryck Enter eller välj Reconnect.

Automatiska tester: `ctest --test-dir build -R 'ssh_tunnel|daemon_commands'` (fem nya fall mot `test/fakessh.py`).

## Beslut jag tog själv, som du kan vilja ändra

Jag hade inte regeln om att fråga först när jag byggde detta; alla går att ändra på ett ställe
(`ReconnectPolicy` i `src/frontend/sshtunnel.h` och `app/ptyfrontend.cpp`):

1. **Backoff och tak**: 0/1/2/5/10/30 s, 8 försök (cirka 2 minuter) innan den ger upp. Alternativ: för alltid
   var 30:e s (aldrig upp), eller färre försök. Rekommendation: nuvarande, eftersom en tyst fönster som aldrig
   slutar försöka är svår att förstå.
2. **Panelerna kopplas tillbaka automatiskt** när tunneln är uppe, utan Enter. Alternativ: kräva Enter.
   Rekommendation: automatiskt, det var syftet.
3. **Enter i en panel som inte kan koppla upp trots att tunneln ser frisk ut** startar om ssh. Alternativ: bara
   försöka koppla igen. Rekommendation: nuvarande (annars går en hängd tunnel inte att rädda utan palett).

## Inte verifierat

- **Ingen riktig sshd eller nätverksavbrott.** Allt testades mot `test/fakessh.py`, som relayar Unix-sockets.
- **Ingen riktig skärm.** Det riktiga programmet kördes headless (dummy-drivrutin) med en fake-ssh i PATH:
  när forward-ssh dödades startade fönstret en ny och avslutade rent. Notiserna, texten i panelen,
  Enter-hanteringen och paletten syns inte där och är aldrig sedda.
- Ett ssh som hänger utan att dö upptäcks först när `ServerAliveInterval` (15 s x 3) dödar det, eller när du
  trycker Enter/Reconnect.
- `clang-tidy` kördes inte. `ctest` (103 tester) är grönt; det finns ingen browser-e2e här (kördes inte).
