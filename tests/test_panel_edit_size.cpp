// A panel fitted to its window keeps its size in edit mode: the hidden
// elements edit mode shows are added below, in reach by scrolling (reported
// on D#565: Edit shrank every button of the whole-organ panel).
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/Panels.h"

#include <cmath>
#include <memory>
#include <string>

namespace {

using mp::test::Category;

class PanelEditSizeTest final : public mp::test::Test {
public:
  PanelEditSizeTest() : Test("functional.ui.panel-edit-size", Category::Functional) {}
  void run() override {
    // One manual of four stops.
    std::string organ =
        "[Organ]\r\nChurchName=Edit size\r\nHasPedals=N\r\nNumberOfManuals=1\r\n"
        "[Manual001]\r\nName=Hauptwerk\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=4\r\n"
        "Stop001=1\r\nStop002=2\r\nStop003=3\r\nStop004=4\r\n";
    for (int i = 1; i <= 4; ++i)
      organ += "[Stop00" + std::to_string(i) + "]\r\nName=Stop " + std::to_string(i) +
               "\r\nNumberOfLogicalPipes=1\r\nNumberOfAccessiblePipes=1\r\n"
               "FirstAccessiblePipeLogicalKeyNumber=1\r\nPipe001=S" + std::to_string(i) + "\\036.wav\r\n";
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-panel-edit-size");
    dir.deleteRecursively();
    dir.createDirectory();
    // A pipe for each stop: a stop with no samples is left off the panel.
    for (int i = 1; i <= 4; ++i) {
      const juce::File wav = dir.getChildFile("S" + juce::String(i)).getChildFile("036.wav");
      wav.getParentDirectory().createDirectory();
      juce::WavAudioFormat fmt;
      std::unique_ptr<juce::FileOutputStream> os(wav.createOutputStream());
      std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, {}, 0));
      juce::AudioBuffer<float> tone(1, 4800);
      for (int k = 0; k < 4800; ++k)
        tone.setSample(0, k, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 65.41 * k / 48000.0)));
      w->writeFromAudioSampleBuffer(tone, 0, 4800);
    }
    const juce::File file = dir.getChildFile("editsize.organ");
    file.replaceWithText(juce::String::fromUTF8(organ.c_str()));
    {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(file);
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(file, 0, false).ok, "the organ loads");

      // Three of the four stops hidden, the panel fitted to a small window.
      mp::ui::PanelLayout layout;
      layout.fitToWindow = true;
      int stops = 0;
      for (const auto& s : mp::ui::panelSections(proc))
        for (const auto& e : s.elements)
          if (e.kind == mp::ui::PanelElement::Kind::Stop && stops++ > 0) layout.hidden.insert(e.key());
      MP_CHECK(stops == 4, "the panel holds the four stops: " + std::to_string(stops));

      juce::MessageManager::getInstance();
      mp::ui::PanelView view(proc, 0, layout);
      view.setSize(400, 300);
      const auto playing = view.cellBoundsForTest();
      MP_CHECK(!playing.empty(), "the one stop shown is drawn");

      view.setEditingForTest(true);
      const auto editing = view.cellBoundsForTest();
      MP_CHECK(editing.size() == playing.size() + 3, "edit mode shows the three hidden stops too: " +
                                                         std::to_string(editing.size()) + " cells");
      if (!playing.empty() && !editing.empty())
        MP_CHECK(editing[0].getHeight() == playing[0].getHeight() &&
                     editing[0].getWidth() == playing[0].getWidth(),
                 "edit mode keeps the stops' size: " + std::to_string(playing[0].getHeight()) + " high, then " +
                     std::to_string(editing[0].getHeight()));

      view.setEditingForTest(false);
      const auto after = view.cellBoundsForTest();
      MP_CHECK(after.size() == playing.size() && !after.empty() && after[0] == playing[0],
               "and leaving it the panel is as it was");
      settings.deleteFile();
    }
    dir.deleteRecursively();
  }
};
static PanelEditSizeTest g_panelEditSize;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
