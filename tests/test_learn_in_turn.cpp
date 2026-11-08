// Learn tabs in turn (#866): each row armed in its turn, a press learned and
// the next row armed, a tab pressed again refused, Skip, Stop putting a
// row's mapping back, and "Only tabs not mapped" passing over mapped rows.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_ui/Ui.h"

#include <cmath>
#include <memory>
#include <string>

namespace {

using mp::test::Category;

class LearnInTurnTest final : public mp::test::Test {
public:
  LearnInTurnTest() : Test("functional.ui.learn-in-turn", Category::Functional) {}
  void run() override {
    juce::MessageManager::getInstance();
    // Two manuals, a stop on each, and one coupler.
    const std::string organ =
        "[Organ]\r\nChurchName=Learn in turn\r\nHasPedals=N\r\nNumberOfManuals=2\r\n"
        "NumberOfWindchestGroups=1\r\n[WindchestGroup001]\r\nName=G\r\n"
        "[Manual001]\r\nName=Great\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=001\r\n"
        "NumberOfCouplers=1\r\nCoupler001=001\r\n"
        "[Manual002]\r\nName=Swell\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=002\r\n"
        "[Stop001]\r\nName=Principal 8\r\nNumberOfLogicalPipes=3\r\nNumberOfAccessiblePipes=3\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nPipe001=P8\\036.wav\r\nPipe002=P8\\036.wav\r\nPipe003=P8\\036.wav\r\n"
        "[Stop002]\r\nName=Viola 8\r\nNumberOfLogicalPipes=3\r\nNumberOfAccessiblePipes=3\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nPipe001=V8\\036.wav\r\nPipe002=V8\\036.wav\r\nPipe003=V8\\036.wav\r\n"
        "[Coupler001]\r\nName=Swell to Great\r\nDestinationManual=2\r\n";
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-learn-in-turn");
    dir.deleteRecursively();
    dir.createDirectory();
    // The pipes, so the stops are playable and on the panel.
    for (const char* rank : {"P8", "V8"}) {
      const juce::File wav = dir.getChildFile(rank).getChildFile("036.wav");
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
      const int console = proc.registerMidiDevice("Console");
      auto& map = proc.midiMap();
      juce::AudioBuffer<float> buf(2, 256);
      // A tab on the console: controller `cc` at 127, then a block.
      auto press = [&](int cc) {
        proc.pushMidi(console, juce::MidiMessage::controllerEvent(1, cc, 127));
        juce::MidiBuffer m;
        buf.clear();
        proc.processBlock(buf, m);
      };
      mp::ui::LearnInTurnPanel panel(proc);
      panel.setSize(600, 500);
      MP_CHECK(panel.rowCount() == 3, "two stops and a coupler, got " + std::to_string(panel.rowCount()));
      if (panel.rowCount() != 3) return;
      std::string names;
      for (int i = 0; i < panel.rowCount(); ++i) names += panel.row(i).name + "|";
      MP_CHECK(names.find("Principal") != std::string::npos && names.find("Viola") != std::string::npos &&
                   names.find("Swell to Great") != std::string::npos,
               "the rows carry the names, got " + names);
      // The controller each row is mapped to, -1 for none.
      auto cc = [&](int row) {
        const auto b = map.bindingsFor(mp::MidiTargetKind::Switch, panel.row(row).switchId);
        return b.empty() ? -1 : b.front()->source.number;
      };
      const mp::Id first = panel.row(0).switchId;

      panel.start(0);
      MP_CHECK(panel.currentRow() == 0, "Start arms the first row");
      MP_CHECK(map.learning() && map.learningTarget() == first, "the first row's switch learns");
      MP_CHECK(panel.promptText().contains(juce::String::fromUTF8(panel.row(0).name.c_str())),
               "the prompt names it: " + panel.promptText().toStdString());

      press(20);
      panel.poll();
      MP_CHECK(cc(0) == 20, "the first row learns CC 20, got " + std::to_string(cc(0)));
      MP_CHECK(!proc.shownSwitchEngaged(first), "the press that teaches does not draw the stop");
      MP_CHECK(panel.currentRow() == 1, "the window moves on to the second row");
      MP_CHECK(map.learning() && map.learningTarget() == panel.row(1).switchId, "the second row learns");

      // The first tab pressed again, to put it back up: refused.
      press(20);
      panel.poll();
      MP_CHECK(panel.currentRow() == 1, "a tab pressed again leaves the row where it is");
      MP_CHECK(cc(1) == -1, "the second row does not take the first row's tab, got " + std::to_string(cc(1)));
      MP_CHECK(cc(0) == 20, "the first row keeps its tab");
      MP_CHECK(map.learning() && map.learningTarget() == panel.row(1).switchId, "the second row asks again");

      press(21);
      panel.poll();
      MP_CHECK(cc(1) == 21, "the second row learns CC 21, got " + std::to_string(cc(1)));
      MP_CHECK(panel.currentRow() == 2, "on to the third row");
      panel.skip();
      MP_CHECK(panel.currentRow() == -1 && !map.learning(), "Skip on the last row finishes");
      MP_CHECK(panel.learnedCount() == 2, "two tabs learned, got " + std::to_string(panel.learnedCount()));
      MP_CHECK(panel.promptText().contains("2"), "the prompt says how many: " + panel.promptText().toStdString());
      MP_CHECK(midiFile.loadFileAsString().contains("cc 1 20 switch") &&
                   midiFile.loadFileAsString().contains("cc 1 21 switch"),
               "the map is saved with both tabs");

      // The tabs work once learned.
      press(20);
      MP_CHECK(proc.shownSwitchEngaged(first), "CC 20 draws the first row's stop now");

      // A row with a mapping: empty while it waits, its mapping back on Stop.
      mp::MidiBinding old;
      old.source.kind = mp::MidiSourceKind::ControlChange;
      old.source.channel = 1;
      old.source.number = 30;
      old.source.deviceId = console;
      old.trigger = mp::MidiTrigger::Toggle;
      proc.setObjectBindings(mp::MidiTargetKind::Switch, panel.row(2).switchId, {old});
      panel.start(2);
      MP_CHECK(cc(2) == -1, "the armed row waits with no mapping");
      panel.stop();
      MP_CHECK(cc(2) == 30, "Stop puts the row's mapping back, got " + std::to_string(cc(2)));
      MP_CHECK(!map.learning(), "Stop ends the learn");

      // The same tab pressed for a row that had it: learned again.
      panel.start(2);
      press(30);
      panel.poll();
      MP_CHECK(cc(2) == 30 && panel.currentRow() == -1, "the row's own tab is learned again");

      // Only tabs not mapped: every row is mapped, so there is nothing to do;
      // then one row cleared, and Start goes straight to it.
      panel.unmappedOnlyButton().setToggleState(true, juce::dontSendNotification);
      panel.start(0);
      MP_CHECK(panel.currentRow() == -1 && !map.learning(), "every row is mapped: nothing to learn");
      proc.clearObjectMapping(mp::MidiTargetKind::Switch, panel.row(1).switchId);
      panel.start(0);
      MP_CHECK(panel.currentRow() == 1, "Start passes over the mapped rows, got " +
                                            std::to_string(panel.currentRow()));
      panel.stop();
      MP_CHECK(!map.learning(), "stopped");
    }
    settings.deleteFile();
    midiFile.deleteFile();
    dir.deleteRecursively();
  }
};

static LearnInTurnTest g_learnInTurn;

}  // namespace

#endif
