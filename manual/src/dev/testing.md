# Testing

This page covers the test binary, how to build and run all or part of it, the fixtures, what continuous integration runs on every pull request, and how to add a test. The suite needs no sample library and runs in about a second, so run it before every pull request.

## The test binary

All tests are in one executable, `mp_tests`, built from `tests/`:

| File | Content |
|---|---|
| `tests/MpTest.h`, `tests/MpTest.cpp` | The framework: `Test`, `Category`, `Failure`, `registry()`, `runAll()`, `measure()` and the `MP_CHECK` macro |
| `tests/test_core.cpp` | Most tests, and `main()` |
| `tests/test_split.cpp` | The split keyboard: zones, layers, bass, sustain, follow, on and off, presets, text files, exclusive input |
| `tests/fixtures/` | Synthetic organ definitions |

`tests/CMakeLists.txt` links `mp_core`, `mp_control`, `mp_archive` and `mp_split` always, and adds `mp_sampler`, `mp_dsp` and `mp_audio` when those targets exist. For each of the three it defines a macro that the test code uses to guard what needs it:

| Macro | Defined when | Guards |
|---|---|---|
| `MP_TEST_HAS_SAMPLER` | `mp_sampler` is built | Voice engine, streaming and disk tests |
| `MP_TEST_HAS_DSP` | `mp_dsp` is built (`MP_ENABLE_DSP=ON`) | Tremulant and enclosure tests |
| `MP_TEST_HAS_AUDIO` | `mp_audio` is built (JUCE available) | Tests that load an organ into `MasterpieceProcessor`, console, language and convolution tests |

Three paths reach the tests as compile definitions: `MP_TEST_FIXTURES_DIR`, `MP_TEST_TRANSLATIONS_DIR` and `MP_TEST_SOURCE_DIR`. They are absolute, so the binary runs from any working directory.

### Categories

Each test has a category and a name of the form `category.area.what`, for example `functional.odf.detect-type` or `perf.voice.polyphony`.

| Category | Names | Content |
|---|---|---|
| `Functional` | `functional.*`, `probe.*` | Pass or fail checks. 212 tests (210 named `functional.*`, 2 named `probe.*`) across the areas `control`, `voice`, `midi`, `odf`, `archive`, `samples`, `split`, `ui`, `settings`, `fixtures`, `dsp`, `codm`, `audio`, `loader`, `i18n` and others |
| `Perf` | `perf.*` | Six benchmarks: ODF scan throughput, voice polyphony (single-threaded, threaded, compact storage, vector units) and temperament throughput. Each prints its metric and asserts a floor, relaxed in Debug builds (`#ifdef NDEBUG`) |

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

The binary has no option to pick a single test by name. It prints `[PASS] name` or `[FAIL] name` with the message on the next line for each test, then `N run, M failed`, and exits with 1 when any test failed. Any other exception fails the test with `unexpected exception: ...`.

With CTest, the binary is registered twice:

```bash
ctest --preset dev                  # both
ctest --preset dev -R MpFunctional  # mp_tests --no-perf
ctest --preset dev -R MpPerf        # mp_tests --perf-only
```

Test presets exist for `dev`, `dev-fast`, `core-only`, `ci-linux`, `ci-macos`, `ci-windows`, `ci-functional`, `ci-perf` and `ci-linux-arm`. Run performance tests from a Release build; the floors are meant for optimised code, and the voice benchmarks print the real-time ratio they reach.

The preset `ci-functional` builds Debug with AddressSanitizer, UndefinedBehaviorSanitizer and gcov coverage; `ci-perf` builds Release for `MpPerf`. See [Building from source](building.md#presets).

## Fixtures

All fixtures are synthetic organ definitions in `tests/fixtures/`, written for this project:

| File | Purpose |
|---|---|
| `minimal.Organ_Hauptwerk_xml` | The smallest full definition that loads |
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

Three tests keep the language files in step with the code (`functional.i18n.*`). `language-files` requires that Spanish, German, Dutch, Polish, Czech, French and Italian are present and that every language file reads, has a `language:` line and a `countries:` line, has no byte order mark, contains every string of `resources/translations/strings.txt` with a non-empty translation and the same placeholders, and contains no string the template lacks. `template-covers-sources` requires that every `TRANS("...")` string in `src/mp_ui/*.cpp`, `src/mp_ui/*.h` and `apps/MasterpieceApp/Main.cpp` is in `strings.txt`; fix it with `python tools/i18n.py --write`. `choose-language` checks that choosing a language changes what `TRANS()` returns.

## What CI runs

`.github/workflows/build.yml` runs on every push to `main`, on every pull request, on demand and when `release.yml` calls it. A new push to the same ref cancels the older run. Changes that touch only `README.md`, `COPYING`, `LICENCE`, `.gitignore`, `screenshots/**`, `site/**`, `manual/**`, `ATTRIBUTION.md`, `.github/pages-downloads.py` or `.github/workflows/pages.yml` start no build.

The `build` job runs once per row of this matrix:

| Runner | Preset | Result |
|---|---|---|
| `ubuntu-22.04` | `ci-linux` | `.deb` for amd64 |
| `windows-latest` | `ci-windows` | Setup program and portable files |
| `ubuntu-22.04` | `ci-linux-arm64` | Cross-built `.deb` for arm64 |
| `ubuntu-22.04` | `ci-linux-arm` | Cross-built `.deb` for armhf |
| `macos-14` | `ci-macos` | Apple silicon app, deployment target 11.0 |
| `macos-14` | `ci-macos` with `-DCMAKE_OSX_ARCHITECTURES=x86_64` | Intel app, deployment target 10.15; its tests and launch run under Rosetta |

Ubuntu 22.04 is used because a binary needs the glibc it was built against or newer; the packages built there run on Debian 12 and Raspberry Pi OS.

Steps of each row, in order:

1. Restore the ccache (saved from `main` only).
2. Install dependencies; for ARM rows, run `cmake/ci-arm-multiarch.sh`.
3. Configure with `cmake --preset <preset>` and build with `cmake --build --preset <preset>`.
4. **Tests.** On every row that can run its own binary, find `build/<preset>/tests/mp_tests*` and run it with `--no-perf`. The ARM rows skip this step because the binary cannot run on the build host. The performance tests never run in CI.
5. **ARM output check.** `file` must report `ARM aarch64` or `ARM, EABI`, so a toolchain that silently fell back to the host compiler fails the job.
6. **Launch check.** Linux starts the app under `xvfb-run` for 20 seconds and expects it to still be running (exit code 124 from `timeout`). Windows starts `Masterpiece.exe` and requires it to be alive after 15 seconds. macOS does the same for 15 seconds, and also checks the binary's architecture and the oldest macOS it asks for (`minos`) against the deployment target of the row.
7. **Installers.** Windows builds the NSIS setup program, installs it silently, checks that exactly `bin\Masterpiece.exe` and `Uninstall.exe` are installed, that the program is registered for Add/Remove Programs and has a Start menu shortcut, starts the installed program for 15 seconds, uninstalls, and checks that the files, the registry entry and the shortcut are gone. Linux builds the `.deb`, checks its contents and installs it in `debian:12` and `ubuntu:24.04` containers (emulated with QEMU for ARM), checks that every library resolves and removes it. Native Linux also starts the installed package under `xvfb`. macOS zips the app.
8. Upload the artifacts (kept for 3 days).

Separate jobs in the same workflow:

| Job | Runner | Content |
|---|---|---|
| `flatpak` | `ubuntu-latest` in the freedesktop 24.08 container | Builds `masterpiece-linux-x86_64.flatpak` from `packaging/flatpak` |
| `ios` | `macos-15` | Builds the app for the simulator, then plays generated test organs, records the output inside the app and checks it against the numbers the organs were made from, taps through the interface and opens a package through the document picker. A pull request runs the large iPad; `main` runs three devices. Skipped on release tags |
| `ios-device` | `macos-14` | Builds the unsigned device `.ipa` |
| `android` | `ubuntu-22.04` | Builds the APK and the Play bundle with Gradle and checks that the APK holds `libjuce_jni.so` for `arm64-v8a` and `x86_64` |

`pages.yml` builds the project site and this manual; it runs no tests. No workflow builds the `ci-functional` or `ci-perf` presets.

## Adding a test

1. Choose the file. Put a test for a JUCE-free module in `test_core.cpp` or `test_split.cpp`, or add a new `.cpp` to `add_executable(mp_tests ...)` in `tests/CMakeLists.txt`. A new file needs `#include "MpTest.h"` and the module headers by their path under `src` (for example `mp_control/MidiMap.h`).
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
8. Run `mp_tests --no-perf` and confirm that the new test prints `[PASS]`, and that it fails when you revert the change it covers.

For a new fixture, add a synthetic definition to `tests/fixtures/` under a name that states its purpose (`edge.<case>` for an edge case) and load it with `loadFixture`.

Registration order follows static initialisation, which the language does not define across files, so a test must not depend on another having run first.
