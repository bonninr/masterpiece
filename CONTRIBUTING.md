# Contributing to Masterpiece

Thank you for helping. Bug reports, organ definitions that do not load, and
pull requests are all welcome.

## Before you open a pull request

- Branch from `main` and keep each pull request to one change.
- Run the test suite (`mp_tests`); CI runs it on every supported platform.
- Describe what the change does for a player, and how you tested it.

## Translating

The program's own text is translated with one plain-text file per language in
`resources/translations/`, named by the two-letter language code (`es.txt`,
`de.txt`). Names of organs, stops and pages come from the organ and are never
translated.

To add a language, copy `resources/translations/strings.txt` (the list of every
string, in English) to `<code>.txt`, fill in the `language:` line with the
language's name in itself and the `countries:` line, and put the translation on
the right of each line. `%1`, `%2` stand for numbers and names the program puts
in; keep them, in whatever order the language wants. A string left out stays
English. The files are UTF-8 without a byte order mark.

`python tools/i18n.py` reports what each language lacks, and
`python tools/i18n.py --write` rewrites `strings.txt` after a string is added to
the code. The test suite fails when a language file is incomplete or the list
is out of date.

## Licence of contributions

By contributing to Masterpiece you agree that your contribution is licensed
under the GPL-3.0-only with the additional permission stated in
[`COPYING`](COPYING), and that it may be distributed as part of Masterpiece
through app stores and other channels, including under their terms.
