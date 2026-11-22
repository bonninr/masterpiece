# Use cases

Each entry: a request as a player would write it, then the means. `api` is
`scripts/mp_api.py`; `BASE` is `http://127.0.0.1:8080/api/v1`. Square
brackets mark what the player fills in.

## Build and code

**"Build a local copy of Masterpiece."**
Clone, check out `dev`, configure and build `dev-fast` (`source-and-prs.md`).
Report the program's path and start it once to confirm it opens.

**"Add [feature]. Create a pull request so it gets added."**
Read the code around the feature, branch from `dev`, change it, add a test,
update the manual page and the translations, build, run `mp_tests --no-perf`,
push to a fork, open the pull request against `dev` (`source-and-prs.md`).

**"Fix issue #[n] and send the fix."**
Read the issue and its comments, reproduce it (command line or API), write a
test that fails, fix, test, pull request with "Fixes #[n]".

**"Translate Masterpiece into [language]."**
Copy `resources/translations/strings.txt` to `<code>.txt`, translate every
line, keep the `%1` placeholders, run `python tools/i18n.py --check`, pull
request.

## MIDI consoles

**"I connected a Johannus [model] to the computer. Download its MIDI manual
and configure all the MIDI elements."**
1. Find the console's MIDI implementation chart (the maker's site or manual):
   the channel of each manual and the pedal, and the message of each stop,
   coupler, piston and shoe.
2. List the organ's objects: `api names switches`, `api list controls --all`,
   `api list keyboards`. Note each `odfId`.
3. Pair each console element with an organ object by name and function.
   Report the pairs that have no counterpart on either side.
4. Close Masterpiece, back up `midi/<organ>.mpmidi`, write the `manual`,
   binding and `send` lines (`settings-files.md`).
5. Start with `--log-midi`, ask the player to move each element, and read in
   the log what each message reached.

**"Map my console's pistons to the generals."**
As above, with `switch` targets for the organ's pistons, or the console file
`midi/console.mpmidi` for the player's own generals and sequencer.

**"Note [n] of the Swell does not sound."**
Start with `--log-midi`, play the note, read why it reached nothing: channel,
key range, a stop not drawn, a missing sample.

## Audio outputs

**"Load Alessandria and configure all perspectives to my multi-output sound
card."**
1. Count the device's outputs (the audio device in the window, or the player).
2. Close Masterpiece, back up `settings.mpglobal`, write one output per
   perspective in `buses` and name each with `busname`.
3. Load the organ, list the ranks (`api list ranks --all`), group them by
   perspective (the rank names and the windchest groups carry it), and route
   each group with `PUT BASE/routes/{rank}/value`.
4. Check with `api dump routes` and a short take per output.

**"Send all the pedal ranks to output 5 of my sound card, with a low-pass that
follows the spec of the Yamaha [subwoofer model]."**
1. Read the subwoofer's spec: its crossover frequency and slope, or its
   frequency range.
2. Close Masterpiece, back up `settings.mpglobal`, add output 5 on the
   device's channel in `buses`, and a `busfilt` line with the low-pass:
   frequency from the spec, Linkwitz-Riley 24 dB unless the spec says
   otherwise.
3. Start, load the organ, route every rank of the Pedal division to output 5.
4. Render a pedal scale and check that the content above the crossover
   frequency falls off (`analyze_audio.py`).

**"Set up two speakers and a subwoofer with a crossover at [Hz]."**
Outputs 1 (stereo) and 2 (mono, the subwoofer's channel), output 1 feeding
output 2 at +6 dB, high-pass on 1 and low-pass on 2 at the same frequency,
Linkwitz-Riley 24 dB, delay on the nearer one. The manual's audio page
(`manual/src/guide/audio.md`, Output routing) gives the full recipe.

## Voicing

**"Adjust the reeds of Bückeburg: make them darker."**
1. Load the organ; list the ranks and pick the reeds by name (Trompete,
   Posaune, Fagott, Dulzian, Krummhorn, Regal, Schalmei, Vox humana, Oboe…).
   Show the list to the player before changing anything.
2. Dump `brightnesses` and `ranks` as the undo.
3. Lower each reed's high shelf: `PUT BASE/brightnesses/{rank}/value` to −30
   (−3 dB) as a first step.
4. Render a short passage on each reed before and after; report the treble
   share and the level. Darker reeds also sound softer; offer a level
   correction of the same size on `ranks`.

**"Make the Principal chorus of the Great more even in loudness."**
Render each rank of the division alone on the same chromatic scale, measure
the level, set `ranks` levels towards the median, render again, report.

**"Find the pipes of [stop] that are out of line."**
Render a chromatic scale on the stop alone, split it per note
(`analyze_audio.py --segments N`), and list the notes more than 6 dB off the
median. Trim them with `pipes`.

**"Which pipes of [rank] are out of tune with their neighbours?"**
Describe the rank's pipes for `tuning`; list the outliers.

**"Spread the Swell wider and move it to the left."**
`pans` and `widths` for the Swell division (one per division after the ranks).

**"Save this voicing, so I can return to it."**
Dump `ranks`, `pipes`, `brightnesses`, `basses`, `pans`, `widths` and `routes`
to a JSON file with the organ's name. Restoring replays the values.

## Registration and playing

**"Prepare the registrations for [piece]: [list]. Put them on generals 1 to
4."**
For each registration: general cancel (`POST BASE/commands/1/press`), draw the
stops (`PUT BASE/switches/{i}/value`), setter on (`PUT BASE/program/7` 1),
press the general, setter off. Check with each general's `holds`.

**"Build a sequence of [n] registrations for the service."**
As above, with the sequencer frames: setter on, `PUT BASE/program/6` to the
frame, draw, store.

**"Play this MIDI file with the plenum and record it."**
`Masterpiece --odf ORGAN --registration plenum --play-midi piece.mid
--record-audio piece.wav`.

**"Record the same piece in Werckmeister III and in equal temperament and tell
me the difference."**
Two takes, a temperament each (`POST BASE/temperaments/{i}/press`), then
compare the renders.

## Loading and memory

**"The organ does not fit in memory. Make it load."**
Read the free memory and the organ's size, then propose
loading options in order: int16 storage, streamed releases, 48 kHz, mono, and
perspectives left out. Apply the player's choice in Organ settings or on the
command line.

**"Why does the sound break up?"**
Read program property `lateBlocks` while playing, the audio buffer size, and
`soundingVoices` against `polyphony`. Report which limit is hit.

## Reports and evidence

**"Masterpiece crashed. Collect what the developers need."**
Copy `masterpiece.log` and `masterpiece.previous.log` before the program
starts again, the system's crash record, and the check report (**Help ›
Diagnostics…**). Draft the issue; let the player review it before posting.

**"Show me what the console looks like with this registration."**
`GET BASE/pages`, then `GET BASE/pages/{page}/picture`.

**"Log every change during my practice session."**
`api events switch,control,program` into a file.

**"Check that these [n] organs still open."**
Open each with `--gui-only` and read the exit status and the log.
