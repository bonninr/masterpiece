// A GrandOrgue pipe marked Percussive with HasIndependentRelease is a key or
// stop action noise: the attack is struck and plays to its end, and letting
// go sounds the release as a sound of its own (#978). The release sounded only
// while the attack was still playing: held longer, the key came up silent.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

// A quarter second of tone at middle C, with no loop.
void writeTone(const juce::File& file) {
  constexpr int kFrames = 12000;
  constexpr double kPitchHz = 261.63;
  juce::WavAudioFormat fmt;
  std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
  std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, {}, 0));
  juce::AudioBuffer<float> tone(1, kFrames);
  for (int i = 0; i < kFrames; ++i)
    tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * kPitchHz * i / 48000.0)));
  w->writeFromAudioSampleBuffer(tone, 0, kFrames);
}

// Stop 1, Keys: a key noise with an independent release. Stop 2, Bell:
// percussive, its release named but not independent, which GrandOrgue never
// plays. Stop 3, Action: one pipe, so it sounds with its drawstop, a stop
// action noise with an independent release.
std::string organText() {
  std::string pipes = "NumberOfLogicalPipes=2\r\nNumberOfAccessiblePipes=2\r\n"
                      "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\n"
                      "Percussive=Y\r\nAcceptsRetuning=N\r\n"
                      "Pipe001=down.wav\r\nPipe001ReleaseCount=1\r\nPipe001Release001=up.wav\r\n"
                      "Pipe002=DUMMY\r\n";
  return "[Organ]\r\nChurchName=Independent release\r\nHasPedals=N\r\nNumberOfManuals=1\r\n"
         "NumberOfWindchestGroups=1\r\n[WindchestGroup001]\r\nName=G\r\n"
         "[Manual001]\r\nName=Great\r\nNumberOfLogicalKeys=2\r\nFirstAccessibleKeyMIDINoteNumber=60\r\n"
         "NumberOfAccessibleKeys=2\r\nNumberOfStops=3\r\nStop001=001\r\nStop002=002\r\nStop003=003\r\n"
         "[Stop001]\r\nName=Keys\r\nHasIndependentRelease=Y\r\n" + pipes +
         "[Stop002]\r\nName=Bell\r\n" + pipes +
         "[Stop003]\r\nName=Action\r\nNumberOfLogicalPipes=1\r\nNumberOfAccessiblePipes=1\r\n"
         "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\n"
         "Percussive=Y\r\nHasIndependentRelease=Y\r\nAcceptsRetuning=N\r\n"
         "Pipe001=down.wav\r\nPipe001ReleaseCount=1\r\nPipe001Release001=up.wav\r\n";
}

class IndependentReleaseTest final : public mp::test::Test {
public:
  IndependentReleaseTest() : Test("functional.playing.independent-release", Category::Functional) {}
  void run() override {
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-independent-release");
    dir.deleteRecursively();
    dir.createDirectory();
    writeTone(dir.getChildFile("down.wav"));
    writeTone(dir.getChildFile("up.wav"));
    const juce::File file = dir.getChildFile("release.organ");
    file.replaceWithText(juce::String(organText()));

    {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(file);
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(file, 0, false).ok, "the organ loads");

      constexpr int kBlock = 256;  // 5.3 ms
      juce::AudioBuffer<float> buf(2, kBlock);
      float peak = 0.0f;
      const auto run = [&](int n, juce::MidiBuffer first = {}) {
        peak = 0.0f;
        for (int b = 0; b < n; ++b) {
          buf.clear();
          juce::MidiBuffer m;
          if (b == 0) m = first;
          proc.processBlock(buf, m);
          for (int c = 0; c < buf.getNumChannels(); ++c)
            for (int i = 0; i < kBlock; ++i) peak = std::max(peak, std::fabs(buf.getSample(c, i)));
        }
      };
      const auto voices = [&] { return proc.voiceStats().activeVoices; };
      const auto at = [&] {
        return " (" + std::to_string(voices()) + " voice(s), peak " + std::to_string(peak) + ")";
      };
      // The importer gives the first manual MIDI channel 2.
      juce::MidiBuffer on, off;
      on.addEvent(juce::MidiMessage::noteOn(2, 60, 0.8f), 0);
      off.addEvent(juce::MidiMessage::noteOff(2, 60), 0);

      // Keys: held past the attack's end, the key comes up and the release
      // sounds.
      proc.setStopEngaged(2001, true);
      run(4, on);
      MP_CHECK(voices() == 1, "the key strikes the attack" + at());
      run(190);  // a second; the attack is a quarter
      MP_CHECK(voices() == 0, "the attack has played to its end" + at());
      run(4, off);
      MP_CHECK(voices() == 1, "letting go after the attack ended sounds the release" + at());
      MP_CHECK(peak > 0.02f, "and it is heard" + at());
      run(190);
      MP_CHECK(voices() == 0, "the release ends with its recording" + at());

      // Let go during the attack: the attack rings on, the release sounds
      // beside it.
      run(4, on);
      run(4, off);
      MP_CHECK(voices() == 2, "letting go during the attack sounds the release beside it" + at());
      run(190);
      MP_CHECK(voices() == 0, "both end with their recordings" + at());

      // Bell: percussive with no independent release. The attack rings on,
      // and no release follows it.
      proc.setStopEngaged(2001, false);
      proc.setStopEngaged(2002, true);
      run(4, on);
      run(4, off);
      MP_CHECK(voices() == 1, "a percussive pipe rings on and sounds no release" + at());
      run(190);
      MP_CHECK(voices() == 0, "it ends with its recording" + at());
      run(4, on);
      run(190);
      run(4, off);
      MP_CHECK(voices() == 0, "held past its end, it lets go in silence" + at());
      proc.setStopEngaged(2002, false);

      // Action: drawn, the stop action sounds; pushed in a second later, its
      // release does.
      mp::Id action = 0;
      for (const auto& [id, sw] : proc.organModel().switches)
        if (sw.name == "Action") action = id;
      MP_CHECK(action != 0, "the action stop has a drawstop");
      proc.setSwitchEngaged(action, true);
      run(4);
      MP_CHECK(voices() == 1, "drawing the stop sounds its action" + at());
      run(190);
      MP_CHECK(voices() == 0, "the action has played to its end" + at());
      proc.setSwitchEngaged(action, false);
      run(4);
      MP_CHECK(voices() == 1, "pushing it in sounds its release" + at());
      MP_CHECK(peak > 0.02f, "and it is heard" + at());
      settings.deleteFile();
    }
    dir.deleteRecursively();
  }
};
IndependentReleaseTest g_independentRelease;

}  // namespace

#endif
