# Control API

The sources, in order of precedence:

1. `GET /api/v1/openapi.json` from the running program: the endpoints, types
   and schemas that build serves.
2. `protocol/control.yaml` in the repository: the definition the program and
   the OpenAPI file are generated from (`tools/gen-protocol.py`).
3. `manual/src/dev/control-protocol.md`: the prose reference, with value
   ranges, program properties, commands and error codes.

This page holds what an agent needs to drive the API, not a copy of it.

## Requests

- Base URL: `http://<host>:<port>/api/v1`; port 8080 by default.
- Reading needs nothing. A change needs **Allow changes from the network** on,
  `Content-Type: application/json`, and `Authorization: Bearer <token>` when a
  token is set.
- `X-Session: <n>` on a change answers `stale-session` when another organ was
  loaded since the agent read `session`.
- During a load, calls answer `busy`. Wait for program property `organState` 2.

| Verb | Path | Does |
|---|---|---|
| GET | `/` | Identify: protocol, organ state, session |
| GET | `/program` | Every program property |
| PUT | `/program/{index}` | Sets one program property |
| GET | `/{type}` | Count and list; console objects only, `?all=1` for every one |
| GET | `/{type}/{i}` | One object with its properties |
| GET | `/{type}/{i}/value` | Its value |
| GET | `/{type}/values` | Every value of the type |
| PUT | `/{type}/{i}/value` | `{"value": N}`, or `"toggle"` for a switch |
| POST | `/{type}/{i}/press` | Pistons, presets, temperaments, commands |
| GET | `/events?types=switch,control` | Server-sent events, one per change |
| GET, POST | `/pages…` | Console pictures and the pointer |

## Types

| Plural | Value |
|---|---|
| `switches` | 0/1: stops, couplers, tremulants, pistons (property `role`) |
| `controls` | 0–127: shoes, crescendo, other continuous controls |
| `divisions` | Live level of the division, 0–203, 127 as voiced; resets with each organ |
| `keyboards` | Read only; property `channel` |
| `generals`, `divisionals`, `eqpresets`, `temperaments`, `commands` | Pressed |
| `notes` | 0/1; index = keyboard index × 128 + MIDI note; 1 plays, 0 releases |
| `ranks` | Voicing level, tenths of a dB, −240 to 120; next note |
| `pipes` | Level on top of the rank's, tenths of a dB; next note |
| `brightnesses` | High shelf above 3 kHz, tenths of a dB, −120 to 120; one per rank, then one per division |
| `basses` | Low shelf below 200 Hz, as `brightnesses` |
| `pans` | −100 left to 100 right; per rank, then per division |
| `widths` | Percent, 0 mono to 200; per rank, then per division |
| `enclosures` | Shade opening, 0–127 |
| `tremulants` | 0/1 running; properties `rate`, `depth` |
| `windchests` | Pressure, tenths of a percent of nominal; read only |
| `routes` | The output a rank plays through, from 1, as the Outputs tab lists them; next note |
| `pairs` | Live level of each output, 0–203, 127 as voiced; resets with each organ |
| `ranklevels` | Live level of each rank, as `pairs` |
| `mutes`, `solos` | 0/1: per rank, then division, then perspective, then output; reset with each organ |
| `organs` | The favourite organs, 64 slots; a press loads one |

Program properties by index: 3 master gain (tenths of a dB), 4 transpose, 5
temperament, 6 sequencer frame, 7 setter, 8 voicing EQ on, 14 MIDI recorder,
15 audio recorder, 16 metronome, 18 room on, 19 room wet. The full list, with
ranges, is in `protocol/control.yaml`.

Commands by index: 0 panic, 1 general cancel, 2–5 sequencer next, previous,
first, last, 8 quit, 9 power off.

## Habits

- Map by name once per load: `mp_api.py names switches` prints index, name,
  role and division. Keep `odfId` to find the same object after a reload.
- A rank's describe gives its `tuning` and the output `bus` of its main
  perspective; a route's describe gives `to`, the output's name.
- A switch with `available` false is a stop the sample set does not ship (a
  demo); it stays off when set.
- Dump a type before changing it (`mp_api.py dump ranks`): the dump is the
  undo.
