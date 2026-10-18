# Writing the Masterpiece manual

The manual is an mdBook in `manual/`, published at masterpieceorgan.com/docs.
Pages are Markdown under `manual/src/`; the table of contents is
`manual/src/SUMMARY.md`. Build it with `mdbook build manual`.

## Readers

Organists and virtual-organ builders: people who know organs, consoles and
MIDI well, and computers to varying degrees. Write for professionals. Never
explain organ basics (what a coupler, a general or a swell box is).

## Register

Technical reference, in the manner of docs.solidcraft.io: dense, exact and
complete. Full declarative sentences, present tense. Exact names, figures,
defaults, ranges, paths and commands. A page opens with one paragraph saying
what it covers and what the reader can do with it, then goes straight to the
substance.

- Interface labels exactly as the program shows them, in bold:
  **Settings > General settings > Audio**, the **Release tails** box. Copy
  them from the `TRANS("...")` strings in the code, character for character.
- File names, folders, commands and settings-file keys in `code`.
- Tables for parameters and for comparisons.
- Callouts with mdBook's admonitions, sparingly:
  `> [!NOTE]`, `> [!TIP]`, `> [!WARNING]`, `> [!CAUTION]`.

## Rules

- State what a thing is and does. Never "X, not Y", "rather than",
  "instead of", "not X but Y" or "unlike". Search your text for "not",
  "rather than", "instead of" and "without" before you finish, and rewrite
  each one as a positive statement where you can. "Does not" is fine when it
  states a plain limitation a reader must know ("The plugin does not open
  RAR packages").
- No marketing words: powerful, seamless, robust, blazing, effortless,
  cutting-edge, simply, just, easily. No exclamation marks, no emoji, no
  all-caps words, no rhetorical questions.
- No condescension, no justifying design choices, no padding. Every sentence
  carries a fact.
- The format: say "Hauptwerk-format sample sets" (or "Hauptwerk-format
  organs"), and "GrandOrgue organs". The word "Hauptwerk" appears only in that
  compound, where a reader needs it to know what files work. Never name the
  company behind it, never compare Masterpiece with other programs.
- Never name a download manager or any download tool.
- Nothing about AI, assistants or how the manual was written.

## Accuracy

Every statement comes from the code in this repository (`src/`, `apps/`,
`packaging/`, `.github/`, `README.md`, `COMMAND-LINE.md`, `PERFORMANCE.md`).
Read the code for each setting: its label, its default, its range or
options, what it changes, when it takes effect (at once, at the next load,
at restart), and where it is stored. Comments in the code explain intent and
history; use them.

When something cannot be confirmed from the code, write the best statement you
can and put an HTML comment right after it: `<!-- CHECK: what to verify -->`.
Never invent a number, a default, a menu item or a behaviour.

Platform differences (Windows, macOS, Linux, Raspberry Pi, Android, iPhone
and iPad, the plugins) matter: say where a setting or feature differs or is
missing.

## Parameters

Document every parameter a page covers. For each one, in a table or as an
`###` heading with a short paragraph:

| Field | What to give |
|---|---|
| Name | The label exactly as shown |
| What it does | One or two sentences, concrete |
| Default | The default value |
| Range or options | Every option, with what each does |
| Takes effect | At once, at the next organ load, or at restart |
| Advice | When to change it, and to what, where that is useful |

## Screenshots

Reference screenshots as `![Description](../images/NAME.png)` from pages in
`guide/` or `dev/`, and `![Description](images/NAME.png)` from pages at the
top. Use these names; they are being captured from the Linux build, 0.8.x,
with the Nancy demo (a Hauptwerk-format demo set) unless noted:

| Name | Shows |
|---|---|
| `console-nancy.png` | The main window, Nancy's console, a few stops drawn |
| `console-grandorgue.png` | The GrandOrgue demo organ |
| `console-composite.png` | A composite organ (Jehmlich composite) |
| `console-portrait.png` | A narrow, tall window: a portrait jamb layout |
| `menu-file.png` | The File menu open |
| `menu-view.png` | The View menu open |
| `menu-settings.png` | The Settings menu open |
| `open-window.png` | File > Open...: the favourites window |
| `loading-dialog.png` | The load progress dialog |
| `general-audio.png` ... `general-log.png` | Each tab of General settings: `general-audio`, `general-room`, `general-metronome`, `general-midi`, `general-mixer`, `general-voicing`, `general-combination-sets`, `general-display`, `general-language`, `general-log` |
| `organ-loading.png`, `organ-stops.png`, `organ-voicing-eq.png`, `organ-midi.png` | Each tab of Organ settings |
| `manual-keys.png`, `manual-pistons.png`, `manual-send.png` | The dialog for one manual's MIDI, each tab |
| `midi-receive.png`, `midi-send.png`, `midi-shortcut.png` | The MIDI dialog of one stop, each tab |
| `split-keyboard.png` | View > Split keyboard |
| `combinations.png` | View > Combinations |
| `recorder.png` | File > Recorder... |
| `panel.png` | A panel tab (the plain stop view) |
| `stop-list.png` | The stop list view |
| `tuning-panel.png` | The temperament and tuning controls |
| `swell-faders.png` | View > Swell faders |
| `keyboard.png` | View > Keyboard, the on-screen keyboard |

If a page needs a screenshot that is not in the list, use a new descriptive
name in the same form and list it, with exactly what it must show, in your
report.

## Links

Link to other pages with relative paths: `[Memory, loading and
streaming](memory.md)` within `guide/`, `[Building](../dev/building.md)`
across. Section anchors are the heading text in lower case with spaces as
hyphens.
