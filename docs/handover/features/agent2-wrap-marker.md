# agent2: markör för radbrutna rader

Kontrakt: `docs/tasks/wrap-marker.md`. Teknik: `docs/SCROLLING_AND_REFLOW.md`, avsnittet "Wrapped-line marker".

## Vad ändrades

En rad som fortsätter på nästa (en lång rad som bröts, eller som reflow lagt om) får en liten fylld
**triangel** (4 px bred, 40 % av radhöjden, vid radens mitt) i marginalen till höger om texten. Rader som
tar slut där får ingen. Ingen cell täcks och inget i rutnätet eller layouten ändras.

- Native-fönstret och webbklienten ritar samma sak (de delar `SdlBackend`).
- **Fönstrets kant** (en ensam panel eller panelen längst till höger): scrollistens remsa är 12 px. Markören
  ligger i dess vänstra del och rullningslistens tumme i den högra, **4 px bred i stället för 8**.
- **Panel intill en avdelare**: rullningslisten ligger kvar i närmare halvan av avdelaren, markören i den
  bortre halvan.
- Plats saknas (för smal remsa/avdelare, panel utan remsa): ritas inte.
- Inga markörer på alternativ-skärmen (vim, less, htop ...).
- Färgen är rullningslistens tumme i vila (110,122,142), "dämpad förgrund".
- Ingen ny flagga eller inställning (det finns ingen config-fil).

## Så kontrollerar du det

1. `matterm`, skriv något långt: `printf 'x%.0s' $(seq 1 300)` i ett fönster med några tiotal kolumner.
   En triangel ska synas till höger om varje bruten rad utom den sista.
2. Gör fönstret smalare och bredare: raderna läggs om (reflow) och triangeln följer med de rader som
   fortsätter; den får inte flimra eller försvinna beroende på exakt bredd.
3. Dela panelen (Ctrl+Shift+D eller via paletten) och titta på mellanrummet: rullningslist till vänster
   i avdelaren, triangel till höger i den.
4. Starta `vim` eller `less`: inga trianglar.
5. Tester: `ctest --test-dir build -R 'wrap_mark|sdl_backend|frame_signature|terminal_model'`.

## Frågor till ägaren (jag valde ett förslag och gjorde det lätt att byta)

Utseendet är konstanter i `struct Style` i `src/render/wrapmark.h`; byte av något av dem är en rad.

1. **Form och riktning.** Nu: triangel som pekar åt höger (▸, "raden fortsätter"). Alternativ: pekar nedåt
   (▾); eller liten böjd pil (↵ som tecken i stället för fylld form, kräver glyf i fonten). Rekommendation:
   nuvarande.
2. **Storlek.** Nu 4 px bred, 40 % av radhöjden. Alternativ: större (6 px, 50 %) om den är svår att se på en
   hög-DPI-skärm. Rekommendation: nuvarande, justera efter att du sett den.
3. **Färg.** Nu samma som rullningslistens tumme. Alternativ: ännu svagare (nära bakgrunden) eller en
   accentfärg. Rekommendation: nuvarande.
4. **Rullningslistens tumme blev smalare (8 -> 4 px)** när markören delar remsan. Alternativ: göra remsan
   bredare (ändrar layouten, 12 -> 16 px, påverkar kolumnantalet), eller lägga markören över sista cellen
   (täcker tecknet, avråder jag från). Rekommendation: nuvarande.
5. **Av/på.** Ingen inställning ännu (ingen config-fil). Alternativ: flagga `--no-wrap-marker`, eller
   vänta på inställningsvyn. Rekommendation: vänta; säg till om du vill ha flaggan nu.

## Kostnad (docs/CPU.md-metoden)

Release-bygge, headless, `bench/cpu-measure.sh` med `bench/claudelike.py` (innehåller bruten text),
före = main 111f959 + mitt kontrakt, efter = denna commit. Fyra mätningar per rad vid 30 uppdateringar/s,
en vid vila och 10/s:

| last | före | efter |
|---|---|---|
| vila (efter blinktimeout) | 0,8 % | 0,8 % |
| 10 uppdateringar/s | 5,5 % | 5,7 % |
| 30 uppdateringar/s | 14,5 % (12,7-15,3) | 15,4 % (14,0-16,8) |

Skillnaden (cirka en procentenhet vid 30/s) ligger inom mätbruset. Ingen layoutändring i antal kolumner
eller rader. Vilande fönster ritar fortfarande ingenting (markörerna ingår i bildsignaturen).

## Inte verifierat

- **Aldrig sett på en riktig skärm eller i en webbläsare.** Pixeltesterna (mjukvaru-renderare) kontrollerar
  var markören hamnar och att inga celler ändras, inte hur den ser ut på en hög-DPI-skärm.
- **`web/main.cpp` ändrades med en rad men webbygget kördes inte** (ingen emscripten här, ingen browser-e2e).
  Raden är `rendered.wrapMarks = model.viewWrapMarks();`, samma som i native.
- Med SDL:s grafikdrivrutiner (OpenGL, Wayland) ritas triangeln med `SDL_RenderGeometry`; testad bara med
  mjukvaru-renderaren. Kantutjämning av triangeln finns inte.
- Kostnaden mättes med dummy-drivrutinen och mjukvaru-renderaren, som tidigare CPU-siffror.
