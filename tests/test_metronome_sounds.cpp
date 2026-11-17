// The metronome's sounds (#1181): the beep is the original click, sample for
// sample; the wood block and the pendulum are clicks of their own that die
// away within the beat; the sound chosen is kept with the other settings.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/Metronome.h"

#include <cmath>
#include <vector>

using mp::test::Category;

namespace {
constexpr double kRate = 48000.0;
constexpr int kBeat = 24000;  // 120 beats a minute

// Two seconds of the metronome, in blocks of 512, the left channel.
std::vector<float> render(mp::Metronome::Sound sound, bool enabled = true) {
  mp::Metronome m;
  m.prepare(kRate);
  m.setTempo(120.0);
  m.setBeatsPerBar(4);
  m.setLevel(0.5f);
  m.setSound(sound);
  m.setEnabled(enabled);
  std::vector<float> out;
  juce::AudioBuffer<float> block(2, 512);
  for (int done = 0; done < 4 * kBeat; done += 512) {
    block.clear();
    m.process(block);
    MP_CHECK(block.getSample(0, 7) == block.getSample(1, 7), "both channels carry the click");
    for (int i = 0; i < 512; ++i) out.push_back(block.getSample(0, i));
  }
  return out;
}

double energy(const std::vector<float>& s, int from, int to) {
  double e = 0.0;
  for (int i = from; i < to && i < static_cast<int>(s.size()); ++i) e += static_cast<double>(s[i]) * s[i];
  return e;
}

// Zero crossings a second over a stretch: how bright the click is.
double crossingsPerSecond(const std::vector<float>& s, int from, int to) {
  int n = 0;
  for (int i = from + 1; i < to; ++i)
    if ((s[i - 1] < 0.0f) != (s[i] < 0.0f)) ++n;
  return n * kRate / (to - from);
}
}  // namespace

class MetronomeSoundsTest final : public mp::test::Test {
public:
  MetronomeSoundsTest() : Test("functional.audio.metronome-sounds", Category::Functional) {}

  void run() override {
    using Sound = mp::Metronome::Sound;
    // The beep is the click it always was: a 1500 Hz sine on the first beat,
    // starting at full level and falling by the same factor every sample.
    const auto beep = render(Sound::beep);
    const double inc = 2.0 * juce::MathConstants<double>::pi * 1500.0 / kRate;
    const float decay = static_cast<float>(std::exp(-1.0 / (0.035 * kRate * 0.25)));
    float env = 1.0f;
    double worst = 0.0;
    for (int i = 0; i < 400; ++i) {
      const float want = static_cast<float>(std::sin(inc * i)) * env * 0.5f;
      worst = std::max(worst, static_cast<double>(std::abs(beep[i] - want)));
      env *= decay;
    }
    MP_CHECK(worst < 1e-6, "the beep is the original click, sample for sample: " + std::to_string(worst));

    const auto wood = render(Sound::woodBlock);
    const auto pendulum = render(Sound::pendulum);
    for (const auto* s : {&wood, &pendulum}) {
      const bool isWood = s == &wood;
      const std::string name = isWood ? "the wood block" : "the pendulum";
      for (int b = 0; b < 4; ++b)
        MP_CHECK(energy(*s, b * kBeat, b * kBeat + 2000) > 1.0, name + " clicks on beat " + std::to_string(b + 1));
      // The beats after the first die away well inside the beat.
      MP_CHECK(energy(*s, kBeat + 8000, 2 * kBeat) == 0.0, name + " is silent before the next beat");
      float peak = 0.0f;
      for (float v : *s) peak = std::max(peak, std::abs(v));
      MP_CHECK(peak > 0.1f && peak < 1.0f, name + " peaks between -20 and 0 dBFS at level 0.5: " + std::to_string(peak));
      MP_CHECK(render(isWood ? Sound::woodBlock : Sound::pendulum) == *s, name + " is the same every run");
    }
    MP_CHECK(energy(wood, kBeat, kBeat + 2000) != energy(beep, kBeat, kBeat + 2000), "the wood block is not the beep");
    // The pendulum's tick is brighter than the wood block; its first beat
    // is a bell that rings on for the beat's first tenth of a second.
    MP_CHECK(crossingsPerSecond(pendulum, kBeat, kBeat + 400) > crossingsPerSecond(wood, kBeat, kBeat + 400),
             "the pendulum's tick is brighter than the wood block");
    MP_CHECK(energy(pendulum, 2400, 4800) > 100.0 * energy(pendulum, kBeat + 2400, kBeat + 4800),
             "the pendulum's first beat rings on as a bell");

    // Switched off, nothing is added, whatever the sound.
    for (auto sound : {Sound::beep, Sound::woodBlock, Sound::pendulum})
      MP_CHECK(energy(render(sound, false), 0, 4 * kBeat) == 0.0, "the metronome off adds nothing");

    // A sound out of range is the beep.
    mp::Metronome m;
    m.setSound(static_cast<Sound>(7));
    MP_CHECK(m.sound() == Sound::beep, "a sound out of range is the beep");
  }
};
static MetronomeSoundsTest g_metronomeSounds;

class MetronomeSoundKeptTest final : public mp::test::Test {
public:
  MetronomeSoundKeptTest() : Test("functional.settings.metronome-sound-kept", Category::Functional) {}

  void run() override {
    const juce::File global = mp::dataFolder().getChildFile("settings.mpglobal");
    const bool had = global.existsAsFile();
    const juce::String before = had ? global.loadFileAsString() : juce::String();
    {
      mp::MasterpieceProcessor proc;
      proc.metronome().setSound(mp::Metronome::Sound::pendulum);
      proc.rememberMetronome();
    }
    {
      mp::MasterpieceProcessor proc;
      proc.loadGlobalDefaults();
      MP_CHECK(proc.metronome().sound() == mp::Metronome::Sound::pendulum, "the sound chosen comes back");
    }
    // A settings file from before the sounds holds three figures: the beep.
    {
      auto lines = juce::StringArray::fromLines(global.loadFileAsString());
      for (auto& l : lines)
        if (l.startsWith("metronome ")) l = "metronome 72.0 3 0.25";
      global.replaceWithText(lines.joinIntoString("\n"));
      mp::MasterpieceProcessor proc;
      proc.metronome().setSound(mp::Metronome::Sound::woodBlock);
      proc.loadGlobalDefaults();
      MP_CHECK(proc.metronome().sound() == mp::Metronome::Sound::beep && proc.metronome().tempo() == 72.0,
               "an older line is read, and its sound is the beep");
    }
    if (had) global.replaceWithText(before);
    else global.deleteFile();
  }
};
static MetronomeSoundKeptTest g_metronomeSoundKept;

#endif
