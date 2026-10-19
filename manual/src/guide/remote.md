# Remote control and the web console

Masterpiece can serve a page that any browser on the same network opens: a phone or a tablet on the music desk draws stops, presses pistons and moves the shoes, and every change made at the organ shows on it at once. The same server answers the control API, which consoles with a network connection, scripts and other programs use to read and change everything the page can.

Available in the standalone program on Windows, macOS, Linux and the Raspberry Pi. Plugins and the phone and tablet versions do not serve it.

## Turning it on

**Settings > General settings > Remote**:

| Control | What it does | Default |
|---|---|---|
| **Serve the web console and the control API on this network** | Starts the server; off stops it. Kept for the next start. | Off |
| **Allow changes from the network** | Off, the network can only read: the stops drawn, the levels, the organ loaded. | On |
| **Allow quitting Masterpiece and shutting the computer down** | The `quit` and `powerOff` commands, for a console with no screen. Masterpiece saves and quits; `powerOff` then shuts the computer down a few seconds later. Both are refused in the first two minutes after Masterpiece starts, so a faulty button cannot shut the computer down at every start. | Off |
| **Port** | The TCP port of the page and the API. Another program already using it shows **Not running** with the reason. | 8080 |
| **Answer the same requests over MIDI (System Exclusive)** | For a console with a MIDI cable and no network: requests are read on the MIDI inputs, and replies and changes go out on the console's MIDI output, chosen under MIDI. | Off |
| **Answer the same requests over OSC** | For control surfaces and apps that speak OSC, such as TouchOSC and Open Stage Control, on the UDP port beside it. | Off, port 9000 |
| **Token** | When set, every change needs it. Reading needs none. | Empty |
| **Open in the browser** | Opens the page on this computer. | |

Below the switches the tab lists the addresses to open on the network, such as `http://192.168.0.20:8080/`. On Windows the firewall asks once whether Masterpiece may accept connections; allow it for private networks.

While the server is on, anyone on the network can open the page. Without a token, anyone can also change the registration from it. A token keeps changes to those who know it: the page asks for it under **Access**, at the bottom, and keeps it in that browser.

## The web console

The page builds itself from the organ loaded:

- **One panel per division**, with its stops, couplers and tremulants as lit buttons. Stops a demo does not ship are greyed out and cannot be drawn.
- **The organ's pistons**, the switches of the organ file that fire and release.
- **Shoes and controls**: the swell shoes, the crescendo and the organ's other continuous controls as sliders.
- **Combinations**: the player's generals with **0** (general cancel) and **Set**, the divisionals of each division, and the sequencer's frame with its first, previous, next and last buttons.
- **The top bar**: the volume, transpose, temperament, the voicing EQ preset and **Panic**.
- **The bottom line**: the connection, the voices sounding and the late blocks.

Loading another organ rebuilds the page by itself.

## For console builders and scripts

The API is HTTP with JSON on the same port. Every object of the organ has a type and an index from 0, valid for one organ load: switches, controls, divisions, keyboards, generals, divisionals, EQ presets, temperaments and commands, and the program's own properties.

| Request | Does |
|---|---|
| `GET /api/v1` | The program, the protocol version, the session and the organ |
| `GET /api/v1/switches` | Every switch the console shows, with its name, role, division and state; `?all=1` adds the organ's internal switches, every one the player could move |
| `GET /api/v1/switches/values` | Every switch's state, in index order |
| `PUT /api/v1/switches/12/value` with `{"value": 1}` | Draws switch 12; `0` retires it, `"toggle"` toggles it |
| `PUT /api/v1/controls/0/value` with `{"value": 90}` | Moves a shoe, 0 to 127 |
| `POST /api/v1/generals/2/press` | Recalls general 3 |
| `GET /api/v1/generals/2` | General 3, with `holds`: the switches it draws |
| `PUT /api/v1/ranks/4/value` with `{"value": -30}` | Rank 4's voicing level to -3 dB, heard from the next note |
| `PUT /api/v1/notes/188/value` with `{"value": 1}` | Plays MIDI note 60 on the second keyboard (1 × 128 + 60); `0` releases it |
| `PUT /api/v1/program/transpose` with `{"value": -2}` | Transposes two semitones down |
| `POST /api/v1/commands/9/press` | Saves, quits and shuts the computer down, when allowed |
| `GET /api/v1/events` | A stream of every change, as server-sent events |

A request that changes something is sent with `Content-Type: application/json`, and with `Authorization: Bearer <token>` when a token is set. The full description is served at `/api/v1/openapi.json` and can be opened in any OpenAPI tool. The protocol, its object types, properties and errors are described in [Control protocol](../dev/control-protocol.md).

A microcontroller on Wi-Fi, such as an ESP32, sends these requests directly. It should keep the Wi-Fi power saving off, which otherwise delays each request by up to a tenth of a second. Notes are played over MIDI as before.

### Over MIDI and OSC

The same objects, with the same types and indexes, can be read and changed without HTTP:

- **MIDI System Exclusive**, for a console whose microcontroller has a MIDI port and no network, such as an Arduino with USB MIDI. A Set of switch 12 to 1 is the 15 bytes `F0 7D 4D 50 00 00 05 05 01 0C 00 01 00 00 F7`, and the reply comes back on the console's MIDI output. Several programs on one MIDI network are told apart by a unit number, 0 by default, set with the line `apisysexunit 1` in the settings file.
- **OSC**, for TouchOSC, Open Stage Control and similar apps. Send `/mp/replyto` with the app's port first, then `/mp/subscribe`; a fader bound to `/mp/controls/0` moves the first shoe and follows it, and a button bound to `/mp/switches/12` draws switch 12 and lights while it is drawn.

**Allow changes from the network** applies to OSC as to HTTP. Each message and its replies are described in [Control protocol](../dev/control-protocol.md).
