# Remote control and the web console

Masterpiece can serve a page that any browser on the same network opens: a phone or a tablet on the music desk draws stops, presses pistons and moves the shoes, and every change made at the organ shows on it at once. The same server answers the control API, which consoles with a network connection, scripts and other programs use to read and change everything the page can.

Available in the standalone program on Windows, macOS, Linux and the Raspberry Pi. Plugins and the phone and tablet versions do not serve it.

## Turning it on

**Settings > Remote control...** opens the **Remote control** tab of the settings window:

| Control | What it does | Default |
|---|---|---|
| **Serve the web console and the control API on this network** | Starts the server; off stops it. Kept for the next start. | Off |
| **Allow changes from the network** | Off, the network can only read: the stops drawn, the levels, the organ loaded. | On |
| **Allow quitting Masterpiece and shutting the computer down** | The `quit` and `powerOff` commands, for a console with no screen. Masterpiece saves and quits; `powerOff` then shuts the computer down a few seconds later. Both are refused in the first two minutes after Masterpiece starts, so a faulty button cannot shut the computer down at every start. | Off |
| **This computer only** | The page, the API and OSC answer at `127.0.0.1` only: a browser or a program on this computer reaches them, and no other device on the network does. Windows asks for no firewall rule. | Off |
| **Port** | The TCP port of the page and the API. Another program already using it shows **Not running** with the reason. | 8080 |
| **Answer the same requests over MIDI (System Exclusive)** | For a console with a MIDI cable and no network: requests are read on the MIDI inputs, and replies and changes go out on the console's MIDI output, chosen on the **MIDI** tab. | Off |
| **Answer the same requests over OSC** | For control surfaces and apps that speak OSC, such as TouchOSC and Open Stage Control, on the UDP port beside it. | Off, port 9000 |
| **Token** | When set, every change needs it. Reading needs none. | Empty |
| **Open in the browser** | Opens the page on this computer. | |

Below the switches the tab lists the addresses to open on the network, such as `http://192.168.0.20:8080/`. On Windows the firewall asks once whether Masterpiece may accept connections; allow it for private networks. With **This computer only** the tab shows `http://127.0.0.1:8080/`, and the firewall does not ask.

While the server is on, anyone on the network can open the page. Without a token, anyone can also change the registration from it. A token keeps changes to those who know it: the page asks for it under **Access**, at the bottom, and keeps it in that browser.

## The web console

The page builds itself from the organ loaded. **View**, in the top bar, chooses what it shows: **Panels**, the default, or one of the organ's own console pages.

### Panels

- **One panel per division**, with its stops, couplers and tremulants as lit buttons. Stops a demo does not ship are greyed out and cannot be drawn. A stop drawn on two panels of the organ, or worked through a gate, is listed once.
- **The organ's pistons**, the switches of the organ file that fire and release: those of no division first, then each division's divisional pistons under the division's name.
- **Shoes and controls**: the swell shoes, the crescendo and the organ's other continuous controls as sliders.
- **Levels**: a live fader for each division and each output pair, heard at once.
- **Tremulants, enclosures, wind and room**: the tremulants as buttons, each swell box's opening, the wind pressures in a folded list headed with the number of reservoirs and chests and updated twice a second, and **Room** with its wet share.
- **Combinations**: the player's generals with **0** (general cancel) and **Set**, the divisionals of each division under its name, and the sequencer's frame, from `000`, with its first, previous, next and last buttons.
- **Every switch and control**, folded: every switch, control and rank level of the organ, its internal ones too, with a filter by name or number, and the MIDI channel of each keyboard.
- **The top bar**: **View**, the volume, transpose, the temperament, the voicing **EQ** preset and **Panic**.
- **The bottom line**: the connection, the voices sounding, the late blocks, and **Access** with the token and the API description.

### Console pages

Each of the organ's console pages, drawn by the program with its own console code. Taps and drags on the page work as clicks and drags on the console, and after a change only the parts drawn anew are fetched. **Page only** hides the bars and shows the page as large as the screen allows.

Each device keeps its own choice, so one tablet can stay on the left jamb and another on the right. An address ending in `#page=2` opens the third page directly. On a screen held upright, an organ that ships portrait layouts shows its portrait layout.

A page opened while an organ loads shows **Loading the organ** with the percentage, and builds itself when the organ is ready. Loading another organ rebuilds the page by itself.

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
| `GET /api/v1/pages` | The organ's console pages and their layouts |
| `GET /api/v1/pages/0/picture` | The first console page as a PNG image; `layout`, `x`, `y`, `w` and `h` choose a layout and a part of it |
| `POST /api/v1/pages/0/pointer` with `{"phase": "down", "x": 120, "y": 340}` | A press on the page at those coordinates; `move` and `up` follow it |

A request that changes something is sent with `Content-Type: application/json`, and with `Authorization: Bearer <token>` when a token is set. The full description is served at `/api/v1/openapi.json` and can be opened in any OpenAPI tool. The protocol, its object types, properties and errors are described in [Control protocol](../dev/control-protocol.md).

A microcontroller on Wi-Fi, such as an ESP32, sends these requests directly. It should keep the Wi-Fi power saving off, which otherwise delays each request by up to a tenth of a second. Notes are played over MIDI as before.

### Over MIDI and OSC

The same objects, with the same types and indexes, can be read and changed without HTTP:

- **MIDI System Exclusive**, for a console whose microcontroller has a MIDI port and no network, such as an Arduino with USB MIDI. A Set of switch 12 to 1 is the 15 bytes `F0 7D 4D 50 00 00 05 05 01 0C 00 01 00 00 F7`, and the reply comes back on the console's MIDI output. Several programs on one MIDI network are told apart by a unit number, 0 by default, set with the line `apisysexunit 1` in the settings file.
- **OSC**, for TouchOSC, Open Stage Control and similar apps. Send `/mp/replyto` with the app's port first, then `/mp/subscribe`; a fader bound to `/mp/controls/0` moves the first shoe and follows it, and a button bound to `/mp/switches/12` draws switch 12 and lights while it is drawn.

**Allow changes from the network** applies to OSC as to HTTP. Each message and its replies are described in [Control protocol](../dev/control-protocol.md).
