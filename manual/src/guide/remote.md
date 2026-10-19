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
| **Port** | The TCP port. Another program already using it shows **Not running** with the reason. | 8080 |
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
