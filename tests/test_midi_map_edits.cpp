// The MIDI map is read by the audio thread on every message it handles. The
// windows that change it -- the Manual window's rows and pistons, Settings'
// Clear all mappings -- wait for the block in progress, as the MIDI window
// does, and each one still does its job.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_ui/ManualDialog.h"
#include "../src/mp_ui/Settings.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace {

using mp::test::Category;

juce::Button* findButton(juce::Component& c, const juce::String& text) {
  for (auto* child : c.getChildren()) {
    if (auto* b = dynamic_cast<juce::Button*>(child); b != nullptr && b->getButtonText() == text) return b;
    if (auto* b = findButton(*child, text)) return b;
  }
  return nullptr;
}

juce::Button* findByTooltip(juce::Component& c, const juce::String& tip) {
  for (auto* child : c.getChildren()) {
    if (auto* b = dynamic_cast<juce::Button*>(child); b != nullptr && b->getTooltip() == tip) return b;
    if (auto* b = findByTooltip(*child, tip)) return b;
  }
  return nullptr;
}

juce::Slider* findSlider(juce::Component& c, double low, double high) {
  for (auto* child : c.getChildren()) {
    if (auto* s = dynamic_cast<juce::Slider*>(child);
        s != nullptr && s->getMinimum() == low && s->getMaximum() == high)
      return s;
    if (auto* s = findSlider(*child, low, high)) return s;
  }
  return nullptr;
}

// Runs `action` on this thread while another thread holds the audio lock, as
// the audio thread does through a block. True when the map stayed as it was
// until the block ended: the edit waited for it.
bool waitsForTheBlock(mp::MasterpieceProcessor& proc, const std::function<void()>& action) {
  const std::string before = proc.midiMap().toText();
  std::atomic<bool> holding{false};
  bool unchangedInBlock = false;
  std::thread audio([&] {
    const juce::ScopedLock block(proc.getCallbackLock());
    holding.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    unchangedInBlock = proc.midiMap().toText() == before;
  });
  while (!holding.load()) std::this_thread::yield();
  action();
  audio.join();
  return unchangedInBlock;
}

class MidiMapEditsTest final : public mp::test::Test {
public:
  MidiMapEditsTest() : Test("functional.ui.midi-map-edits-wait-for-the-block", Category::Functional) {}
  void run() override {
    juce::MessageManager::getInstance();
    const std::string organ =
        "[Organ]\r\nChurchName=Map edits\r\nHasPedals=N\r\nNumberOfManuals=2\r\n"
        "NumberOfWindchestGroups=1\r\n[WindchestGroup001]\r\nName=G\r\n"
        "[Manual001]\r\nName=Great\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=001\r\n"
        "[Manual002]\r\nName=Swell\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=0\r\n"
        "[Stop001]\r\nName=Principal 8\r\nNumberOfLogicalPipes=3\r\nNumberOfAccessiblePipes=3\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nPipe001=P8\\036.wav\r\nPipe002=P8\\036.wav\r\n"
        "Pipe003=P8\\036.wav\r\n";
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-midi-map-edits");
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
    const juce::File file = dir.getChildFile("edits.organ");
    file.replaceWithText(juce::String::fromUTF8(organ.c_str()));
    // The player's own MIDI files are not this test's to change.
    struct DataFolderBack {
      juce::File previous = mp::dataFolder();
      ~DataFolderBack() { mp::setDataFolder(previous); }
    } dataBack;
    mp::setDataFolder(dir.getChildFile("data"));
    juce::File settings, midiFile;
    std::string failed;
    const auto check = [&](bool ok, const std::string& what) {
      if (!ok) failed += (failed.empty() ? "" : "; ") + what;
    };
    {
      mp::MasterpieceProcessor proc;
      settings = proc.settingsFileFor(file);
      midiFile = proc.midiMapFileFor(file);
      MP_CHECK(proc.loadOrgan(file, 0, false).ok, "the organ loads");
      proc.prepareToPlay(48000.0, 256);
      const auto& kbs = proc.playableKeyboards();
      MP_CHECK(kbs.size() == 2, "two manuals, got " + std::to_string(kbs.size()));
      if (kbs.size() != 2) return;
      const mp::Id great = kbs[0];
      const auto rowsOf = [&](mp::Id kb) {
        int n = 0;
        for (const auto& b : proc.midiMap().keyboardBindings())
          if (b.keyboardId == kb) ++n;
        return n;
      };
      const auto pistonsOf = [&](mp::Id kb) {
        size_t n = 0;
        for (int ch = 1; ch <= 16; ++ch)
          n += proc.midiMap().bindingsFor(mp::MidiTargetKind::RouteKeyboard, mp::routeKeyboardTarget(kb, ch)).size();
        return n;
      };

      auto window = std::make_unique<mp::ui::ManualDialog>(proc, great);
      window->setSize(700, 600);

      // Add: a row more, made while no block is in progress.
      const int rows = rowsOf(great);
      juce::Button* add = findButton(*window, TRANS("Add"));
      MP_CHECK(add != nullptr, "the Keys tab has its Add button");
      check(waitsForTheBlock(proc, [&] { add->onClick(); }), "Add changed the map inside an audio block");
      check(rowsOf(great) == rows + 1, "Add gives the manual a row more: " + std::to_string(rowsOf(great)) +
                                           ", was " + std::to_string(rows));

      // A change to the selected row is applied to the map.
      juce::Slider* transpose = findSlider(*window, -48, 48);
      MP_CHECK(transpose != nullptr, "the Keys tab has its transpose");
      check(waitsForTheBlock(proc, [&] { transpose->setValue(7, juce::sendNotificationSync); }),
            "a row's change went into the map inside an audio block");
      bool moved = false;
      for (const auto& b : proc.midiMap().keyboardBindings())
        if (b.keyboardId == great && b.transpose == 7) moved = true;
      check(moved, "the selected row's transpose is now 7");

      // Remove: the selected row goes.
      const int before = rowsOf(great);
      juce::Button* remove = findButton(*window, TRANS("Remove"));
      MP_CHECK(remove != nullptr, "the Keys tab has its Remove button");
      check(waitsForTheBlock(proc, [&] { remove->onClick(); }), "Remove changed the map inside an audio block");
      check(rowsOf(great) == before - 1, "Remove takes the row away: " + std::to_string(rowsOf(great)) +
                                             ", was " + std::to_string(before));

      // The Pistons tab: one piston removed by its own button, the rest by
      // Remove all pistons.
      for (const int ch : {1, 2}) {
        mp::MidiBinding piston;
        piston.source.kind = mp::MidiSourceKind::Note;
        piston.source.channel = 16;
        piston.source.number = 60 + ch;
        piston.targetKind = mp::MidiTargetKind::RouteKeyboard;
        piston.trigger = mp::MidiTrigger::Momentary;
        piston.targetId = mp::routeKeyboardTarget(great, ch);
        proc.midiMap().bind(piston);
      }
      window.reset();
      mp::ui::ManualDialog::show(proc, great, mp::ui::ManualDialog::Tab::Pistons);
      juce::Component::SafePointer<juce::TopLevelWindow> pistonsWindow;
      for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
        if (auto* w = juce::TopLevelWindow::getTopLevelWindow(i); w != nullptr && w->getName().endsWith(" - MIDI"))
          pistonsWindow = w;
      MP_CHECK(pistonsWindow != nullptr, "the Manual window opens on its Pistons tab");
      juce::Button* one = findByTooltip(*pistonsWindow, TRANS("Remove this piston"));
      MP_CHECK(one != nullptr, "each piston has its own remove button");
      check(waitsForTheBlock(proc, [&] { one->onClick(); }), "a piston's remove changed the map inside an audio block");
      check(pistonsOf(great) == 1, "one piston is left, got " + std::to_string(pistonsOf(great)));
      juce::Button* all = findButton(*pistonsWindow, TRANS("Remove all pistons"));
      MP_CHECK(all != nullptr, "the Pistons tab has Remove all pistons");
      check(waitsForTheBlock(proc, [&] { all->onClick(); }), "Remove all pistons changed the map inside an audio block");
      check(pistonsOf(great) == 0, "no piston is left, got " + std::to_string(pistonsOf(great)));
      delete pistonsWindow.getComponent();

      // Settings, Clear all mappings: every binding goes.
      mp::MidiBinding stop;
      stop.source.kind = mp::MidiSourceKind::ControlChange;
      stop.source.channel = 1;
      stop.source.number = 20;
      stop.targetKind = mp::MidiTargetKind::ReverbWet;
      stop.targetId = 0;
      proc.midiMap().bind(stop);
      MP_CHECK(!proc.midiMap().bindings().empty(), "a binding to clear");
      {
        juce::AudioDeviceManager devices;
        mp::ui::MidiPanel panel(proc, devices, mp::ui::MidiPanel::Part::Console);
        juce::Button* clear = findButton(panel, TRANS("Clear all mappings"));
        MP_CHECK(clear != nullptr, "Settings has Clear all mappings");
        check(waitsForTheBlock(proc, [&] { clear->onClick(); }),
              "Clear all mappings changed the map inside an audio block");
        check(proc.midiMap().bindings().empty(), "Clear all mappings leaves no binding");
      }
    }
    settings.deleteFile();
    midiFile.deleteFile();
    dir.deleteRecursively();
    MP_CHECK(failed.empty(), failed);
  }
};
static MidiMapEditsTest g_midiMapEdits;

}  // namespace

#endif
