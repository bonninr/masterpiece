# Control protocol

Version 0.1. HTTP is implemented, with the web console; SysEx and OSC follow the same model and are described here for the builders who will use them.

One model of what can be read and changed in a running Masterpiece, and the ways to reach it: HTTP with JSON, MIDI System Exclusive, and OSC. The model is the reference. A transport section only says how the model's operations are written as bytes or paths; it adds no operation of its own, so a console, a script and a web page see the same organ.

**The definition is `protocol/control.yaml`.** Its types, properties, program properties, commands, operations, errors and encodings are the ground truth; `tools/gen-protocol.py` generates `src/mp_api/ControlProtocol.h`, the tables the program uses, and `resources/web/openapi.json`, the HTTP description the program serves at `/api/v1/openapi.json`. `gen-protocol.py --check` fails when either is out of date. A change to the protocol is a change to the YAML file, then the generator, and the three committed together.

In the program, `src/mp_api/ControlModel` is the model and `src/mp_api/ApiServer` the HTTP transport (cpp-httplib, one MIT-licensed header). Every request is carried out on the message thread, as a click in the window is; while an organ loads every call answers `busy`. Changes are found by comparing every value 20 times a second, so a change made anywhere (the window, MIDI, a piston, the sequencer, another client) reaches every subscriber.

The window, the MIDI mapping and these transports all act through the same commands inside the program. A stop drawn over SysEx is drawn exactly as a click or a learned MIDI message draws it, and it is reported to every client as a change.

## Principles

- **The organ as loaded, by type and index.** Every object has a type and an index from 0, dense, valid for one load of one organ. An index is the position in a list, so the state of every switch is a plain bit field. The organ file's own id is one of the object's properties, for clients that keep a mapping across loads.
- **A session number** names the load. It moves on each time an organ loads or unloads. A request may carry the session it was built for, and is refused with `stale-session` when the organ has changed under it.
- **Requests and replies, and events.** Each request has one reply. A client that subscribes also receives an event for every change it subscribed to, whatever caused it.
- **Small messages.** Every operation fits a 64-byte SysEx message, except names and descriptions, which come one object at a time. A board with 2 KB of RAM can take part.
- **Integers.** Every value is a whole number in a range the property states; a level in dB is in tenths of a dB, for example.
- **Reading is always allowed; changing needs permission.** Each transport has its own switch in General settings, off by default, and HTTP adds a token.

## Object types

| Code | Type | Index range | Value | Changeable | Notes |
|---|---|---|---|---|---|
| 0 | `program` | properties below | per property | some | One per running program |
| 1 | `switch` | 0 to switches - 1 | 0 off, 1 on | yes | Every switch of the organ file: drawstops, couplers, tremulants, pistons and the rest. Its role is a property |
| 2 | `control` | 0 to controls - 1 | 0 to 127 | yes | Shoes, crescendo and other continuous controls |
| 3 | `division` | 0 to divisions - 1 | none | no | For names and grouping |
| 4 | `keyboard` | 0 to keyboards - 1 | none | no | Manuals and pedal; notes stay on MIDI |
| 5 | `general` | 0 to generals - 1 | 1 while it is the last recalled | press | The player's general pistons |
| 6 | `divisional` | 0 to divisionals - 1 | 1 while it is the last recalled in its division | press | The player's divisional pistons |
| 7 | `eqpreset` | 0 to presets - 1 | 1 while the EQ is this preset | press | The program's ten, then the player's own |
| 8 | `temperament` | 0 to temperaments - 1 | 1 while chosen | press | |
| 9 | `command` | the list below | none | press | Buttons that are no object of the organ |

A `press` acts like a thumb piston: it does what the object does, once.

### Program properties (type 0)

| Index | Property | Value | Changeable |
|---|---|---|---|
| 0 | Session number | 0 to 2 097 151, round | no |
| 1 | Organ loaded | 0 none, 1 loading, 2 ready | no |
| 2 | Load progress | 0 to 1000 (tenths of a percent) | no |
| 3 | Master gain | tenths of a dB, -400 to +240 | yes |
| 4 | Transpose | semitones, -12 to +12 | yes |
| 5 | Temperament | index into `temperament` | yes |
| 6 | Stepper frame | 0 to frames - 1 | yes |
| 7 | Setter | 0 off, 1 on | yes |
| 8 | Voicing EQ on | 0, 1 | yes |
| 9 | Voicing EQ preset | index into `eqpreset`, or -1 when none matches | yes |
| 10 | Polyphony limit | voices | yes |
| 11 | Sounding voices | voices | no |
| 12 | Late blocks since load | count | no |
| 13 | Real-time render threads | 0, 1 | yes |
| 14 | MIDI recorder | 0 stopped, 1 recording | yes |
| 15 | Audio recorder | 0 stopped, 1 recording | yes |
| 16 | Metronome | 0 off, 1 on | yes |
| 17 | Metronome tempo | beats a minute, 20 to 300 | yes |

New properties are added at the end; an index is never reused.

### Commands (type 9)

| Index | Command |
|---|---|
| 0 | Panic: every note off |
| 1 | General cancel |
| 2 | Stepper next |
| 3 | Stepper previous |
| 4 | Stepper first |
| 5 | Stepper last |
| 6 | Voicing EQ next preset |
| 7 | Voicing EQ previous preset |

### Properties of an object

Asked for with **Describe**, one object at a time.

| Property | Types | Value |
|---|---|---|
| `name` | all | The name the organ file gives, or the program's |
| `odfId` | switch, control, division, keyboard | The organ file's own id |
| `role` | switch | `stop`, `coupler`, `tremulant`, `piston`, `other` |
| `latching` | switch | 1 for a drawstop, 0 for a piston |
| `division` | switch, control, divisional | Index into `division`, or -1 |
| `available` | switch | 0 for a stop a demo does not ship |
| `channel` | keyboard | The MIDI channel it plays on, 1 to 16 |
| `own` | eqpreset | 1 for the player's own |

## Operations

| Code | Operation | Request | Reply |
|---|---|---|---|
| 1 | **Identify** | | program name, protocol version, program version, session, unit number |
| 2 | **Count** | type | how many objects of that type |
| 3 | **Describe** | type, index | the object's properties |
| 4 | **Get** | type, index | value |
| 5 | **Set** | type, index, value | the value now |
| 6 | **Press** | type, index | done |
| 7 | **Dump** | type, first index, count | the values; for a type whose values are 0 and 1, a bit field |
| 8 | **Subscribe** | a set of types | done; from then on, events |
| 9 | **Unsubscribe** | a set of types, or all | done |
| 10 | **Event** | (sent by the program) | type, index, value |
| 11 | **Error** | (a reply) | the request's operation and an error code |

A **Set** on a switch takes 0, 1, or 2 for toggle. A **Set** on a momentary switch (a piston) presses it for 1 and does nothing for 0.

When an organ loads, every subscriber receives an event for program property 1 (organ loaded) and property 0 (the new session), and should Count and Dump again.

### Errors

| Code | Error |
|---|---|
| 1 | `unknown-operation` |
| 2 | `unknown-type` |
| 3 | `out-of-range`: no object at that index |
| 4 | `bad-value`: outside the property's range |
| 5 | `read-only` |
| 6 | `not-allowed`: changing is switched off for this transport, or the token is missing |
| 7 | `no-organ` |
| 8 | `stale-session` |
| 9 | `busy`: an organ is loading |
| 10 | `bad-message`: too short, too long or malformed |
| 11 | `unsupported-version` |

### Versions

The protocol version is a major and a minor number. A minor version adds properties, commands, types or operations at the end; a client written for an earlier minor version works unchanged. A major version may change what exists, and a client is refused with `unsupported-version`.

## HTTP and JSON

The program serves HTTP on the port chosen in General settings, on the local network.

| Operation | Method and path |
|---|---|
| Identify | `GET /api/v1` |
| Count | included in `GET /api/v1/{types}` |
| Describe | `GET /api/v1/{types}/{index}`, or the whole list with `GET /api/v1/{types}` |
| Get | `GET /api/v1/{types}/{index}/value` |
| Set | `PUT /api/v1/{types}/{index}/value` with `{"value": 1}`, or `{"value": "toggle"}` for a switch |
| Press | `POST /api/v1/{types}/{index}/press` |
| Dump | `GET /api/v1/{types}/values` |
| Subscribe, events | `GET /api/v1/events?types=switch,control`, a server-sent event stream; or a WebSocket on the same path |
| Error | the HTTP status, with `{"error": "out-of-range"}` |

The type in a path is its plural: `switches`, `controls`, `generals`, `eqpresets`. The program is `GET /api/v1/program` and `PUT /api/v1/program/{property}`. A request that changes something is sent as `Content-Type: application/json` (a page of another site cannot send that without the browser asking first, and the server never agrees, so a stray web page cannot change the organ), carries `Authorization: Bearer <token>` when a token is set, and may carry `X-Session: <n>`.

An event:

```
event: change
data: {"type": "switch", "index": 12, "value": 1, "session": 41}
```

A Dump of switches:

```json
{"session": 41, "type": "switch", "first": 0, "count": 60, "values": [0, 1, 1, 0, ...]}
```

## MIDI System Exclusive

Read on the MIDI inputs allowed in General settings, answered on the MIDI output chosen there.

### Message layout

```
F0 7D 4D 50 <unit> <major> <operation> <sequence> <payload ...> F7
```

| Byte | Meaning |
|---|---|
| `7D` | The ID for non-commercial and experimental use, while the protocol is tried. A registered manufacturer ID replaces it before version 1.0 |
| `4D 50` | `M` `P` |
| `unit` | 0 to 126, set in General settings, for several programs on one MIDI network; 127 addresses all |
| `major` | the protocol's major version |
| `operation` | the code from the operations table |
| `sequence` | 0 to 127, chosen by the client and repeated in the reply, to pair them; 0 in events |

Every data byte is 7 bits, as MIDI requires.

| Field | Bytes | Encoding |
|---|---|---|
| type | 1 | the type code |
| index | 2 | 14 bits, low 7 bits first |
| value | 3 | 21 bits, two's complement, low 7 bits first |
| count | 2 | as index |
| text | variable | UTF-8, in groups of 7 bytes each preceded by a byte with their high bits (the 8-to-7 packing of sample dumps) |

### Payloads

| Operation | Request payload | Reply payload |
|---|---|---|
| Identify | none | minor, program version (3 bytes), session (value), unit, then name (text) |
| Count | type | type, count |
| Describe | type, index | type, index, then property code (1 byte) and its value or text, repeated; names chunked across messages with a part number |
| Get | type, index | type, index, value |
| Set | type, index, value | type, index, value now |
| Press | type, index | type, index |
| Dump | type, first, count (up to 280 for bits, 16 for values) | type, first, count, then bits 7 to a byte, low bit first, or values |
| Subscribe | a type mask (2 bytes: bit n for type n) | the mask |
| Event | | type, index, value |
| Error | | operation, error code |

A Dump of 280 switches is 40 bytes of bits; a console asks for 0-279, 280-559 and so on until it has them all.

### Examples

Draw switch 12 on unit 0, sequence 5:

```
F0 7D 4D 50 00 00 05 05 01 0C 00 01 00 00 F7
                     |  |  |  |     |
                     |  |  |  |     value 1
                     |  |  |  index 12
                     |  |  type switch
                     |  sequence 5
                     operation Set
```

The reply carries the same sequence and the value now:

```
F0 7D 4D 50 00 00 05 05 01 0C 00 01 00 00 F7
```

The event every subscriber receives:

```
F0 7D 4D 50 00 00 0A 00 01 0C 00 01 00 00 F7
```

## OSC

UDP on the port chosen in General settings. Replies and events go to the address and port the request came from.

| Operation | Address and arguments |
|---|---|
| Identify | `/mp/identify` |
| Count | `/mp/{type}/count` |
| Get | `/mp/{type}/{index}` with no argument |
| Set | `/mp/{type}/{index}` with an integer |
| Press | `/mp/{type}/{index}/press` |
| Subscribe | `/mp/subscribe` with the type names as strings |
| Event | `/mp/{type}/{index}` with the integer value, sent by the program |
| Error | `/mp/error` with the address and the error name |

## MIDI-CI Property Exchange

A later binding for MIDI 2.0 tools: each HTTP resource becomes a property resource with the same JSON, under names of the form `X-MP-Switches`.

## Open questions

- A registered SysEx manufacturer ID, applied for before version 1.0.
- Whether notes may be played through these transports, for automation and tests. MIDI is the transport for playing.
- Whether a web console served by the program is part of the first version.
- 14-bit values for continuous controls, for consoles with fine shoe sensors.
