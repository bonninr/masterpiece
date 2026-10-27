// A manual piston learned on a keyboard moves that keyboard on one press,
// whatever the organ calls the manuals it draws (#591).
//
// A console draws its manuals as keyboards of their own and plays through the
// organ's internal keyboards, joined to them by plain unison key flow. The
// piston names the internal keyboard; the channel plays the drawn one. Two
// names for one manual, and the piston has to know they are the same.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"

using mp::test::Category;

namespace {

// The shape of St. Matthew Cheltenham: a console that draws the Great and the
// Choir, and internal keyboards the pipes hang off. Keyboard 1 is the Great as
// drawn, with no assignment code, which is what keeps it out of the list of
// keyboards a player assigns to a channel.
juce::String cheltenhamShape() {
  return R"(<?xml version="1.0"?><Hauptwerk FileFormat="Organ">)"
         R"(<ObjectList ObjectType="_General"><_General>)"
         R"(<Identification_Name>Order</Identification_Name>)"
         R"(<Identification_UniqueOrganID>78</Identification_UniqueOrganID>)"
         R"(</_General></ObjectList>)"
         R"(<ObjectList ObjectType="Keyboard">)"
         R"(<Keyboard><KeyboardID>1</KeyboardID><Name>Great</Name>)"
         R"(<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys>)"
         R"(<KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>)"
         R"(<Keyboard><KeyboardID>66</KeyboardID><Name>1_InputKbd_DivCode2</Name>)"
         R"(<DefaultInputOutputKeyboardAsgnCode>2</DefaultInputOutputKeyboardAsgnCode>)"
         R"(<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys>)"
         R"(<KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>)"
         R"(<Keyboard><KeyboardID>68</KeyboardID><Name>1_InputKbd_DivCode4</Name>)"
         R"(<DefaultInputOutputKeyboardAsgnCode>4</DefaultInputOutputKeyboardAsgnCode>)"
         R"(<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys>)"
         R"(<KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>)"
         R"(</ObjectList>)"
         R"(<ObjectList ObjectType="Division">)"
         R"(<Division><DivisionID>801</DivisionID><Name>Great</Name></Division>)"
         R"(<Division><DivisionID>803</DivisionID><Name>Choir</Name></Division>)"
         R"(</ObjectList>)"
         R"(<ObjectList ObjectType="KeyAction">)"
         // The drawn Great plays on through the internal Great.
         R"(<KeyAction><SourceKeyboardID>1</SourceKeyboardID>)"
         R"(<DestIsKeyboardNotDivision>Y</DestIsKeyboardNotDivision>)"
         R"(<DestKeyboardID>66</DestKeyboardID>)"
         R"(<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode>)"
         R"(<MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey>)"
         R"(<NumberOfKeys>61</NumberOfKeys></KeyAction>)"
         R"(<KeyAction><SourceKeyboardID>66</SourceKeyboardID>)"
         R"(<DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision>)"
         R"(<DestDivisionID>801</DestDivisionID>)"
         R"(<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode>)"
         R"(<MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey>)"
         R"(<NumberOfKeys>61</NumberOfKeys></KeyAction>)"
         R"(<KeyAction><SourceKeyboardID>68</SourceKeyboardID>)"
         R"(<DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision>)"
         R"(<DestDivisionID>803</DestDivisionID>)"
         R"(<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode>)"
         R"(<MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey>)"
         R"(<NumberOfKeys>61</NumberOfKeys></KeyAction>)"
         R"(</ObjectList>)"
         R"(</Hauptwerk>)";
}

}  // namespace

class ManualPistonOrderTest final : public mp::test::Test {
public:
  ManualPistonOrderTest() : Test("functional.midi.manual-piston-order", Category::Functional) {}

  void run() override {
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("mp-piston-order");
    dir.deleteRecursively();
    dir.createDirectory();
    const auto odf = dir.getChildFile("order.Organ_Hauptwerk_xml");
    odf.replaceWithText(cheltenhamShape());

    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");

    // The player's channel 1 plays the Great as the console draws it.
    proc.clearChannelAssignments();
    proc.setKeyboardForChannel(1, 1);
    MP_CHECK(proc.keyboardForChannel(1) == 1, "channel 1 starts on the drawn Great");

    // One piston steps the keyboard between the Great and the Choir, learned
    // the way the MIDI window learns it: the internal keyboards.
    for (mp::Id kb : {static_cast<mp::Id>(66), static_cast<mp::Id>(68)}) {
      mp::MidiBinding b;
      b.source.kind = mp::MidiSourceKind::ProgramChange;
      b.source.channel = 12;
      b.source.number = 24;
      b.targetKind = mp::MidiTargetKind::RouteKeyboard;
      b.targetId = mp::routeKeyboardTarget(kb, 1);
      proc.midiMap().bind(b);
    }

    juce::AudioBuffer<float> buf(2, 256);
    auto press = [&] {
      juce::MidiBuffer m;
      m.addEvent(juce::MidiMessage::programChange(12, 24), 0);
      proc.processBlock(buf, m);
    };

    press();
    MP_CHECK(proc.keyboardForChannel(1) == 68,
             "one press moves the keyboard on to the Choir; it is on " +
                 std::to_string(proc.keyboardForChannel(1)));
    press();
    MP_CHECK(proc.keyboardForChannel(1) == 66,
             "the next press steps it on to the Great's internal keyboard; it is on " +
                 std::to_string(proc.keyboardForChannel(1)));

    // A piston for the manual the channel already plays, named by the drawn
    // keyboard, leaves the channel where it is: moving it would only let go
    // of every held key for the same manual.
    mp::MidiBinding same;
    same.source.kind = mp::MidiSourceKind::ProgramChange;
    same.source.channel = 12;
    same.source.number = 25;
    same.targetKind = mp::MidiTargetKind::RouteKeyboard;
    same.targetId = mp::routeKeyboardTarget(1, 1);
    proc.midiMap().bind(same);
    juce::MidiBuffer m;
    m.addEvent(juce::MidiMessage::programChange(12, 25), 0);
    proc.processBlock(buf, m);
    MP_CHECK(proc.keyboardForChannel(1) == 66,
             "a piston for the manual already playing leaves the channel alone; it is on " +
                 std::to_string(proc.keyboardForChannel(1)));

    proc.releaseResources();
    dir.deleteRecursively();
  }
};
static ManualPistonOrderTest g_manualPistonOrder;

#endif  // MP_TEST_HAS_AUDIO
