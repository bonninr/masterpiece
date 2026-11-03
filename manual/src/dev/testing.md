# Testing

This page covers the test binary, how to build and run all or part of it, the fixtures, what continuous integration runs on every pull request, and how to add a test. The suite needs no sample library and runs in about a minute on a desktop, so run it before every pull request.

## The test binary

All tests are in one executable, `mp_tests`, built from `tests/`:

| File | Content |
|---|---|
| `tests/MpTest.h`, `tests/MpTest.cpp` | The framework: `Test`, `Category`, `Failure`, `registry()`, `runAll()`, `measure()` and the `MP_CHECK` macro |
| `tests/test_core.cpp` | Most tests, and `main()` |
| `tests/test_split.cpp` | The split keyboard: zones, layers, bass, sustain, follow, on and off, presets, text files, exclusive input |
| `tests/test_*.cpp` | One file per area or fix: the control API and its transports, the sample codecs and coded playback, the score, the recorder, live levels, the EQ per rank, the output limiter, settings archives, sparse loading and others |
| `tests/fixtures/` | Synthetic organ definitions and MusicXML scores |

`tests/CMakeLists.txt` collects every `tests/test_*.cpp` with a glob, so a new test file needs no line there. It links `mp_core`, `mp_control`, `mp_archive` and `mp_split` always, and adds `mp_sampler`, `mp_dsp` and `mp_audio` when those targets exist. It defines a macro for each optional part, and a test that needs one guards itself with it and compiles to nothing where that part is absent:

| Macro | Defined when | Guards |
|---|---|---|
| `MP_TEST_HAS_SAMPLER` | `mp_sampler` is built | Voice engine, streaming and disk tests |
| `MP_TEST_HAS_DSP` | `mp_dsp` is built (`MP_ENABLE_DSP=ON`) | Tremulant and enclosure tests |
| `MP_TEST_HAS_AUDIO` | `mp_audio` is built (JUCE available) | Tests that load an organ into `MasterpieceProcessor`, console, language and convolution tests |
| `MP_TEST_HAS_SCORE` | `mp_audio` and `mp_score` are built | The score panel's engraving and playback; `MP_TEST_SCORE_DATA_DIR` names the font folder |
| `MP_TEST_HAS_API` | `mp_audio` is built and `MP_ENABLE_API` is on | The control API end to end, over HTTP, SysEx and OSC |

Three paths reach the tests as compile definitions: `MP_TEST_FIXTURES_DIR`, `MP_TEST_TRANSLATIONS_DIR` and `MP_TEST_SOURCE_DIR`. They are absolute, so the binary runs from any working directory.

### Categories

Each test has a category and a name of the form `category.area.what`, for example `functional.odf.detect-type` or `perf.voice.polyphony`.

| Category | Names | Content |
|---|---|---|
| `Functional` | `functional.*`, `probe.*` | Pass or fail checks, about 400 of them, across the areas `control`, `voice`, `midi`, `odf`, `archive`, `samples`, `split`, `ui`, `settings`, `fixtures`, `dsp`, `codm`, `audio`, `loader`, `api`, `score`, `i18n` and others. How many run depends on the parts the build has |
| `Perf` | `perf.*` | Nine benchmarks: ODF scan throughput, voice polyphony (single-threaded, threaded, compact storage, vector units), chord start, playback of coded samples, split Rice decoding and temperament throughput. Each prints its metric and asserts a floor, relaxed in Debug builds (`#ifdef NDEBUG`) |

## Running

Build and run:

```bash
cmake --build --preset dev --target mp_tests
build/dev/tests/mp_tests --no-perf
```

The binary takes these options:

| Option | Effect |
|---|---|
| (none) | Runs every registered test, functional and performance |
| `--no-perf` | Runs the `Functional` tests only |
| `--perf-only` | Runs the `Perf` tests only |
| `--only <prefix>` | Runs only the tests whose names start with the prefix, for example `--only functional.api.` | It prints `[PASS] name` or `[FAIL] name` with the message on the next line for each test, then `N run, M failed`, and exits with 1 when any test failed. Any other exception fails the test with `unexpected exception: ...`.

With CTest, the binary is registered twice:

```bash
ctest --preset dev                  # both
ctest --preset dev -R MpFunctional  # mp_tests --no-perf
ctest --preset dev -R MpPerf        # mp_tests --perf-only
```

Test presets exist for `dev`, `dev-fast`, `core-only`, `ci-linux`, `ci-macos`, `ci-windows`, `ci-functional`, `ci-perf` and `ci-linux-arm`. Run performance tests from a Release build; the floors are meant for optimised code, and the voice benchmarks print the real-time ratio they reach.

The suite keeps its settings and its sample cache apart from the player's. At start, `main()` empties `mp-tests-data` in the system temporary folder and points the settings folder at it (`mp::setDataFolder`), and points the sample cache at `mp-tests-cache` there, so a test that saves a setting or writes a cache never reaches the player's files. A test that starts the control API sets **This computer only**, so the server listens on `127.0.0.1` and opens no firewall prompt.

The preset `ci-functional` builds Debug with AddressSanitizer, UndefinedBehaviorSanitizer and gcov coverage; `ci-perf` builds Release for `MpPerf`. See [Building from source](building.md#presets).

## Fixtures

All fixtures in `tests/fixtures/` are written for this project: synthetic organ definitions and short MusicXML scores.

| File | Purpose |
|---|---|
| `minimal.Organ_Hauptwerk_xml` | The smallest full definition that loads |
| `recorder-keyboards.Organ_Hauptwerk_xml` | Several keyboards, for recording and playing back a take on the keyboard it was played on |
| `organ-trio.musicxml`, `titled-tempo.musicxml`, `two-bars.musicxml`, `whole-notes.musicxml` | Scores for the score panel's engraving and playback |
| `minimal.CustomOrgan_Hauptwerk_xml` | The smallest compact definition |
| `example2-equivalent.CustomOrgan_Hauptwerk_xml` | A small compact definition that exercises every compact-definition table the compiler handles |
| `edge.unknown-tables.Organ_Hauptwerk_xml` | Unknown tables: loads with warnings |
| `edge.encrypted.Organ_Hauptwerk_xml` | Encrypted samples: loads, sets the flag, lists them |
| `edge.dangling-ids.Organ_Hauptwerk_xml` | Ids that point at nothing: loads and reports them |
| `m22.tuning.Organ_Hauptwerk_xml` | Temperament and pitch cases |
| `m24.expression.Organ_Hauptwerk_xml` | Continuous controls, linkages and shoes |

Rules for fixtures:

- Write your own synthetic definitions. Never copy fixtures from other projects, and never commit a licensed sample set; sets a developer owns stay on that developer's machine.
- Audio is generated at test time: `voicetest::makeTone` in `test_core.cpp` synthesises resident samples, and tests that need files write them to the system temporary folder and delete them. No audio file is committed.
- Many tests build XML in a string and load it with `OdfLoader::loadFromXmlString`, so a case needs a fixture file only when the file itself matters.
- The helpers `readFixture(name)` and `loadFixture(name, model, diagnostics)` in `test_core.cpp` read from `MP_TEST_FIXTURES_DIR`.

## Translations in the tests

Two tests keep the language files in step with the code (`functional.i18n.*`). `language-files` reads every `TRANS("...")` string in the `.cpp`, `.h` and `.mm` files under `src/` and `apps/`, the way `tools/i18n.py` reads them. It requires that Spanish, German, Dutch, Polish, Czech, French and Italian are present and that every language file reads, names its language, lists its countries, has no byte order mark, contains every string of the sources with a non-empty translation and the same placeholders, and contains no string the sources no longer have. `choose-language` checks that choosing a language changes what `TRANS()` returns.

The sources are the list of strings: no generated list is kept in the tree, so two changes that each add a string do not collide. `python tools/i18n.py` reports what each language lacks, `--check` exits with 1 when anything is missing, stale or inconsistent, and `--write` writes `resources/translations/strings.txt`, the template translators start from, which Git ignores.

## What CI runs

`.github/workflows/build.yml` runs on every push to `main`, on every pull request, on demand and when `release.yml` calls it. A new push to the same ref cancels the older run. Changes that touch only `README.md`, `COPYING`, `LICENCE`, `.gitignore`, `screenshots/**`, `site/**`, `manual/**`, `ATTRIBUTION.md`, `.github/pages-downloads.py` or `.github/workflows/pages.yml` start no build.

The `build` job runs once per row of this matrix:

| Runner | Preset | Result |
|---|---|---|
| `ubuntu-22.04` | `ci-linux` | `.deb` for amd64 |
| `windows-latest` | `ci-windows` | Setup program and portable files |
| `ubuntu-22.04` | `ci-linux-arm64` | Cross-built `.deb` for arm64 |
| `ubuntu-22.04` | `ci-linux-arm` | Cross-built `.deb` for armhf |
| `macos-15` | `ci-macos` | Apple silicon app, deployment target 11.0 |
| `macos-15` | `ci-macos` with `-DCMAKE_OSX_ARCHITECTURES=x86_64` | Intel app, deployment target 10.15; its tests and launch run under Rosetta. Left out of pull requests |

Every row configures with `-DMP_BUILD_PLUGIN=OFF`; the plugins have a job of their own. A pull request configures with `-DMP_ENABLE_LTO=OFF`, `main` and releases with link-time optimisation.

Ubuntu 22.04 is used because a binary needs the glibc it was built against or newer; the packages built there run on Debian 12 and Raspberry Pi OS.

Steps of each row, in order:

1. Restore the ccache and the Verovio compiler cache (saved from `main` only).
2. Install dependencies; for ARM rows, run `cmake/ci-arm-multiarch.sh`. The native Linux row runs `python3 tools/gen-protocol.py --check`.
3. Configure with `cmake --preset <preset>` and build with `cmake --build --preset <preset>`.
4. **Tests.** On every row that can run its own binary, find `build/<preset>/tests/mp_tests*` and run it with `--no-perf`. The ARM rows skip this step because the binary cannot run on the build host. The performance tests never run in CI.
5. **ARM output check.** `file` must report `ARM aarch64` or `ARM, EABI`, so a toolchain that silently fell back to the host compiler fails the job.
6. **Launch check.** Linux starts the app under `xvfb-run` for 20 seconds and expects it to still be running (exit code 124 from `timeout`). Windows starts `Masterpiece.exe` and requires it to be alive after 15 seconds. macOS does the same for 15 seconds, and also checks the binary's architecture and the oldest macOS it asks for (`minos`) against the deployment target of the row.
7. **Installers.** Windows builds the NSIS setup program, installs it silently, checks that exactly `bin\Masterpiece.exe` and `Uninstall.exe` are installed, that the program is registered for Add/Remove Programs and has a Start menu shortcut, starts the installed program for 15 seconds, uninstalls, and checks that the files, the registry entry and the shortcut are gone. Linux builds the `.deb`, checks its contents and installs it in `debian:12` and `ubuntu:24.04` containers (emulated with QEMU for ARM), checks that every library resolves and removes it. Native Linux also starts the installed package under `xvfb`. macOS zips the app.
8. Upload the artifacts (kept for 3 days). Windows and Linux pack the program in a `Masterpiece` folder.

Separate jobs in the same workflow:

| Job | Runner | Content |
|---|---|---|
| `targets` | `ubuntu-latest` | Writes the lists of rows for `build` and `plugins`, leaving out the rows a pull request skips |
| `plugins` | `ubuntu-22.04`, `windows-latest`, `macos-15` | Builds the VST3 and LV2 plugins on Linux and Windows and the VST3 and AU plugins on both Macs, with `-DMP_BUILD_STANDALONE=OFF`, checks the binaries and packs them. A pull request leaves out both Macs |
| `flatpak` | `ubuntu-latest` in the freedesktop 24.08 container | Builds `masterpiece-linux-x86_64.flatpak` from `packaging/flatpak` |
| `ios` | `macos-15` | Builds the app for the simulator, then plays generated test organs, records the output inside the app and checks it against the numbers the organs were made from, taps through the interface and opens a package through the document picker. A pull request runs the large iPad; `main` runs three devices. Skipped on release tags |
| `ios-device` | `macos-15` | Builds the unsigned device `.ipa` |
| `android` | `ubuntu-22.04` | Builds the APK and the Play bundle with Gradle and checks that the APK holds `libjuce_jni.so` for `arm64-v8a` and `x86_64` |

`pages.yml` builds the project site and this manual; it runs no tests. `main-latest.yml` publishes each green build of `main` as the prerelease `main-latest`. No workflow builds the `ci-functional` or `ci-perf` presets.

## Adding a test

1. Choose the file. A new area or a fix gets a file of its own, `tests/test_<what>.cpp`, which the glob in `tests/CMakeLists.txt` picks up at the next configure. A new file needs `#include "MpTest.h"` and the module headers by their path under `src` (for example `mp_control/MidiMap.h`), and wraps everything that needs an optional part in its `MP_TEST_HAS_*` macro.
2. Derive a class from `mp::test::Test`, give it a name and a category in the constructor, and write `run()`:

```cpp
class DetectTypeTest final : public mp::test::Test {
public:
  DetectTypeTest() : Test("functional.odf.detect-type", Category::Functional) {}
  void run() override {
    MP_CHECK(mp::OdfLoader::detectType("<foo", "x.xml") == mp::OdfType::Unknown,
             "garbage should be Unknown");
  }
};
static DetectTypeTest g_detect;
```

3. The file-scope `static` instance registers the test: `Test`'s constructor appends the object to `mp::test::registry()` during static initialisation. A test without an instance never runs.
4. Use `MP_CHECK(condition, message)` for each check. It throws `Failure` with the file, the line and the message when the condition is false. Throw `mp::test::Failure` yourself for other cases. The message should say what the test expected.
5. For a performance test, use `Category::Perf`, time the work with `mp::test::measure(fn)` (wall time, and CPU time from `getrusage` or `GetProcessTimes`), print the metric with `std::printf`, and assert a floor that Debug builds can relax with `#ifdef NDEBUG`.
6. Guard code that needs a module behind the macro for that module (`#ifdef MP_TEST_HAS_AUDIO` and the others above), so the suite still builds in a configuration without it.
7. Name the test after the area: `functional.<area>.<what>`. Give a test that fixes a reported fault a comment naming the issue number.
8. Run `mp_tests --only <its name>`, then `mp_tests --no-perf`, and confirm that the new test prints `[PASS]`, and that it fails when you revert the change it covers.

For a new fixture, add a synthetic definition to `tests/fixtures/` under a name that states its purpose (`edge.<case>` for an edge case) and load it with `loadFixture`.

Registration order follows static initialisation, which the language does not define across files, so a test must not depend on another having run first.
