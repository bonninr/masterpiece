# Masterpiece skills

Instructions for AI agents that operate Masterpiece for its player: build it,
configure it, voice an organ, map a MIDI console, route outputs, record and
measure takes, prepare pull requests.

Masterpiece contains no AI and calls none, and the skills change no code of
it. The direction is the other way round: the player's agent reads these
instructions and works Masterpiece from the outside, through the same means
every player and script has: the control API built into the program, the
command line, the settings files, and the source code.

| Folder | Holds |
|---|---|
| `masterpiece/SKILL.md` | The entry point: how to connect, the rules, the safety limits |
| `masterpiece/reference/` | The control API, the command line, the settings files, builds and pull requests, and the use cases with sample requests |
| `masterpiece/scripts/` | `mp_api.py` (API client, Python 3, every system), `mp_launch.sh` and `mp_stop.sh` (an instance of its own, Linux and macOS), `mp_build.sh`, `render_takes.sh`, `analyze_audio.py` (needs numpy) |

## One-time use

No installation. Give the agent this request:

```
Read https://raw.githubusercontent.com/bonninr/masterpiece/dev/skills/masterpiece/SKILL.md
and the files it references, then follow those instructions to help me with Masterpiece.
```

Then ask for the task.

## Installing

For agents that load skills from a folder (a folder per skill, each with a
`SKILL.md`):

```bash
git clone --depth 1 --branch dev https://github.com/bonninr/masterpiece.git
cp -r masterpiece/skills/masterpiece <the agent's skills folder>/
```

On Windows, copy the `masterpiece` folder with File Explorer, or with
`xcopy /E /I masterpiece\skills\masterpiece <the agent's skills folder>\masterpiece`.

The agent's documentation names its skills folder. An agent without skill
folders reads `SKILL.md` as an ordinary instruction file: point it at the
copy.

To update, copy the folder again from a fresh clone.

## Before the first task

In Masterpiece, **Settings › General settings › Remote**:

1. **Serve the web console and the control API on this network**: on.
2. **Allow changes from the network**: on.
3. **This computer only**: on.
4. **Token**: any secret text. Give it to the agent.

The agent then reads and changes what the window shows. With the API off, the
agent can still build, read the files and prepare changes, but cannot drive
the running program.

## Sample requests

- Build a local copy of Masterpiece.
- Add [feature]. Create a pull request so it gets added.
- I connected a Johannus [model] to the computer. Download its MIDI manual and
  configure all the MIDI elements.
- Load Alessandria and configure all perspectives to my multi-output sound
  card.
- Send all the pedal ranks to output 5 of my sound card, with a low-pass that
  follows the spec of the Yamaha [subwoofer model].
- Adjust the reeds of Bückeburg: make them darker.
- Make the Principal chorus of the Great more even in loudness, and show me the
  measurements.
- Find the pipes of the Bourdon 16' that stand out in level.
- Prepare the registrations for [piece] on generals 1 to 4.
- Record [MIDI file] with the plenum, in equal temperament and in Werckmeister
  III, and compare the two.
- The organ does not fit in memory: propose loading options and apply the one
  I choose.
- Masterpiece crashed: collect the logs and the crash record and draft the
  issue.

`masterpiece/reference/use-cases.md` lists more, with the steps for each.

## Contributing

Changes to the skills go through pull requests against `dev`, as for the
program. Use cases that worked, and ones that failed, go to the project's
Discussions.
