# agent2: daemonsidan av "matmux som en liten tmux"

Kontrakt: `docs/tasks/attach-new-session.md` (avsnittet Split, agent2). Själva läget (lista med "New terminal",
`--new`, vägran att koppla till den egna terminalen) är agent3:s.

## Vad ändrades

- **`MATTERM_SESSION=<id>`** sätts i miljön för varje terminal som daemonen startar (också de som skapas
  av en vanlig Hello). Ett program som kopplar upp sig kan jämföra med listans id och vägra sin egen
  terminal (annars matar utdata sig själv). Test: `daemon_create`.
- **`CreateTerminal`** (nytt kontrollmeddelande, additivt 1.3.3): storlek, mapp, kommando (tomt = användarens
  skal) och namn. Miljön väljs fortfarande aldrig av klienten. Kontrollsocketen tillhör en användare och
  den användaren kan redan köra vad som helst, så kommandot ger inget nytt; webbanslutningar kan inte
  skicka det. Kommando som inte startar ger `StartFailed`. Klientanropet är
  `frontend::createTerminal(socket, request, error)`; mot en äldre daemon utan kommando/namn faller det
  tillbaka på `CreateSession`.
- **Daemon startas eller återanvänds** av attach-klienten med `selectDaemon(options, remote, true)`
  (finns sedan förra uppgiften; samma val som fönstret gör).
- **Bygghashen får `-dirty`** när spårade filer har ändringar (`abcd1234-dirty`); tas när cmake körs. Ett
  sådant bygge installeras under sitt innehålls-hash i stället (namnet ska säga vad som finns i filen).

## Så kontrollerar du det

1. `ctest --test-dir build -R 'daemon_create|daemon_version|ssh_tunnel'`.
2. `build/matmux --version` visar `-dirty` om du har osparade ändringar.
3. Inuti en daemon-terminal: `echo $MATTERM_SESSION` ger terminalens id (`--daemon-list`/listan visar samma).

## Inte verifierat

- Inget av agent3:s läge (list, `--new`) är sett tillsammans med detta; bara daemonsidan är provad.
- Ingen riktig telefon eller sshd. Äldre daemon (före 1.3.3) fejkas inte i testet av reservvägen.
