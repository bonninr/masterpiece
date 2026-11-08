// Test Sound: a small organ Masterpiece generates on the player's machine in
// seconds (#882), so the program sounds before a sample set is installed. One
// manual, one stop, a swell pedal and a long stone room. Nothing of it ships
// but this code.
//
// Every recorded key is a sum of fifteen ranks of harmonics, each with its
// own uneven spectrum, slow drifts in pitch and level and a wind shared by
// all of them; flues speak with a chiff, reeds bloom, the basses hit and
// settle. The sustain repeats exactly over its loop, so the loop needs no
// crossfade, and the release follows a cue point in the same file.
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace mp {

struct TestSoundOptions {
  int sampleRate = 44100;
  int firstKey = 36;  // C2
  int lastKey = 96;   // C7
  // One key in this many is generated; each serves its neighbours, retuned
  // in the definition.
  int keyStep = 3;
  int threads = 0;  // 0: every core
  double roomSeconds = 8.0;
};

struct TestSoundResult {
  bool ok = false;
  std::string error;
  std::filesystem::path organFile;  // <folder>/TestSound.organ
  std::filesystem::path roomFile;   // <folder>/TestSound/room.wav
  int samples = 0;                  // the files generated
};

// The organ's name, as its console and the organ list show it.
inline constexpr const char* kTestSoundName = "Test Sound";
// Its files' name: <folder>/TestSound.organ and the folder <folder>/TestSound.
inline constexpr const char* kTestSoundStem = "TestSound";

// Writes the organ into `folder`: TestSound.organ, and TestSound/ with one
// 16-bit WAV per generated key and room.wav, the impulse response. `progress`
// is called from the worker threads with 0..1; returning false stops the work
// and leaves nothing behind.
TestSoundResult writeTestSound(const std::filesystem::path& folder, const TestSoundOptions& options = {},
                                 const std::function<bool(double)>& progress = {});

// The generated key that sounds `key`, nearest first and the lower on a tie.
int testSoundSourceKey(int key, const TestSoundOptions& options);

}  // namespace mp
