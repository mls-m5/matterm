# agent2: avsluta en terminal från listan (daemonsidan)

Uppgift från integratören: ägaren vill kunna AVSLUTA en terminal från matmux-listan. Listan och tangenten är agent3:s.

## Vad som finns
- Nytt kontrollmeddelande `EndSession{id}` → `SessionEnded{id}` (additivt 1.3.4, `docs/PROTOCOL.md`).
- Daemonen avslutar programmet som när man stänger en panel: SIGHUP till processgruppen, sedan SIGKILL efter en kort frist
  (`terminationGrace`, 500 ms), och glömmer sessionen när programmet är borta (ingen replay, ingen gravsten). En klient som är kopplad
  får det vanliga "programmet avslutades" (`Exit`). En orörd (frånkopplad) terminal avslutas på samma sätt.
- Okänt id: `UnknownSession` ("no such session"); redan på väg att avslutas räknas som okänd. Bara kontrollanslutningen hos socketens
  ägare; webbanslutningar nekas som för `CreateTerminal`.
- Klientanrop: `frontend::endSession(socket, id, error)`. Mot en daemon äldre än 1.3.4 (svarar BadMessage): "this daemon is older than
  1.3.4 and cannot end a terminal from the list".
- Svaret kommer när begäran är godtagen, inte när processen lämnat: raden försvinner ur listan en kort stund senare.

## Tester (`daemon_end`, riktig daemon)
Kodek; vilande skal avslutas och försvinner; upptaget program (`exec sleep`) får SIGHUP och en ansluten klient ser `Exit`; ett program
som ignorerar SIGHUP dödas efter fristen; okänt id och dubbel avslutning; webbanslutning nekas och terminalen finns kvar; äldre daemon
(fejkad: svarar handskakningen och sedan BadMessage) ger rätt meddelande.

## Inte verifierat
Tillsammans med agent3:s lista (tangent, bekräftelse). Ingen riktig ssh/telefon.
