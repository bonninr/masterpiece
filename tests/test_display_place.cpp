// From #1012: a console display shared by several sends, each written at its
// place on the line as GrandOrgue does; MIDI map lines that cannot be read,
// named; and a message that moves to another control, reported.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_control/GrandOrgueMidi.h"
#include "../src/mp_control/MidiMap.h"
#include "../src/mp_core/GrandOrgueImport.h"
#include "../src/mp_core/OdfLoader.h"

#include <string>

namespace {

using mp::test::Category;

class DisplayPlaceTest final : public mp::test::Test {
public:
  DisplayPlaceTest() : Test("functional.midi.display-place", Category::Functional) {}
  void run() override {
    // Two sends write halves of one 32-character LCD; each change leaves the
    // other half as it was.
    mp::MidiSend left, right;
    left.display = right.display = mp::MidiDisplay::Lcd;
    left.number = right.number = 3;
    left.start = 0;
    left.length = 16;
    right.start = 16;
    right.length = 16;
    std::string line;
    mp::MidiMap::placeOnLine(left, "Organ name that is too long", line);
    mp::MidiMap::placeOnLine(right, "+2", line);
    MP_CHECK(line == "Organ name that +2              ", "both halves on one line: '" + line + "'");
    mp::MidiMap::placeOnLine(right, "-1", line);
    MP_CHECK(line == "Organ name that -1              ", "the right half changes alone: '" + line + "'");
    // A length of 0 runs to the line's end; a 16-character string display.
    mp::MidiSend tail;
    tail.display = mp::MidiDisplay::Text;
    tail.start = 10;
    std::string text;
    mp::MidiMap::placeOnLine(tail, "abcdefghij", text);
    MP_CHECK(text == "          abcdef", "a string display is 16 wide: '" + text + "'");

    // Saved and read back with the map.
    mp::MidiMap map;
    right.targetKind = mp::MidiTargetKind::ConsoleDisplay;
    right.targetId = 4;
    right.kind = mp::MidiSourceKind::SysEx;
    map.setSendsFor(right.targetKind, right.targetId, {right});
    mp::MidiMap back;
    MP_CHECK(back.fromText(map.toText()), "the map reads back");
    const auto sends = back.sendsFor(right.targetKind, right.targetId);
    MP_CHECK(sends.size() == 1 && sends[0].start == 16 && sends[0].length == 16,
             "the place is kept in the file");

    // From GrandOrgue: MIDISendStart and MIDISendLength.
    const std::string organ =
        "[Organ]\nChurchName=Test\nHasPedals=N\nNumberOfManuals=1\nNumberOfWindchestGroups=1\nNumberOfRanks=1\n"
        "[WindchestGroup001]\nName=Main\n"
        "[Rank001]\nName=Principal 8\nFirstMidiNoteNumber=36\nNumberOfLogicalPipes=1\nWindchestGroup=1\nPipe001=P8.wav\n"
        "[Manual001]\nName=Great\nNumberOfLogicalKeys=1\nFirstAccessibleKeyMIDINoteNumber=36\n"
        "NumberOfAccessibleKeys=1\nNumberOfStops=1\nStop001=1\n"
        "[Stop001]\nName=Principal 8\nNumberOfRanks=1\nRank001=1\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\nNumberOfAccessiblePipes=1\n";
    const auto rep = mp::convertGrandOrgueText(organ);
    MP_CHECK(rep.ok, "conversion failed: " + rep.error);
    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    MP_CHECK(l.loadFromXmlString(rep.xml, "test.organ", o, m, d), "converted definition must load");
    const std::string cmb =
        "[SetterTranspose]\nMIDISendEventType001=HWLCD\nMIDISendKey001=3\nMIDISendLowValue001=2\n"
        "MIDISendStart001=16\nMIDISendLength001=16\nNumberOfMIDISendEvents=1\n"
        "[MetronomeBPM]\nMIDISendEventType001=HWLCD\nMIDISendKey001=3\nMIDISendLowValue001=2\n"
        "NumberOfMIDISendEvents=1\n"
        "[Organ]\nChurchName=Test\n";
    mp::MidiMap imported;
    const auto r = mp::importGrandOrgueMidi(cmb, m, imported);
    MP_CHECK(r.ok, "a GrandOrgue settings file");
    int placed = 0, whole = 0;
    for (const auto& s : imported.sends()) {
      if (s.display != mp::MidiDisplay::Lcd) continue;
      if (s.start == 16 && s.length == 16) ++placed;
      if (!s.placed()) ++whole;
    }
    MP_CHECK(placed == 1, "the transpose writes the right half: " + std::to_string(placed));
    MP_CHECK(whole == 1, "the tempo, with no place given, fills the line: " + std::to_string(whole));

    // A line of a MIDI map edited by hand that cannot be read is named.
    mp::MidiMap edited;
    edited.fromText("# a comment\nbind cc 1 7 nonsense\nsend switch 5 note 1 60 0 127\n");
    MP_CHECK(edited.unreadLines().size() == 1 && edited.unreadLines()[0].number == 2,
             "the second line is named: " + std::to_string(edited.unreadLines().size()));

    // One message works one control: a controller learned for a stop is taken
    // from the swell pedal it worked, and the change says which.
    mp::MasterpieceProcessor proc;
    mp::MidiBinding pedal;
    pedal.source = {mp::MidiSourceKind::ControlChange, 1, 11, 0};
    proc.setObjectBindings(mp::MidiTargetKind::ContinuousControl, 7, {pedal});
    mp::MidiBinding tab;
    tab.source = pedal.source;
    const auto displaced = proc.setObjectBindings(mp::MidiTargetKind::Switch, 9, {tab});
    MP_CHECK(displaced.size() == 1 && displaced[0].targetKind == mp::MidiTargetKind::ContinuousControl &&
                 displaced[0].targetId == 7,
             "the swell pedal's binding is reported: " + std::to_string(displaced.size()));
    const auto again = proc.setObjectBindings(mp::MidiTargetKind::Switch, 9, {tab});
    MP_CHECK(again.empty(), "learning the same again takes nothing more");
  }
};
static DisplayPlaceTest g_displayPlace;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
