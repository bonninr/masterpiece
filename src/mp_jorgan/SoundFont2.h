// SoundFont 2 files, read for the organs that keep their pipes in one.
//
// A SoundFont holds its samples as one block of 16-bit audio (24-bit with
// the optional sm24 chunk) and describes how to play them in two layers of
// zones: a preset (chosen by bank and program) lists instruments, and an
// instrument lists samples, each over a key and velocity range, with
// generators that tune, loop, attenuate and shape it. This reads the file's
// tables, resolves a preset into the flat list of regions a player needs --
// the two layers combined as the format specifies -- and reads a sample's
// audio. It plays nothing and knows nothing about organs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mp {

struct Sf2Sample {
  std::string name;
  uint32_t start = 0, end = 0;          // sample points in the data block, end exclusive
  uint32_t loopStart = 0, loopEnd = 0;  // the loop, loopEnd exclusive
  uint32_t rate = 44100;
  int originalPitch = 60;               // the MIDI note the sample sounds
  int pitchCorrection = 0;              // cents to add to play it in tune
  int link = 0;                         // the other channel of a stereo pair
  int type = 1;                         // 1 mono, 2 right, 4 left, 8 linked; 0x8000 ROM
};

// One sample over a key and velocity range, with everything the two layers
// of zones say about it summed into one value each.
struct Sf2Region {
  int keyLo = 0, keyHi = 127;
  int velLo = 0, velHi = 127;
  int sample = -1;            // index into SoundFont2::samples()
  int rootKey = 60;           // the note the sample sounds as played here
  double tuneCents = 0.0;     // coarse and fine tune plus the sample's correction
  int scaleTuning = 100;      // cents per key
  int loopMode = 0;           // 0 none, 1 loop, 3 loop until release, then play on
  // Offsets into the sample, in sample points, from the address generators.
  int64_t startOffset = 0, endOffset = 0, loopStartOffset = 0, loopEndOffset = 0;
  double attenuationCb = 0.0; // centibels, as written (players differ on scaling it)
  double pan = 0.0;           // -500 left .. 500 right, tenths of a percent
  double attackSec = 0.001, holdSec = 0.001, decaySec = 0.001, releaseSec = 0.001;
  double sustainCb = 0.0;     // the volume envelope's sustain level, below full
  double filterFcCents = 13500.0;  // 13500 and above: no filter
  int exclusiveClass = 0;
  // The two LFOs: their rates (Hz) and what they swing, pitch in cents and
  // level in centibels.
  double modLfoHz = 8.176, vibLfoHz = 8.176;
  double modLfoToPitch = 0.0, vibLfoToPitch = 0.0, modLfoToVolumeCb = 0.0;
};

struct Sf2Preset {
  std::string name;
  int bank = 0, program = 0;
};

class SoundFont2 {
public:
  // Read the file's tables. The audio stays on disk until a sample is read.
  bool open(const std::string& path, std::string& error);

  const std::vector<Sf2Preset>& presets() const { return presets_; }
  const std::vector<Sf2Sample>& samples() const { return samples_; }
  int bitsPerSample() const { return sm24Offset_ ? 24 : 16; }

  // The preset with this bank and program, or -1.
  int findPreset(int bank, int program) const;

  // Every region of the preset, both layers combined.
  std::vector<Sf2Region> regions(int presetIndex) const;

  // Sample points [from, to) of the data block as floats in -1..1.
  bool readFrames(uint32_t from, uint32_t to, std::vector<float>& out) const;

  // Build a SoundFont in memory, for tests: what open() reads back.
  struct Builder {
    struct Zone {
      int keyLo = 0, keyHi = 127, sample = 0, rootKey = -1, fineTune = 0;
      int loopMode = 1, attenuationCb = 0, releaseTimecents = -12000;
      int coarseTune = 0, modEnvToPitch = 0, modLfoToVolume = 0, freqModLfo = 0;
    };
    struct Preset { std::string name; int bank = 0, program = 0; std::vector<Zone> zones; };
    struct Sample { std::string name; std::vector<int16_t> pcm; uint32_t loopStart = 0, loopEnd = 0;
                    uint32_t rate = 44100; int originalPitch = 60; int pitchCorrection = 0; };
    std::vector<Preset> presets;  // each gets an instrument of its own
    std::vector<Sample> samples;
    std::string bytes() const;
  };

private:
  struct Gen { int op; uint16_t amount; };
  struct Zone { std::vector<Gen> gens; };
  struct Item { std::string name; int bank = 0, program = 0; std::vector<Zone> zones; };

  std::string path_;
  uint64_t smplOffset_ = 0, smplBytes_ = 0, sm24Offset_ = 0;
  std::vector<Sf2Preset> presets_;
  std::vector<Item> presetItems_, instruments_;
  std::vector<Sf2Sample> samples_;
};

// Write a WAV file: 16 or 24-bit PCM, with a smpl chunk naming its unity note
// and its loop, and a cue point where its release begins (negative for none).
// Interleaved frames, `channels` of them per frame.
bool writeWav(const std::string& path, const std::vector<float>& interleaved, int channels,
              uint32_t rate, int bits, int unityNote, double unityCents, int64_t loopStart,
              int64_t loopEnd, int64_t releaseCue, std::string& error);

}  // namespace mp
