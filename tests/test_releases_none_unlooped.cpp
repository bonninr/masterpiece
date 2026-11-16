// Releases: None on a pipe whose recording has no loop (#1126). A pipe with
// no loop and no release is struck, as a bell is, and plays its recording to
// the end. With every release left out by the player's choice, each unlooped
// pipe of a set read that way, and a short note on it sounded on for the
// whole file. Its definition gives it a release, so it stops at key-up.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

// Three seconds of a 100 Hz tone with no loop, and a release marker at 2.5 s.
constexpr int kFrames = 3 * 48000;
void writeTone(const juce::File& file) {
  juce::StringPairArray meta;
  meta.set("MidiUnityNote", "60");
  meta.set("NumCuePoints", "1");
  meta.set("Cue0Identifier", "1");
  meta.set("Cue0Offset", juce::String(kFrames - 24000));
  juce::WavAudioFormat fmt;
  std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
  std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
  juce::AudioBuffer<float> tone(1, kFrames);
  for (int i = 0; i < kFrames; ++i)
    tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 100.0 * i / 48000.0)));
  w->writeFromAudioSampleBuffer(tone, 0, kFrames);
}

// One manual, two stops on middle C: the first pipe names its recording again
// as its release, the second has none at all, as a bell or a noise is written.
// The first is a speaking stop and takes retuning; a stop that takes none is
// a noise effect and keeps its releases under Releases: None (#1161).
std::string organText() {
  std::string s =
      "[Organ]\r\nChurchName=Unlooped\r\nHasPedals=N\r\nNumberOfManuals=1\r\n"
      "NumberOfWindchestGroups=1\r\n[WindchestGroup001]\r\nName=G\r\n"
      "[Manual001]\r\nName=Great\r\nNumberOfLogicalKeys=61\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
      "NumberOfAccessibleKeys=61\r\nNumberOfStops=2\r\nStop001=001\r\nStop002=002\r\n";
  for (int st = 1; st <= 2; ++st) {
    s += "[Stop00" + std::to_string(st) + "]\r\nName=Stop " + std::to_string(st) +
         "\r\nNumberOfLogicalPipes=61\r\nNumberOfAccessiblePipes=61\r\n"
         "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\n" +
         std::string(st == 1 ? "" : "AcceptsRetuning=N\r\n");
    for (int p = 1; p <= 61; ++p) {
      char key[16];
      std::snprintf(key, sizeof key, "Pipe%03d", p);
      if (p != 25) {
        s += std::string(key) + "=DUMMY\r\n";
        continue;
      }
      s += std::string(key) + "=c.wav\r\n";
      if (st == 1) s += std::string(key) + "LoadRelease=N\r\n" + key + "ReleaseCount=1\r\n" + key + "Release001=c.wav\r\n";
      else s += std::string(key) + "LoadRelease=N\r\n";
    }
  }
  return s;
}

class ReleasesNoneUnloopedTest final : public mp::test::Test {
public:
  ReleasesNoneUnloopedTest() : Test("functional.voice.releases-none-unlooped-stops", Category::Functional) {}
  void run() override {
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-releases-none-unlooped");
    dir.deleteRecursively();
    dir.createDirectory();
    writeTone(dir.getChildFile("c.wav"));
    const juce::File file = dir.getChildFile("unlooped.organ");
    file.replaceWithText(juce::String(organText()));

    // The voices a quarter of a second after a 0.2 s note on one stop, with
    // the releases chosen.
    const auto voicesAfter = [&](mp::SampleLibrary::ReleaseChoice choice, mp::Id stop) {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(file);
      settings.deleteFile();
      proc.setReleaseChoice(choice);
      proc.prepareToPlay(48000.0, 256);
      if (!proc.loadOrgan(file, 0, false).ok) return -1;
      proc.setStopEngaged(stop, true);
      juce::AudioBuffer<float> buf(2, 256);
      const auto run = [&](int blocks, const juce::MidiMessage* msg) {
        for (int b = 0; b < blocks; ++b) {
          buf.clear();
          juce::MidiBuffer m;
          if (b == 0 && msg != nullptr) m.addEvent(*msg, 0);
          proc.processBlock(buf, m);
        }
      };
      const auto on = juce::MidiMessage::noteOn(2, 60, 0.8f);
      const auto off = juce::MidiMessage::noteOff(2, 60);
      run(38, &on);    // 0.2 s
      const int held = proc.voiceStats().activeVoices;
      run(47, &off);   // 0.25 s
      const int after = proc.voiceStats().activeVoices;
      settings.deleteFile();
      return held == 1 ? after : -1 - held;
    };

    using RC = mp::SampleLibrary::ReleaseChoice;
    const int none = voicesAfter(RC::None, 2001);
    MP_CHECK(none == 0, "Releases None: the unlooped pipe stops at key-up (" + std::to_string(none) +
                            " voice(s) a quarter of a second later)");
    const int all = voicesAfter(RC::All, 2001);
    MP_CHECK(all == 1, "Releases All: the same pipe plays its release (" + std::to_string(all) + " voice(s))");
    const int struck = voicesAfter(RC::None, 2002);
    MP_CHECK(struck == 1, "a pipe written with no release still rings on (" + std::to_string(struck) +
                              " voice(s))");
    dir.deleteRecursively();
  }
};
ReleasesNoneUnloopedTest g_releasesNoneUnlooped;

}  // namespace

#endif
