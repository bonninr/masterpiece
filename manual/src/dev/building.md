# Building from source

This page covers building Masterpiece from the repository: the tools each system needs, the CMake presets and options, the app, the plugins and the tests, cross-compiling for the Raspberry Pi, the Android and iOS builds, the Flatpak, the installers and the release version. After reading it you can produce every artifact the project publishes.

## Prerequisites

Every desktop build needs:

- A C++20 compiler. `CMakeLists.txt` sets `CMAKE_CXX_STANDARD 20` as required and without extensions, so CMake refuses a compiler that lacks C++20. CI builds with MSVC from Visual Studio 2022 (`windows-latest`), the default GCC of Ubuntu 22.04 (`ubuntu-22.04`, including the ARM cross compilers) and Apple Clang (`macos-14`).
- CMake 3.22 or later (`cmake_minimum_required(VERSION 3.22)`).
- Ninja. Every desktop preset uses the Ninja generator.
- Git and network access. CMake fetches the dependencies at configure time with `FetchContent`.

CMake fetches these libraries; nothing else needs installing for them:

| Library | Version | Used by | Option |
|---|---|---|---|
| JUCE | tag `9.0.1` | audio, GUI, plugin wrappers | `MP_FETCH_JUCE`, `MP_JUCE_TAG` |
| pugixml | tag `v1.14` | the definition loader (`mp_core`) | `MP_FETCH_PUGIXML`, `MP_PUGIXML_TAG` |
| libarchive | tag `v3.8.10` | RAR 5, non-solid RAR 4 and `.orgue` packages (`mp_archive`) | `MP_FETCH_LIBARCHIVE`, `MP_LIBARCHIVE_TAG` |
| unarr | commit `9c8e898b...` | solid RAR 4 packages (`mp_archive`); only its RAR 4 reader is compiled | `MP_FETCH_UNARR`, `MP_UNARR_COMMIT` |
| WavPack | tag `5.8.1` | WavPack-compressed samples | `MP_FETCH_WAVPACK`, `MP_WAVPACK_TAG` |

libarchive is configured with every optional codec, crypto backend and command-line tool switched off; its own RAR decoders are the only ones used. WavPack is built as the decoder library only, with support for WavPack 1 to 3 files enabled.

### Windows

- Visual Studio 2022 or its Build Tools with the **Desktop development with C++** workload.
- CMake and Ninja on the `PATH`.
- Build from the **x64 Native Tools Command Prompt**, which puts the MSVC compiler on the path.

The build links the Visual C++ runtime statically (`/MT`, `/MTd` in Debug), so the executable needs no Visual C++ Redistributable on the machine that runs it. ASIO support is compiled in (`JUCE_ASIO=1`) using the ASIO headers that JUCE bundles, under the GPL option of the Steinberg ASIO licence; no SDK download is needed. The Windows installer needs NSIS (`makensis`).

### macOS

```
xcode-select --install
brew install cmake ninja
```

On Apple Clang the build adds `-fno-strict-aliasing` and defines `JUCE_USE_VDSP_FRAMEWORK=1`. The CI builds set the oldest macOS the app opens on with `-DCMAKE_OSX_DEPLOYMENT_TARGET`: `11.0` for Apple silicon and `10.15` (with `-DCMAKE_OSX_ARCHITECTURES=x86_64`) for Intel. Apple silicon and Intel are built as two separate apps.

### Debian, Ubuntu and Raspberry Pi OS

```bash
sudo apt-get install -y build-essential git cmake ninja-build pkg-config \
  libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
  libxcomposite-dev libfreetype6-dev libfontconfig1-dev libglu1-mesa-dev \
  mesa-common-dev libxi-dev libjack-jackd2-dev
```

`libjack-jackd2-dev` supplies headers only. JUCE's JACK backend (`JUCE_JACK=1`) includes `<jack/jack.h>` to compile and loads `libjack.so.0` when it scans for devices, so a machine that has no JACK library runs the program, and the `.deb` lists no JACK dependency. The embedded web browser and curl are switched off (`JUCE_WEB_BROWSER=0`, `JUCE_USE_CURL=0`), so no WebKit or GTK packages are needed.

### Other platform tools

| Target | Tools |
|---|---|
| Android | Android SDK with NDK `27.2.12479018` and CMake `3.22.1`, JDK 17 or later, Git |
| iOS | Xcode with the iOS SDK |
| Flatpak | `flatpak-builder` and the freedesktop 24.08 runtime and SDK |
| Windows installer | NSIS |
| Debian packages | `cpack` (part of CMake) |

## Presets

`CMakePresets.json` (version 6) holds the configure, build and test presets. All of them derive from the hidden preset `base`, which selects the Ninja generator, the build directory `build/<preset>`, the install directory `install/<preset>`, `MP_FETCH_JUCE=ON` and `MP_JUCE_TAG=9.0.1`.

| Preset | Configuration | What it builds | Used for |
|---|---|---|---|
| `dev` | Release | App, plugins, tests; DSP and streaming on | A full local build |
| `dev-fast` | Release | App, tests; no plugins; `MP_ENABLE_LTO=OFF` | Iteration |
| `dev-debug` | Debug | App, plugins, tests | Debugging |
| `core-only` | Release | Tests only; no app, no plugins; `MP_ENABLE_DSP=OFF` | Running the tests without compiling the DSP |
| `ios-sim` | Xcode generator | The iOS app (`CMAKE_SYSTEM_NAME=iOS`, deployment target 15.0) | iOS simulator and device builds |
| `ci-linux` | Release | As `dev` | CI, Linux x86-64 |
| `ci-macos` | Release | As `dev` | CI, macOS |
| `ci-windows` | Release | As `dev` | CI, Windows |
| `ci-functional` | Debug | Tests with `MP_BUILD_ASAN=ON` and `MP_BUILD_COVERAGE=ON` | Sanitizer and coverage runs |
| `ci-perf` | Release | Tests; DSP and streaming on | Performance tests |
| `ci-linux-arm` | Release | App for 32-bit Raspberry Pi OS, cross-compiled | CI, armhf |
| `ci-linux-arm64` | Release | App for 64-bit Raspberry Pi OS, cross-compiled | CI, arm64 |

Build presets exist for every preset except `ios-sim`. Test presets exist for `dev`, `dev-fast`, `core-only`, `ci-linux`, `ci-macos`, `ci-windows`, `ci-functional`, `ci-perf` and `ci-linux-arm`. All test presets print the output of failing tests.

The workflow `.github/workflows/build.yml` uses `ci-linux`, `ci-windows`, `ci-macos`, `ci-linux-arm`, `ci-linux-arm64` and `ios-sim`. It has no job for `ci-functional` or `ci-perf`; run those presets locally.

The two ARM presets set `CMAKE_SYSTEM_PROCESSOR` and the cross compilers (`arm-linux-gnueabihf-gcc` and `aarch64-linux-gnu-gcc`), name their toolchain file, switch off the plugins and the tests, and set `MP_ENABLE_LTO=ON` and `MP_NATIVE_ARCH=OFF`.

A file named `CMakeUserPresets.json` is ignored by Git and holds personal presets.

## CMake options

| Option | Default | Effect |
|---|---|---|
| `MP_BUILD_STANDALONE` | `ON` | Builds the standalone app, target `MasterpieceApp`. |
| `MP_BUILD_PLUGIN` | `ON` | Builds the plugins, target `MasterpiecePlugin`: VST3 and AU on macOS, VST3 and LV2 on Windows and Linux. |
| `MP_BUILD_TESTS` | `ON` | Builds `mp_tests` and registers the CTest tests when `tests/` exists. |
| `MP_ENABLE_DSP` | `ON` | Compiles `mp_dsp`: wind, tremulant, enclosure and voicing processing. It can still be switched off per organ at run time. Requires JUCE (`juce_dsp`). |
| `MP_ENABLE_STREAMING` | `ON` | Adds `src/mp_sampler` to the build. `mp_audio` includes headers from `mp_sampler`, so leave it `ON` for every build of the app or the plugins. |
| `MP_BUILD_ASAN` | `OFF` | Adds `-fsanitize=address,undefined` (GCC and Clang). |
| `MP_BUILD_COVERAGE` | `OFF` | Adds `--coverage` (GCC and Clang). |
| `MP_ENABLE_LTO` | `ON` | Turns on interprocedural optimisation in Release builds when the compiler supports it. |
| `MP_NATIVE_ARCH` | `OFF` | Compiles for the build machine's CPU: `-march=native`, or `/arch:AVX2` on MSVC. Leave it off for builds that run on other machines. |
| `MP_FETCH_JUCE` | `ON` | Fetches JUCE with `FetchContent`. With `OFF`, CMake calls `find_package(JUCE CONFIG REQUIRED)`. |
| `MP_JUCE_TAG` | `9.0.1` | The JUCE tag to fetch. |
| `MP_JUCE_REPO_URL` | the JUCE GitHub repository | Where to fetch JUCE from. |
| `MP_JUCE_SOURCE_DIR` | empty | A local JUCE checkout to use in place of a fetch. Read when `MP_FETCH_JUCE` is `ON`. |
| `MP_FETCH_PUGIXML`, `MP_FETCH_LIBARCHIVE`, `MP_FETCH_UNARR`, `MP_FETCH_WAVPACK` | `ON` | Fetch the library. With `OFF`, CMake looks for an installed copy with `find_package`. |

On Android and iOS, CMake sets `MP_MOBILE` and forces `MP_BUILD_PLUGIN` and `MP_BUILD_TESTS` to `OFF`. Phones and tablets build the app alone.

## Building the app

```bash
cmake --preset dev
cmake --build --preset dev
```

The app lands in `build/dev/apps/MasterpieceApp/MasterpieceApp_artefacts/Release/`:

| System | Product |
|---|---|
| Windows | `Masterpiece.exe` |
| macOS | `Masterpiece.app` |
| Linux | `Masterpiece` |

JUCE places artefacts in a subdirectory named after the configuration, so a Debug build writes to `.../Debug/`. To build one target, add `--target MasterpieceApp`.

For quick iteration, `dev-fast` skips the plugins and link-time optimisation:

```bash
cmake --preset dev-fast
cmake --build --preset dev-fast
```

## Building the plugins

`MasterpiecePlugin` is built when `MP_BUILD_PLUGIN` is `ON`. The presets `dev`, `dev-debug`, `ci-linux`, `ci-macos` and `ci-windows` build it; `dev-fast`, `core-only`, `ci-functional`, `ci-perf` and the ARM presets switch it off. The target declares the formats `VST3 AU` on macOS and `VST3 LV2` elsewhere. CI builds the program with `-DMP_BUILD_PLUGIN=OFF` and the plugins in a job of their own with `-DMP_BUILD_STANDALONE=OFF`, so the two sets of link-time optimised links run on separate runners. Plugin identity:

| Property | Value |
|---|---|
| Plugin code | `Msp1` |
| Manufacturer code | `Mspc` |
| LV2 URI | `https://masterpiece.audio/plugins/masterpiece` |
| Type | Synthesizer, MIDI input, no MIDI output |

The products are in `build/<preset>/apps/MasterpiecePlugin/MasterpiecePlugin_artefacts/<Configuration>/`, in the subfolders `VST3` (`Masterpiece.vst3`), `AU` (`Masterpiece.component`, macOS) and `LV2` (`Masterpiece.lv2`). The plugin's `getStateInformation` stores the path of the organ definition that is loaded, and `setStateInformation` loads that organ again; the organ's settings are kept in Masterpiece's own files.

The plugin entry point (`apps/MasterpiecePlugin/PluginEntry.cpp`) returns the same `MasterpieceProcessor` the standalone app drives. See [Architecture](architecture.md).

## Building the tests

```bash
cmake --build --preset dev --target mp_tests
build/dev/tests/mp_tests --no-perf
```

`ctest --preset dev` runs the same binary as the CTest tests `MpFunctional` (`--no-perf`) and `MpPerf` (`--perf-only`). See [Testing](testing.md).

## Cross-compiling for the Raspberry Pi

Both toolchains target a Debian or Ubuntu build host with multiarch libraries. The target libraries sit under `/usr/lib/arm-linux-gnueabihf` or `/usr/lib/aarch64-linux-gnu` on the build machine's own root, so no sysroot is needed. Both toolchain files set `CMAKE_FIND_ROOT_PATH_MODE_PROGRAM` to `NEVER`, so tools such as JUCE's `juceaide` helper run natively, and point `PKG_CONFIG_LIBDIR` at the target's `pkgconfig` directory.

| File | Target | Compiler flags | Preset |
|---|---|---|---|
| `cmake/arm64-toolchain.cmake` | 64-bit Raspberry Pi OS, `aarch64` | none: ARMv8-A is the floor and NEON is part of it, so one binary covers Pi 3 to Pi 5 | `ci-linux-arm64` |
| `cmake/arm-toolchain.cmake` | 32-bit Raspberry Pi OS, `armv7l` | `-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mthumb`: covers Pi 2 to Pi 5 in 32-bit mode and excludes the ARMv6 Pi 1 and Zero | `ci-linux-arm` |

`cmake/ci-arm-multiarch.sh <debian-arch> <triple>` prepares an Ubuntu host. CI runs it as `bash cmake/ci-arm-multiarch.sh arm64 aarch64-linux-gnu` and `bash cmake/ci-arm-multiarch.sh armhf arm-linux-gnueabihf`. The script:

1. Pins the existing apt sources to `amd64` so that apt asks the amd64-only mirror for amd64 indexes alone.
2. Adds `ports.ubuntu.com` as the source for the foreign architecture, including the `-security` suite.
3. Runs `dpkg --add-architecture` and `apt-get update`.
4. Installs `cmake`, `ninja-build`, `pkg-config` and `gcc-<triple>`, `g++-<triple>`, `binutils-<triple>`.
5. Installs the development libraries (`libasound2-dev`, the X11 libraries, `libfreetype6-dev`, `libfontconfig1-dev`, `libxi-dev`, `libjack-jackd2-dev`) for the target architecture and for the host. The host copy is required: the cross build still compiles `juceaide` natively, and it links `juce_graphics`.

The script needs `sudo` and reads `UBUNTU_CODENAME` from `/etc/os-release`. Then:

```bash
cmake --preset ci-linux-arm64
cmake --build --preset ci-linux-arm64
```

The script's package list has no GLU or Mesa packages, while the native Linux list above includes `libglu1-mesa-dev` and `mesa-common-dev`. The script's comment says Masterpiece does not use JUCE's OpenGL module and that GLU would pull in Mesa's LLVM; the CI cross builds succeed with this list.

A cross-built binary cannot run on the build host. CI checks its machine type with `file` (`ARM aarch64` or `ARM, EABI`) and installs the `.deb` in an emulated container of the target architecture.

## Android

The Android build lives in `android/` and uses Gradle around the repository's own CMake build. Targets: `arm64-v8a` and `x86_64` (phones, tablets and the emulator), Android 8.0 or later (`minSdk 26`; `compileSdk` and `targetSdk` 35). Needs the Android SDK with NDK `27.2.12479018` and CMake `3.22.1`, JDK 17 or later and Git. On Windows, add Visual Studio 2022 or its Build Tools with the C++ workload.

```
cd android
./gradlew assembleRelease          # gradlew.bat on Windows
```

The APK is `android/app/build/outputs/apk/release/app-release.apk`; install it with `adb install -r`. Set the SDK location with the `ANDROID_HOME` environment variable or a `local.properties` file in `android/` containing `sdk.dir=<path>`.

How it fits together:

- Under the NDK, CMake builds `MasterpieceApp` as `libjuce_jni.so`, the library JUCE's Java side loads. The Gradle build passes `-DMP_FETCH_JUCE=OFF` and `-DJUCE_DIR=...`, `-DANDROID_STL=c++_static`, `-DANDROID_ARM_NEON=TRUE` and related flags.
- The first build clones JUCE at the tag named by `MP_JUCE_TAG` into `android/.juce/` and installs it for the build machine. JUCE's `juceaide` helper has to run on the build machine. The Android build then finds that install with `find_package`, and Gradle compiles JUCE's Java glue from the same copy. The log is `android/.juce/prepare.log`.
- `versionName` is the CMake project version; `versionCode` is `major * 10000 + minor * 100 + patch`.
- Only release builds play organs; the engine cannot keep up unoptimised. `bundleRelease` produces the App Bundle `app-release.aab` for Google Play.
- Signing: a build is signed with the release key when `MP_ANDROID_KEYSTORE` (the keystore path) and `MP_ANDROID_KEYSTORE_PASSWORD` are set in the environment, in `~/.gradle/gradle.properties` or in the repository's `.env`. The key alias is `masterpiece`. When they are unset the build uses the debug key: installable, and it cannot update a released version. CI reads the key from the repository secrets `ANDROID_KEYSTORE_BASE64` and `ANDROID_KEYSTORE_PASSWORD`, and a tagged release fails without them.

## iOS

The `ios-sim` preset configures an Xcode project for the app alone, with deployment target 15.0. Despite the name it serves the simulator and devices; the SDK is chosen at build time. The commands CI runs:

```bash
cmake --preset ios-sim

# Simulator
cmake --build build/ios-sim --config Release --target MasterpieceApp -- \
  -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO

# Device, unsigned
cmake --build build/ios-sim --config Release --target MasterpieceApp -- \
  -sdk iphoneos -arch arm64 CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY=""
```

CI packs the device build as an unsigned `.ipa` (a ZIP with the app in a `Payload` folder). Apple runs only signed code, so the person installing it signs it with their own Apple ID; no developer account is needed to build it. The app is landscape only and registers as a document browser with file sharing enabled, so organs reach it through the Files app.

CI also runs the simulator build on iPad and iPhone devices with the scripts in `.github/ios-check/`: `make_organ.py` writes an organ of pure sine pipes with known levels, `check_audio.py` compares a recording of its playback with those numbers, and `drive_ui.py` taps through the interface by control names.

## Flatpak

`packaging/flatpak/io.github.bonninr.Masterpiece.yml` builds an x86-64 Flatpak from the checkout it sits in.

| Item | Value |
|---|---|
| App ID | `io.github.bonninr.Masterpiece` |
| Runtime and SDK | `org.freedesktop.Platform` and `org.freedesktop.Sdk`, version `24.08` |
| Command | `masterpiece` |
| Permissions | `--share=ipc`, `--socket=x11`, `--socket=pulseaudio`, `--filesystem=xdg-run/pipewire-0`, `--device=all`, `--filesystem=home`, `/media`, `/run/media`, `/mnt` |
| CMake options a Release build with the plugin, the tests and the developer tools switched off |

The first module installs the JACK headers from the jack2 `v1.9.22` archive into `/app/include/jack`; JUCE needs them to compile and opens `libjack` itself at run time. The second module builds Masterpiece with `cmake-ninja` and `--share=network`, because `FetchContent` downloads JUCE and the other libraries during the build. A Flathub submission would need those sources listed in the manifest. The manifest also installs `io.github.bonninr.Masterpiece.metainfo.xml` and renames the shared MIME file to the app ID.

CI builds it in the job `flatpak` with `flatpak/flatpak-github-actions/flatpak-builder` inside the container `ghcr.io/flathub-infra/flatpak-github-actions:freedesktop-24.08`, producing `masterpiece-linux-x86_64.flatpak`.

## Packaging

`cmake/Packaging.cmake` is included for every non-mobile build that has the standalone app. It configures CPack for one package per system and installs only the `app` component, so the install rules of JUCE and the other fetched libraries stay out of the package. The package stays monolithic: CPack's component mode produced an NSIS installer whose silent install did nothing.

Output names stay the same between releases.

| System | Generator | File |
|---|---|---|
| Windows | NSIS | `masterpiece-windows-setup.exe` |
| Debian, Ubuntu, Raspberry Pi OS | DEB | `masterpiece-linux-amd64.deb`, `masterpiece-linux-arm64.deb`, `masterpiece-linux-armhf.deb` |

Run `cpack -G NSIS` or `cpack -G DEB` inside the build directory (`build/ci-windows`, `build/ci-linux`).

The Windows installer installs `bin\Masterpiece.exe` and `Uninstall.exe`. It writes Start menu and desktop shortcuts, version information for the file's Properties and the "Open with Masterpiece" registry entries for organ definitions and packages (`packaging/openwith-install.nsh` and `openwith-uninstall.nsh`), and a new version installs over the old one. CI installs it silently (`/S /D=<folder>`), starts the installed program, uninstalls it and checks that the files, the registry entry and the shortcut are gone.

The Linux package installs:

| Path under `/usr` | Content |
|---|---|
| `bin/masterpiece` | The app, lower case |
| `share/applications/masterpiece.desktop` | The launcher |
| `share/icons/hicolor/256x256/apps/masterpiece.png` | The icon |
| `share/mime/packages/masterpiece.xml` | The organ file types |

Its section is `sound`. For native builds `dpkg-shlibdeps` works out the dependencies; cross-built packages use the list `libasound2 | libasound2t64, libfreetype6, libfontconfig1, libx11-6, libxext6, libxrandr2, libxinerama1, libxcursor1, libgl1`. CI installs each package in `debian:12` and `ubuntu:24.04` containers (emulated for ARM), checks that every library resolves, and removes it again.

macOS has no CPack package. CI copies `Masterpiece.app` into one folder and zips it with `ditto`, which keeps the bundle's executable bit. The plugins job packs `Masterpiece-plugins` folders: `Masterpiece.component` and `Masterpiece.vst3` on macOS (zipped with `ditto`), `VST3` and `LV2` folders on Windows and Linux. The release publishes them as `masterpiece-<version>-plugins-<platform>`. The macOS build is unsigned.

## Versioning

The version is the `project(Masterpiece VERSION x.y.z ...)` line in `CMakeLists.txt`. The app (`MP_VERSION`, printed by `--version`), the installer, the `.deb`, the Android `versionName` and `versionCode`, and the plugin bundles take it from there.

`cmake/set-version.sh <x.y.z>` changes that line and rewrites the download links in `README.md` (the `releases/download/vX.Y.Z/` segment and the `masterpiece-X.Y.Z-` file names), then prints the project line and the number of README references. It uses GNU `sed -i`, so run it on Linux or in Git Bash. Run it on the release branch before tagging, so the README badges point at the files the tag is about to publish.

The script does not touch the `<releases>` list in `packaging/flatpak/io.github.bonninr.Masterpiece.metainfo.xml`.

## Releasing

1. Run `cmake/set-version.sh <version>` on a branch and merge it.
2. Tag the merge commit `v<version>`.
3. `.github/workflows/release.yml` starts on a tag matching `v*`, or by hand with the input `tag` for an existing tag. It calls `build.yml`, so a release is built by the steps that verify every pull request, then downloads the artifacts.
4. The `Package` step zips or tars the loose-file artifacts, copies the finished packages (Windows setup, `.deb`, Flatpak, macOS zips, Android APK, iOS `.ipa`), and gives every file the version in its name: `masterpiece-<version>-<name>`. It stops if any file lacks the version. The Google Play bundle (`play-bundle`) is never published.
5. `gh release create` publishes the files as `Masterpiece <tag>` with `--generate-notes`; if the release exists, `gh release upload --clobber` replaces the files.
6. The last step starts `pages.yml`, which republishes the project site so its download links point at the new files.

The Android build fails on a tag unless the signing secrets are present. The iOS simulator job is skipped on tags; the `.ipa` job runs on them.
