// Test Sound, the organ Masterpiece generates for itself (#882): it is written
// complete or not at all, the same on any number of threads, its definition
// converts and loads with one manual, one stop and a swell, and it sounds.
#include "MpTest.h"
#include "../src/mp_core/GrandOrgueImport.h"
#include "../src/mp_core/OdfLoader.h"
#include "../src/mp_core/TestSound.h"
#ifdef MP_TEST_HAS_AUDIO
#include "../src/mp_audio/MasterpieceProcessor.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

namespace fs = std::filesystem;
using mp::test::Category;

// A few keys and a short room: the shape of the real organ in a fraction of
// the time.
mp::TestSoundOptions smallOrgan(int threads) {
  mp::TestSoundOptions o;
  o.firstKey = 36;
  o.lastKey = 48;
  o.keyStep = 6;
  o.roomSeconds = 1.0;
  o.threads = threads;
  return o;
}

fs::path freshFolder(const char* name) {
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path(ec) / name;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

std::string bytes(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

class TestSoundOrganTest final : public mp::test::Test {
public:
  TestSoundOrganTest() : Test("functional.odf.test-sound-organ", Category::Functional) {}
  void run() override {
    const fs::path one = freshFolder("mp_test_sound_1t");
    const fs::path four = freshFolder("mp_test_sound_4t");
    double last = -1.0;
    bool rising = true;
    const mp::TestSoundResult a = mp::writeTestSound(one, smallOrgan(1), [&](double p) {
      rising = rising && p >= last;
      last = p;
      return true;
    });
    MP_CHECK(a.ok, "the organ is written: " + a.error);
    MP_CHECK(rising && last == 1.0, "progress rises to 1");
    // Keys 36, 42 and 48: one in six, and the last.
    MP_CHECK(a.samples == 3, "three keys are generated, got " + std::to_string(a.samples));
    MP_CHECK(fs::exists(a.organFile) && fs::exists(a.roomFile), "the definition and the room are there");
    MP_CHECK(mp::testSoundSourceKey(37, smallOrgan(1)) == 36 && mp::testSoundSourceKey(39, smallOrgan(1)) == 36 &&
                 mp::testSoundSourceKey(40, smallOrgan(1)) == 42 && mp::testSoundSourceKey(48, smallOrgan(1)) == 48,
             "each key sounds from the nearest generated key, the lower on a tie");

    const mp::TestSoundResult b = mp::writeTestSound(four, smallOrgan(4));
    MP_CHECK(b.ok, "written on four threads: " + b.error);
    for (const char* f : {"036.wav", "042.wav", "048.wav", "room.wav"})
      MP_CHECK(bytes(one / "TestSound" / f) == bytes(four / "TestSound" / f),
               std::string(f) + " is the same on one thread and on four");

    const mp::GrandOrgueImportReport rep = mp::convertGrandOrgue(a.organFile.string());
    MP_CHECK(rep.ok, "the definition converts: " + rep.error);
    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    MP_CHECK(l.loadFromXmlString(rep.xml, a.organFile.string(), o, m, d), "and loads");
    MP_CHECK(m.keyboards.size() == 1, "one manual");
    MP_CHECK(m.stops.size() == 1, "one stop");
    MP_CHECK(m.enclosures.size() == 1, "one swell");
    std::size_t pipes = 0;
    for (const auto& [id, r] : m.ranks) pipes += r.pipes.size();
    MP_CHECK(pipes == 13, "a pipe for every key, got " + std::to_string(pipes));

    // Stopped part way, it leaves nothing behind: no definition, no folder.
    const fs::path cut = freshFolder("mp_test_sound_cut");
    std::atomic<int> calls{0};
    const mp::TestSoundResult c = mp::writeTestSound(cut, smallOrgan(2), [&](double) { return ++calls < 2; });
    MP_CHECK(!c.ok, "a cancelled generation reports it");
    MP_CHECK(!fs::exists(cut / "TestSound.organ") && !fs::exists(cut / "TestSound"), "and leaves nothing");

    std::error_code ec;
    fs::remove_all(one, ec);
    fs::remove_all(four, ec);
    fs::remove_all(cut, ec);
  }
};
TestSoundOrganTest g_testSoundOrgan;

#ifdef MP_TEST_HAS_AUDIO
class TestSoundPlaysTest final : public mp::test::Test {
public:
  TestSoundPlaysTest() : Test("functional.playing.test-sound-plays", Category::Functional) {}
  void run() override {
    const fs::path dir = freshFolder("mp_test_sound_play");
    const mp::TestSoundResult r = mp::writeTestSound(dir, smallOrgan(0));
    MP_CHECK(r.ok, "the organ is written: " + r.error);
    {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(44100.0, 256);
      const juce::File odf(juce::String::fromUTF8(r.organFile.u8string().c_str()));
      const juce::File settings = proc.settingsFileFor(odf);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
      juce::AudioBuffer<float> buf(2, 256);
      const auto peakOver = [&](int blocks, juce::MidiBuffer first) {
        float peak = 0.0f;
        for (int b = 0; b < blocks; ++b) {
          buf.clear();
          juce::MidiBuffer midi;
          if (b == 0) midi = first;
          proc.processBlock(buf, midi);
          peak = std::max(peak, buf.getMagnitude(0, buf.getNumSamples()));
        }
        return peak;
      };
      peakOver(8, {});
      juce::MidiBuffer on;
      // The organ's channels count from the pedal: with no pedal, its one
      // manual is channel 2.
      on.addEvent(juce::MidiMessage::noteOn(2, 40, 0.8f), 0);
      // Key 40 has no file of its own: it plays 42's, retuned.
      const float peak = peakOver(80, on);
      MP_CHECK(peak > 0.01f && peak < 1.0f, "a key sounds, peak " + std::to_string(peak));
      MP_CHECK(proc.voiceStats().activeVoices >= 1, "and its pipe speaks");
      settings.deleteFile();
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};
TestSoundPlaysTest g_testSoundPlays;
#endif

}  // namespace
