# Masterpiece for Android

Work in progress: the standalone app, built as an APK for 64-bit phones and
tablets (`arm64-v8a`) and the emulator (`x86_64`), Android 8.0 or later.

## Building

Needs the Android SDK with the NDK `27.2.12479018` and CMake `3.22.1`
(Android Studio's SDK Manager installs both), a JDK 17 or later, and git. On
Windows, also Visual Studio 2022 or its Build Tools with *Desktop development
with C++*.

```
cd android
./gradlew assembleRelease          # gradlew.bat on Windows
```

The APK is in `app/build/outputs/apk/release/`. Install it with
`adb install -r app/build/outputs/apk/release/app-release.apk`.

Tell Gradle where the SDK is with `ANDROID_HOME`, or a `local.properties`
file here containing `sdk.dir=<path to the SDK>`.

## How it is put together

- The native half is the repository's own CMake build, the one every desktop
  target uses. Under the NDK it builds the app as `libjuce_jni.so`, the
  library JUCE's Java side loads.
- The first build installs JUCE, at the tag `CMakeLists.txt` names, for the
  machine doing the building, into `.juce/`. A JUCE build runs a helper
  program, juceaide, and it has to run here, not on the phone. The Android
  build then finds that install with `find_package`, and Gradle compiles
  JUCE's Java glue from the same copy. The output of this step is in
  `.juce/prepare.log`.
- Release and debug builds are both signed with the debug key for now:
  installable, not publishable.
- Play release builds: the engine cannot keep up with an organ unoptimised.
