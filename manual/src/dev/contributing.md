# Contributing

This page explains how to report a problem, ask a question, propose a change and add a translation, what licence a contribution carries, and the conventions the code follows. Masterpiece is free software under GPL-3.0-only, developed in the open at <https://github.com/bonninr/masterpiece>.

## Reporting issues

Bug reports and organ definitions that do not load are welcome at <https://github.com/bonninr/masterpiece/issues>. A report that can be acted on contains:

- The version (`Masterpiece --version`) and the system.
- The organ and where it came from: the sample set's name and publisher, and whether it was opened from a folder or from a RAR or `.orgue` package.
- The log. Masterpiece always writes `masterpiece.log`, and keeps the previous run's as `masterpiece.previous.log`, in the settings folder:

  | System | Folder |
  |---|---|
  | Windows | `%APPDATA%\Masterpiece` |
  | macOS | `~/Library/Masterpiece` |
  | Linux | `~/.config/Masterpiece` |

  **Settings > General settings > Log** shows the current log in the program.
- For a fault in loading, `--log <file>` writes the log from the start with the timing of each stage. For a fault in MIDI, `--log-midi` records every message and what became of it. For a click or a wrong tail on key release, `--log-releases` describes every release. For a stuck note that appears at random, `--record-midi <file>` saves the session as a MIDI file. See [Command line](../guide/command-line.md).
- The steps that produce the fault, and what you expected.

An issue describes one concrete problem. Use Discussions for questions, ideas and themes.

## Discussions

Questions, ideas, requests for support of a new feature and conversations about how something should work go to <https://github.com/bonninr/masterpiece/discussions>. Source comments cite discussions and issues by number (for example `#358` for the split keyboard).

## Pull requests

- Branch from `main`, and keep each pull request to one change.
- Run the test suite (`mp_tests`; see [Testing](testing.md)). CI runs it on every pull request on Windows, Linux x86-64, Linux arm64 and macOS (Apple silicon and Intel), and builds the 32-bit Raspberry Pi target, the Flatpak, iOS and Android.
- Add or update a test for behaviour you change. A test that fails without your change documents the fault.
- Describe in the pull request what the change does for a player, and how you tested it.
- Run `python tools/i18n.py --check` when you add or change a string the program shows; see [Translating](#translating).

A pull request that touches only `README.md`, `COPYING`, `LICENCE`, `.gitignore`, `screenshots/`, `site/`, `manual/`, `ATTRIBUTION.md` or `.github/workflows/pages.yml` starts no build.

## Licence of contributions

By contributing to Masterpiece you agree that your contribution is licensed under the GPL-3.0-only with the additional permission stated in `COPYING`, and that it may be distributed as part of Masterpiece through app stores and other channels, including under their terms.

The additional permission in `COPYING` covers combining a modified version with the JUCE framework under a JUCE licence other than the GPL or AGPL, or with the UnRAR library by RARLAB under its own licence.

## Translating

The program's own text is translated with one plain-text file per language in `resources/translations/`, named by the two-letter language code. The languages shipped are Czech (`cs`), German (`de`), Spanish (`es`), French (`fr`), Italian (`it`), Dutch (`nl`) and Polish (`pl`). Names of organs, stops and pages come from the organ and are never translated. English is the text in the code and has no file.

To add a language:

1. Copy `resources/translations/strings.txt`, the list of every string in English, to `resources/translations/<code>.txt`.
2. Fill in the `language:` line with the language's name in that language, and the `countries:` line with the country codes where it is spoken.
3. Put the translation on the right of each `"English" = "English"` line.
4. Keep `%1`, `%2` and the other placeholders; they stand for numbers and names the program puts in, and may come in any order the language needs.
5. Save the file as UTF-8 with no byte order mark.

At run time a string a file leaves out stays English. The tests are stricter: they fail when any language file lacks a string, holds an empty translation, changes the placeholders of a string or contains a string the program no longer has. Translate every line before you open the pull request. Lines beginning with `#` are comments.

The translated text is chosen in **Settings > General settings > Language**.

`tools/i18n.py` keeps the files in step with the code:

| Command | Effect |
|---|---|
| `python tools/i18n.py` | Reports, for each language, how many strings are translated and which are missing, stale, empty or have different placeholders |
| `python tools/i18n.py --write` | Rewrites `strings.txt` from the `TRANS("...")` strings in `src/` and `apps/`; run it after you add or change a string in the code |
| `python tools/i18n.py --check` | Reports, and exits with 1 when anything is missing, stale or inconsistent |

Every string a player reads goes through `TRANS("...")`. Write the English text in the code and run `--write`; the test `functional.i18n.template-covers-sources` fails when `strings.txt` is out of date. CMake embeds every `resources/translations/*.txt` except `strings.txt` in the program; the file list is a glob that CMake re-evaluates at each build.

## Code style

There is no formatter configuration in the repository. The style below is what the existing code shows; match the file you edit.

**Language and layout.**

- C++20, two-space indentation, `#pragma once` in headers, `//` comments.
- Source files are UTF-8. The repository normalises line endings through `.gitattributes` (`* text=auto`; shell scripts, `.yml` and `.cmake` files are always LF).
- The code is in namespace `mp`; sub-namespaces are `mp::ui` (interface), `mp::split`, `mp::dsp`, `mp::simd`, `mp::codm` and `mp::test`. File-local helpers sit in an anonymous namespace.

**Names.**

- Types are `PascalCase`: `OrganModel`, `SwitchNetwork`, `VoiceEngine`. Functions and methods are `camelCase`: `loadOrgan`, `resolvePipesInto`. Data members end in an underscore: `model_`, `engagedSwitches_`. Constants begin with `k`: `kMidiQueueSize`, `kFirstRoleChannel`. Enumerators are `PascalCase` inside an `enum class`.
- Object ids use the alias `Id` (an `int`), as the definition format does; `0` means none.
- Files are named after the main type: `SwitchNetwork.h` and `SwitchNetwork.cpp`. A test is a class named `<What>Test` with a test name `functional.<area>.<what>`.

**Module rules.**

- Keep `mp_core`, `mp_archive`, `mp_sampler`, `mp_control` and `mp_split` free of JUCE headers. A new engine feature goes in one of these when it can, so the test suite covers it with no audio stack.
- Text the player reads goes through `TRANS("...")`.
- Code on the audio thread follows the rules in [Architecture](architecture.md#rules-for-the-audio-thread).

**Comments.** The code carries a high density of comments, and the comments explain why. A file opens with a block that says what the file is for, the model it follows and the faults that shaped it. A non-obvious line states the reason for it, often with the number of the issue or discussion that exposed the problem (`#120`, `#267`, `#320`) and the organ on which it showed. Write comments in full sentences in plain language; they are part of how the code is reviewed. Do not restate what the line does.

Comments also cite decision records (`ADR-012`) and milestones (`M2.4`). The decision records are not in the public repository, so a comment that depends on one also says what it decided.

**Tests.** A fix for a fault found on a real organ gets a test that reproduces the fault on a synthetic definition. Fixtures are written for the project and never copied from other projects or from licensed sample sets; see [Testing](testing.md#fixtures).

## This manual

The manual is an mdBook in `manual/`; the table of contents is `manual/src/SUMMARY.md`, and `manual/WRITING.md` is the style guide. Build it with `mdbook build manual`. `.github/workflows/pages.yml` builds it with mdBook v0.5.4 into `site/docs` when a commit changes `manual/**`, and publishes it at <https://masterpieceorgan.com/docs/>.

## Related files

| File | Content |
|---|---|
| `README.md` | Features, installation, a summary of building and the layout of the modules |
| `COMMAND-LINE.md` | Every command-line option |
| `PERFORMANCE.md` | The measurements, in the [Performance](performance.md) page |
| `ATTRIBUTION.md` | Credits for sample sets and music |
| `COPYING`, `LICENCE` | The licence terms |
