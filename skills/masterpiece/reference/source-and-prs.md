# Source code, builds and pull requests

The repository is https://github.com/bonninr/masterpiece. Development happens
on the `dev` branch; releases come from `main`.

The manual's developer pages hold the details:

- `manual/src/dev/building.md` — prerequisites per system, presets, targets
- `manual/src/dev/testing.md` — the test suite
- `manual/src/dev/contributing.md` — conventions, translations, licence
- `manual/src/dev/architecture.md` — the modules and how they fit

## A local build

```bash
git clone https://github.com/bonninr/masterpiece.git
cd masterpiece
git checkout dev
cmake --preset dev-fast          # Release, no plugins, no link-time optimisation
cmake --build --preset dev-fast
```

The program lands in
`build/dev-fast/apps/MasterpieceApp/MasterpieceApp_artefacts/Release/`
(`Masterpiece.exe`, `Masterpiece.app` or `Masterpiece`). Use preset `dev` for a
full build with plugins, `dev-debug` for a debugger.

Prerequisites: a C++20 compiler (Visual Studio 2022 on Windows, Xcode command
line tools on macOS, GCC on Linux), CMake 3.22 or later, Ninja and Git. CMake
fetches JUCE and the other libraries. On Linux the build also needs the X11,
ALSA and font development packages listed in `building.md`. On Windows, build
from the **x64 Native Tools Command Prompt**.

`scripts/mp_build.sh` (Linux, macOS) configures and builds a preset, through
ccache when it is installed, and prints the program's path.

The first build takes from 10 to 40 minutes, depending on the machine; later
builds compile only what changed.

## Tests

```bash
cmake --build --preset dev-fast --target mp_tests
build/dev-fast/tests/mp_tests --no-perf
build/dev-fast/tests/mp_tests --only functional.midi     # one group, by name prefix
```

The performance tests (`perf.*`) measure speed and fail on slow machines and
debug builds; `--no-perf` leaves them out.

## A change, from request to pull request

1. Read the code around the change first. Comments explain why the code is the
   way it is, often with the issue number that caused it; keep those reasons.
2. Branch from `dev`: `git checkout -b fix/<short-name> origin/dev`.
3. Make the change, and a test that fails without it. Tests live in `tests/`,
   one file per area or in `tests/test_core.cpp`.
4. Text the player reads goes through `TRANS("...")`. After adding or changing
   one, run `python tools/i18n.py --write`, add the translations in
   `resources/translations/`, and check with `python tools/i18n.py --check`.
5. A change the player sees also changes the manual page that describes it
   (`manual/src/guide/`).
6. Build, run `mp_tests --no-perf`, and try the change in the program.
7. Push the branch to a fork and open the pull request against `dev`. The
   description says what the change does for a player and how it was tested.
   CI builds and tests it on Windows, macOS, Linux, Raspberry Pi, Android and
   iOS.

A pull request that only touches the manual or the README starts no build.

## Reporting instead of changing

When the player asks for a fix the agent cannot make or test, write the
issue instead: https://github.com/bonninr/masterpiece/issues/new/choose, with
the build (**Help › About Masterpiece**), the organ, the steps, and
`masterpiece.log`. A crash report also needs the system's crash record; the
discussion "Crashes come first: how to report one" lists where each system
keeps it.
