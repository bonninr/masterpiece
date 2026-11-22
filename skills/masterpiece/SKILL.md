---
name: masterpiece
description: >
  Operate the Masterpiece virtual pipe organ (github.com/bonninr/masterpiece)
  for its player: build it from source, start it, load organs, draw stops,
  recall pistons, voice ranks and pipes (level, brightness, bass, pan, width,
  tuning), route ranks and perspectives to audio outputs, set crossovers and
  output EQ, map MIDI consoles, render MIDI takes to WAV and measure them, and
  prepare changes to the code as pull requests. Use when a request names
  Masterpiece, an organ sample set loaded in it, its control API, or a task on
  its settings, MIDI mapping, voicing or audio outputs.
---

# Masterpiece

Masterpiece is a free virtual pipe organ (C++, JUCE) for Windows, macOS,
Linux, Android and iOS. It plays Hauptwerk and GrandOrgue sample sets.

## How this skill works

Masterpiece contains no AI and calls none. The agent works from the outside,
with the same means any player or script has:

| Means | Reaches | Reference |
|---|---|---|
| The control API (HTTP/JSON, built in) | Everything the window does while an organ is loaded: stops, couplers, pistons, notes, shoes, voicing, rank routing, live levels, temperament, transpose, recording | `reference/control-api.md` |
| The command line | Opening an organ, drawing a registration, playing MIDI files, recording WAV takes, logs | `reference/command-line.md` |
| The settings files | Settings with no API object: audio outputs and their channels, crossovers, output EQ, MIDI mappings | `reference/settings-files.md` |
| The source code | Builds, fixes, new features, pull requests | `reference/source-and-prs.md` |

`reference/use-cases.md` lists tasks with the means each one uses. Read it to
pick an approach.

Rules:

- Prefer the API over the settings files. The API checks every value, and the
  window shows each change at once.
- Edit a settings file only with Masterpiece closed. The program writes its
  files when it quits and overwrites edits made while it ran. Copy the file
  first.
- Change no code for a task the API, the command line or the settings reach.
  Code changes are for builds, fixes and features the player asks for.
- Report what changed: object, old value, new value. Keep the old values, so
  the player can return to them.

## Connecting to the player's Masterpiece

The player turns the API on in **Settings › General settings › Remote**:

1. **Serve the web console and the control API on this network**: on.
2. **Allow changes from the network**: on.
3. **This computer only**: on, unless the agent runs on another machine.
4. **Token**: any secret text. Changes need it; reading does not.
5. **Port**: 8080 by default.

Then:

```bash
export MP_BASE=http://127.0.0.1:8080/api/v1
export MP_TOKEN=<the token>
python3 scripts/mp_api.py identify     # protocol, organ state, session
python3 scripts/mp_api.py program      # every program property
```

`scripts/mp_api.py` needs only Python 3 and runs on every desktop system (`py`
or `python` in place of `python3` on Windows). Run it with no arguments for
its commands.

## Starting a Masterpiece of its own (Linux, macOS)

For renders, tests and work on a machine where nobody plays:

```bash
eval "$(scripts/mp_launch.sh --odf '/path/to/Organ.organ')"   # API on, token made, waits for the load
python3 scripts/mp_api.py names switches 'trompet'
scripts/mp_stop.sh                                            # always: stops it, restores the settings
```

`mp_launch.sh` writes the API keys into `settings.mpglobal`, backs the file up
first, starts the program (under `xvfb-run` when there is no display), waits for
the API and for the organ, and prints `MP_BASE`, `MP_TOKEN` and `MP_PID`.
`mp_stop.sh` stops it and restores the backup. Set `MP_BIN` to the program when
it is neither a build in this repository nor `Masterpiece` on the `PATH`.

On Windows, start `Masterpiece.exe` and turn the API on as above.

## The API in four lines

- Objects have a type and an index from 0, valid for one organ load. Paths use
  the plural type: `/api/v1/switches/12/value`.
- Read: `GET /{type}`, `GET /{type}/{i}`, `GET /{type}/{i}/value`,
  `GET /{type}/values`.
- Change: `PUT /{type}/{i}/value` with `{"value": N}`; press: `POST /{type}/{i}/press`.
- After a reload, `session` moves on and indices can change: map objects again
  by `name` or `odfId`.

The running program serves its full description at `GET /api/v1/openapi.json`.
That description wins over anything in this skill.

## Measuring instead of guessing

A voicing or routing change is checked by ear by the player, and by
measurement by the agent: render the same MIDI before and after with
`scripts/render_takes.sh`, then compare with `scripts/analyze_audio.py`
(level, peak, noise floor, bass/mid/treble shares, per-note levels). Report the
numbers with the change.

## Safety

- The API serves the local network unless **This computer only** is on. Keep a
  token set.
- `commands` 8 (quit) and 9 (power off) need **Allow quitting Masterpiece and
  shutting the computer down** and answer `busy` in the first two minutes after
  start. Never power off a machine without the player's request.
- A large organ takes minutes and gigabytes to load. Check the free memory
  before loading another one, and ask before closing the player's organ.
