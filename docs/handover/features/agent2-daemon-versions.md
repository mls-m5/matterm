# agent2: daemonversioner, flera daemoner, `matterm-attach`, lista och stopp

Kontrakt: `docs/tasks/daemon-versioning.md` (inklusive tillägget om namn och flera daemoner). Teknik:
`docs/PROTOCOL.md` ("Control version"), `docs/REMOTE_DAEMON.md` ("Versions, several daemons, matterm-attach").

## Vad ändrades

- **Versionsnummer `format.breaking.additive+hash`** för daemonens kontrollprotokoll. Det ligger i
  Welcome-svarets textfält, så handskakningens bytes är oförändrade och gamla daemoner läses som
  breaking = sitt nummer (3), additive 0. Skiljer sig *breaking* vägrar klienten, förklarar och erbjuder en
  ny daemon bredvid den gamla (den gamla behåller sina terminaler). Skiljer sig *additive* blir det bara en
  notis. Hashen (git, 8 tecken) avgör aldrig något.
- **Flera daemoner samtidigt.** En daemon identifieras av sin socket. Nya daemoner lyssnar på
  `daemon-<format>.<breaking>.<additive>.sock` bredvid gamla `daemon.sock`. Fönstret och `--attach-tty`
  frågar alla sockets i mappen och tar den enda kompatibla; av flera den nyaste efter en fråga; finns ingen
  kompatibel erbjuds en ny. `--daemon-socket` gäller alltid. `matterm-daemon --ensure` återanvänder en
  kompatibel daemon oavsett socketnamn. `matterm-server` följer den nyaste socketen.
- **`matterm --daemon-list` / `--daemon-stop`** (och samma i `matterm-attach`): lista (socket, pid, version,
  hash, antal terminaler, om den passar) och stoppa efter fråga som nämner antalet terminaler. Stopp går via
  `Shutdown` (ingår i den frysta handskakningen, så det funkar för alla versioner); svarar daemonen inte
  tas pid från socketen (`SO_PEERCRED`, bara egen användare) och den får SIGTERM.
- **`matterm-attach`**: statisk fil utan SDL: `--attach-tty`-läget, `--daemon-list`, `--daemon-stop`,
  `--version`.
- **`--daemon-install`** kopierar nu både daemon och attach, med versionen i namnet
  (`matterm-daemon-1.3.1-abcd1234`), pekar de stabila namnen `matterm-daemon`/`matterm-attach` i mappen på dem,
  och tar bort gamla versioner: bara filer med exakt de namnen, aldrig en fil som körs, nyaste äldre
  versionen sparas, ingen fråga, det som togs bort skrivs ut. Med `--daemon-host` kör
  `--daemon-list/--daemon-stop` `matterm-attach` på andra datorn (kopieras dit först vid behov).
- Palettposten "Stop daemon..." finns redan och använder `Shutdown`; jag ändrade den inte.

## Så kontrollerar du det

1. `build/matterm-attach --version`; `build/matterm-daemon --version`.
2. Starta `build/matterm-daemon --daemonize`, sedan `build/matterm --daemon-list` och
   `build/matterm --daemon-stop` (svara y).
3. Lokal version-konflikt: starta en daemon, kör `matterm` från ett bygge med annat breaking-nummer
   (testerna fejkar detta: `daemon_version`, `daemon_choice`).
4. `ctest --test-dir build -R 'daemon|tty|ssh|matterm_attach'`.

## Frågor till ägaren (standardval i parentes, lätta att ändra)

1. Offerten ("starta en ny daemon bredvid?") görs bara på denna dator och bara om det finns en terminal att
   fråga i (fönstret startat från en terminal). Annars: tydligt felmeddelande. Över `--daemon-host`
   erbjuds inget, meddelandet pekar på list/stop. (Alternativ: fråga även via en dialogruta i fönstret.)
2. Gamla filer som döpts efter innehåll (före detta schema) rörs inte vid städning. (Alternativ: städa dem också.)
3. Hashen tas när `cmake` körs (inte vid varje bygge); ett bygge utan git visar `unknown`.

## Inte verifierat

- Riktig ssh/sshd och riktig telefon: bara `test/fakessh.py`.
- Byggen med `MATTERM_STATIC_DAEMON=OFF` och icke-Linux.
- Ingen riktig äldre daemon-binär (version 1 eller 2) kördes; de fejkas via `DaemonConfig::version`.
- Webbygget påverkas inte men har inte kompilerats här.
