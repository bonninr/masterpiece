// Every manual of a GrandOrgue set is played, the fifth and those after it as
// much as the first four (#638). Barton 3-7 has a pedal, four manuals, and
// five more for second touch, traps and toe pistons; only the pedal and the
// first four could be played, and Organ settings > MIDI listed only those.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <set>
#include <string>

namespace {

using mp::test::Category;

class GoManyManualsTest final : public mp::test::Test {
public:
  GoManyManualsTest() : Test("functional.midi.grandorgue-many-manuals", Category::Functional) {}
  void run() override {
    // A pedal and eight manuals, one stop each, the last five hidden from the
    // console as Barton's are.
    constexpr int kManuals = 8;
    std::string organ = "[Organ]\r\nChurchName=Many manuals\r\nHasPedals=Y\r\nNumberOfManuals=" +
                        std::to_string(kManuals) + "\r\n";
    for (int m = 0; m <= kManuals; ++m) {
      char sec[16];
      std::snprintf(sec, sizeof sec, "%03d", m);
      char stop[16];
      std::snprintf(stop, sizeof stop, "%03d", m + 1);
      organ += std::string("[Manual") + sec + "]\r\nName=" + (m == 0 ? "Pedal" : "Manual " + std::to_string(m)) +
               "\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\nNumberOfAccessibleKeys=3\r\n"
               "NumberOfStops=1\r\nStop001=" + std::to_string(m + 1) + "\r\nDisplayed=" + (m <= 3 ? "Y" : "N") +
               "\r\n[Stop" + stop + "]\r\nName=Stop " + std::to_string(m + 1) +
               "\r\nNumberOfLogicalPipes=1\r\nNumberOfAccessiblePipes=1\r\n"
               "FirstAccessiblePipeLogicalKeyNumber=1\r\nPipe001=S" + stop + "\\036.wav\r\n";
    }
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-go-many-manuals");
    dir.deleteRecursively();
    dir.createDirectory();
    const juce::File file = dir.getChildFile("many.organ");
    file.replaceWithText(juce::String::fromUTF8(organ.c_str()));
    {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(file);
      MP_CHECK(proc.loadOrgan(file, 0, true).ok, "the organ loads");
      const auto& playable = proc.playableKeyboards();
      MP_CHECK(playable.size() == kManuals + 1, "the pedal and all " + std::to_string(kManuals) +
                                                    " manuals can be played (" +
                                                    std::to_string(playable.size()) + " can)");
      std::set<int> channels;
      std::set<std::string> names;
      for (const auto kb : playable) {
        channels.insert(proc.channelForKeyboard(kb));
        names.insert(proc.keyboardName(kb));
      }
      MP_CHECK(channels.size() == kManuals + 1,
               "each answers to a channel of its own (" + std::to_string(channels.size()) + " channels)");
      MP_CHECK(names.count("Manual 8") == 1, "the eighth manual is among them, by its name");
      settings.deleteFile();
    }
    dir.deleteRecursively();
  }
};
GoManyManualsTest g_goManyManuals;

}  // namespace

#endif
