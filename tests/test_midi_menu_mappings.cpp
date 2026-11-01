// A right-click menu shows every mapping an object has, and Clear mapping
// takes all of it (#565). A drawstop with only a message sent to the console,
// or only a key, read "Not mapped", and Clear mapping left both in place.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/MidiEventDialog.h"

using mp::test::Category;

class MidiMenuMappingsTest final : public mp::test::Test {
public:
  MidiMenuMappingsTest() : Test("functional.midi.menu-shows-and-clears-all", Category::Functional) {}

  void run() override {
    juce::MessageManager::getInstance();
    // The map is saved on every change, the console's part of it for every
    // organ: written in a folder of its own.
    const auto data =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-midi-menu-mappings");
    data.deleteRecursively();
    data.createDirectory();
    const auto previous = mp::dataFolder();
    mp::setDataFolder(data);
    {
      mp::MasterpieceProcessor proc;
      const auto kind = mp::MidiTargetKind::Switch;
      const mp::Id stop = 4;
      using Lines = juce::StringArray;
      MP_CHECK(mp::ui::MidiEventDialog::sendAndKeyLines(proc.midiMap(), kind, stop).isEmpty(),
               "a stop with nothing mapped has no lines");

      mp::MidiSend lamp;
      lamp.targetKind = kind;
      lamp.targetId = stop;
      lamp.kind = mp::MidiSourceKind::Note;
      lamp.channel = 2;
      lamp.number = 36;
      proc.setObjectSends(kind, stop, {lamp});
      mp::KeyShortcut key;
      key.targetKind = kind;
      key.targetId = stop;
      key.key = "ctrl + Q";
      proc.setObjectShortcuts(kind, stop, {key});

      const Lines lines = mp::ui::MidiEventDialog::sendAndKeyLines(proc.midiMap(), kind, stop);
      MP_CHECK(lines.size() == 2, "a send and a key give two lines, got " + lines.joinIntoString(" | ").toStdString());
      MP_CHECK(lines.contains("Sends Note 36 ch 2"), "the send is named: " + lines[0].toStdString());
      MP_CHECK(lines.contains("Key ctrl + Q"), "the key is named");

      mp::MidiBinding press;
      press.source.kind = mp::MidiSourceKind::ProgramChange;
      press.source.channel = 1;
      press.source.number = 5;
      press.targetKind = kind;
      press.targetId = stop;
      proc.setObjectBindings(kind, stop, {press});
      MP_CHECK(mp::ui::MidiEventDialog::describeBinding(press) == "Program 5 ch 1",
               "a program change is named as one");

      proc.clearObjectMapping(kind, stop);
      MP_CHECK(proc.midiMap().bindingsFor(kind, stop).empty(), "Clear mapping takes the message received");
      MP_CHECK(proc.midiMap().sendsFor(kind, stop).empty(), "and the message sent");
      MP_CHECK(proc.midiMap().shortcutsFor(kind, stop).empty(), "and the key");
    }
    mp::setDataFolder(previous);
    data.deleteRecursively();
  }
};
static MidiMenuMappingsTest g_midiMenuMappings;

#endif
