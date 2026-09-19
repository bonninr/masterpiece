# Recital 2: "New tested instruments" (handoff)

Goal: one video. Logo intro with the subtitle "New tested instruments", then 20-30 s of each piece with a caption (work, composer, organ), crossfading between them.

1. Build the app from main at or after the pallet-switch release (`build-scripts/win-build.ps1 Masterpiece`, with `MP_DEPS_DIR=build/win/_deps`, so JUCE isn't downloaded again).
2. Record the takes with `powershell -File build/recital-new.ps1`. Each take:
   - loads with `--preload-drawn --draw-stops <ids> --play-midi --record-audio`;
   - captures the console with PrintWindow on the app's own window handle, so no screen guessing and the machine stays usable;
   - writes `<Organ>.wav/.png/.log` to `build/recitals-new/`.

   Stop lists come from `playable.py`, because demo sets sample only some stops.
3. Optionally add the organs the pallet fix made playable: Alessandria_Demo, ErfurtPredigerkirche_Demo, SwietaLipka_Demo, Goch_Demo in `D:\organs`.
   - Their stops have no StopRank, so pick stops by name (look for "Principal", "Octave" and similar).
   - Draw at least one stop per manual.
   - Check that the `.wav` is not silent (peak above -40 dBFS).
4. Intro: `python build/make-recital-intro.py`, with the subtitle "New tested instruments".
5. Compile with `python build/compile-recital.py`, pointed at `build/recitals-new`:
   - 25 s clips (start after the first 2 s);
   - 1.5 s crossfades;
   - caption text from the PROGRAMME metadata: piece, composer, organ, place.
6. Check by listening to every clip. Reject any take that is silent, clipped, or drawn on only one manual.

Piece choices (MIDI from `build/midi`):

| Organ | Piece |
|---|---|
| Giubiasco | zipoli_toccata |
| Bégard | boellmann_suite |
| Chorzów | brahms_pf_gm |
| Erfurt Büßleben | pachelbel_theme |
| Nancy | franck_chorale3 |
| Nitra | bux_pf_gm |
| Obervellach | mozart_f |
| Oloron | grison_toccata |

Rules:
- No emoji, and no AI or tool attribution anywhere.
- Name Hauptwerk only in compatibility statements.
