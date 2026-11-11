// The Manual window's "Learn from a key" waits for two keys. The window owns
// that learn: pressed again, the button cancels it, and closing the window
// ends it. A learn left waiting took the next two keys played anywhere and
// made them the manual's key range.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_ui/ManualDialog.h"

#include <cmath>
#include <memory>
#include <string>

namespace {

using mp::test::Category;

juce::TextButton* findButton(juce::Component& c, const juce::String& text) {
  for (auto* child : c.getChildren()) {
    if (auto* b = dynamic_cast<juce::TextButton*>(child); b != nullptr && b->getButtonText() == text) return b;
    if (auto* b = findButton(*child, text)) return b;
  }
  return nullptr;
}

class ManualWindowLearnTest final : public mp::test::Test {
public:
  ManualWindowLearnTest() : Test("functional.ui.manual-window-ends-its-learn", Category::Functional) {}
  void run() override {
    juce::MessageManager::getInstance();
    const std::string organ =
        "[Organ]\r\nChurchName=Manual learn\r\nHasPedals=N\r\nNumberOfManuals=2\r\n"
        "NumberOfWindchestGroups=1\r\n[WindchestGroup001]\r\nName=G\r\n"
        "[Manual001]\r\nName=Great\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=001\r\n"
        "[Manual002]\r\nName=Swell\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=0\r\n"
        "[Stop001]\r\nName=Principal 8\r\nNumberOfLogicalPipes=3\r\nNumberOfAccessiblePipes=3\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nPipe001=P8\\036.wav\r\nPipe002=P8\\036.wav\r\n"
        "Pipe003=P8\\036.wav\r\n";
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-manual-window-learn");
    dir.deleteRecursively();
    dir.createDirectory();
    {
      const juce::File wav = dir.getChildFile("P8").getChildFile("036.wav");
      wav.getParentDirectory().createDirectory();
      juce::WavAudioFormat fmt;
      std::unique_ptr<juce::FileOutputStream> os(wav.createOutputStream());
      std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, {}, 0));
      juce::AudioBuffer<float> tone(1, 4800);
      for (int i = 0; i < 4800; ++i)
        tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 65.41 * i / 48000.0)));
      w->writeFromAudioSampleBuffer(tone, 0, 4800);
    }
    const juce::File file = dir.getChildFile("learn.organ");
    file.replaceWithText(juce::String::fromUTF8(organ.c_str()));
    juce::File settings, midiFile;
    {
      mp::MasterpieceProcessor proc;
      settings = proc.settingsFileFor(file);
      midiFile = proc.midiMapFileFor(file);
      MP_CHECK(proc.loadOrgan(file, 0, false).ok, "the organ loads");
      proc.prepareToPlay(48000.0, 256);
      const auto& kbs = proc.playableKeyboards();
      MP_CHECK(kbs.size() == 2, "two manuals, got " + std::to_string(kbs.size()));
      if (kbs.size() != 2) return;
      const mp::Id great = kbs[0], swell = kbs[1];
      const auto rowsOf = [&](mp::Id kb) {
        int n = 0;
        for (const auto& b : proc.midiMap().keyboardBindings())
          if (b.keyboardId == kb) ++n;
        return n;
      };
      const int greatRows = rowsOf(great);

      // The button starts the learn, and pressed again while it waits, cancels it.
      auto window = std::make_unique<mp::ui::ManualDialog>(proc, great);
      window->setSize(600, 600);
      juce::TextButton* learn = findButton(*window, TRANS("Learn from a key"));
      MP_CHECK(learn != nullptr, "the Keys tab has its learn button");
      if (learn == nullptr) return;
      learn->onClick();
      MP_CHECK(proc.keyboardLearning() == great, "the button starts a learn for this manual");
      learn->onClick();
      MP_CHECK(proc.keyboardLearning() == 0, "pressed again while it waits, it cancels the learn");
      learn->onClick();
      MP_CHECK(proc.keyboardLearning() == great, "pressed a third time, it starts again");

      // Closed while it waits: the learn ends with the window.
      window.reset();
      MP_CHECK(proc.keyboardLearning() == 0, "closing the window ends its learn");

      // The next two keys play the organ and leave the manual's rows alone.
      juce::AudioBuffer<float> buf(2, 256);
      for (const int note : {40, 70}) {
        juce::MidiBuffer m;
        m.addEvent(juce::MidiMessage::noteOn(1, note, 0.8f), 0);
        m.addEvent(juce::MidiMessage::noteOff(1, note), 128);
        buf.clear();
        proc.processBlock(buf, m);
      }
      MP_CHECK(rowsOf(great) == greatRows,
               "the keys played after the window closed are no learn: rows " + std::to_string(rowsOf(great)) +
                   ", was " + std::to_string(greatRows));

      // A learn for another manual is that window's, and outlives this one.
      proc.beginKeyboardLearn(swell);
      window = std::make_unique<mp::ui::ManualDialog>(proc, great);
      window.reset();
      MP_CHECK(proc.keyboardLearning() == swell, "closing the Great's window leaves the Swell's learn waiting");
      proc.cancelKeyboardLearn();
    }
    settings.deleteFile();
    midiFile.deleteFile();
    dir.deleteRecursively();
  }
};
static ManualWindowLearnTest g_manualWindowLearn;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
