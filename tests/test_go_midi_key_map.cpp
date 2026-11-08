// A GrandOrgue manual's MIDIKeyNNN lines say which key a MIDI note plays. A
// short octave is written this way: the lowest notes are sent to the keys
// they sound on a historic keyboard, and the notes left over play nothing
// (#834). The import dropped the lines, and every note played its own key.
//
// The same set names each pipe's own recording again as its release for a
// long press. GrandOrgue plays a release file from its release marker; from
// the head, the attack and sustain sounded again after the key came up.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

// One second of a 100 Hz tone: a sustain loop from 0.2 to 0.6 s, whole
// periods long, and a release marker at 0.6 s, the last 0.4 s fading out.
constexpr int kFrames = 48000;
constexpr int kReleaseAt = 28800;
void writeTone(const juce::File& file) {
  juce::StringPairArray meta;
  meta.set("MidiUnityNote", "36");
  meta.set("NumSampleLoops", "1");
  meta.set("Loop0Start", "9600");
  meta.set("Loop0End", juce::String(kReleaseAt - 1));
  meta.set("NumCuePoints", "1");
  meta.set("Cue0Identifier", "1");
  meta.set("Cue0Offset", juce::String(kReleaseAt));
  juce::WavAudioFormat fmt;
  std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
  std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
  juce::AudioBuffer<float> tone(1, kFrames);
  for (int i = 0; i < kFrames; ++i) {
    const double fade =
        i < kReleaseAt ? 1.0 : 1.0 - double(i - kReleaseAt) / double(kFrames - kReleaseAt);
    tone.setSample(0, i, static_cast<float>(0.3 * fade * std::sin(2.0 * 3.14159265 * 100.0 * i / 48000.0)));
  }
  w->writeFromAudioSampleBuffer(tone, 0, kFrames);
}

// One manual of six keys from MIDI 36. Only its first key, C, has a pipe, so
// a voice means C sounded. The map: C and C# play nothing, D plays C, D#
// plays D, as on a manual whose lowest octave is short.
std::string organText() {
  return "[Organ]\r\nChurchName=Key map\r\nHasPedals=N\r\nNumberOfManuals=1\r\n"
         "NumberOfWindchestGroups=1\r\n[WindchestGroup001]\r\nName=G\r\n"
         "[Manual001]\r\nName=Great\r\nNumberOfLogicalKeys=6\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
         "NumberOfAccessibleKeys=6\r\nNumberOfStops=1\r\nStop001=001\r\n"
         "MIDIKey036=000\r\nMIDIKey037=000\r\nMIDIKey038=036\r\nMIDIKey039=038\r\n"
         "[Stop001]\r\nName=Bourdon\r\nNumberOfLogicalPipes=6\r\nNumberOfAccessiblePipes=6\r\n"
         "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nAcceptsRetuning=N\r\n"
         "Pipe001=c.wav\r\nPipe001LoadRelease=N\r\nPipe001ReleaseCount=1\r\nPipe001Release001=c.wav\r\n"
         "Pipe002=DUMMY\r\nPipe003=DUMMY\r\nPipe004=DUMMY\r\n"
         "Pipe005=DUMMY\r\nPipe006=DUMMY\r\n";
}

class GoMidiKeyMapTest final : public mp::test::Test {
public:
  GoMidiKeyMapTest() : Test("functional.midi.grandorgue-key-map", Category::Functional) {}
  void run() override {
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-go-key-map");
    dir.deleteRecursively();
    dir.createDirectory();
    writeTone(dir.getChildFile("c.wav"));
    const juce::File file = dir.getChildFile("keymap.organ");
    file.replaceWithText(juce::String(organText()));

    {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(file);
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(file, 0, false).ok, "the organ loads");
      constexpr mp::Id kGreat = 2;  // the import's first manual
      const auto kb = proc.organModel().keyboards.find(kGreat);
      MP_CHECK(kb != proc.organModel().keyboards.end(), "the manual is imported");
      if (kb == proc.organModel().keyboards.end()) return;
      MP_CHECK(kb->second.keyForMidiNote(36) == -1 && kb->second.keyForMidiNote(37) == -1,
               "C and C# play nothing");
      MP_CHECK(kb->second.keyForMidiNote(38) == 36 && kb->second.keyForMidiNote(39) == 38,
               "D plays C and D# plays D");
      MP_CHECK(kb->second.keyForMidiNote(40) == 40, "the notes the map leaves out play their own key");

      constexpr int kBlock = 256;
      juce::AudioBuffer<float> buf(2, kBlock);
      const auto run = [&](int n, juce::MidiBuffer first = {}) {
        for (int b = 0; b < n; ++b) {
          buf.clear();
          juce::MidiBuffer m;
          if (b == 0) m = first;
          proc.processBlock(buf, m);
        }
      };
      const auto voices = [&] { return proc.voiceStats().activeVoices; };
      const auto device = [&](const juce::MidiMessage& msg) {
        // Device 1 is the split keyboard's own; a console is any other.
        proc.pushMidi(3, msg);
        run(4);
      };
      const auto n = [&] { return " (" + std::to_string(voices()) + " voice(s))"; };
      proc.setStopEngaged(2001, true);
      proc.setKeyboardForChannel(2, kGreat);

      // From a MIDI device, D sounds the C pipe and lights the C key.
      device(juce::MidiMessage::noteOn(2, 38, 0.8f));
      MP_CHECK(voices() == 1, "a device's D sounds the C pipe" + n());
      MP_CHECK(proc.keyDownOnKeyboard(kGreat, 36) && !proc.keyDownOnKeyboard(kGreat, 38),
               "the drawn manual shows C down");
      run(100);  // past the attack, into the loop
      MP_CHECK(voices() == 1, "held, the C pipe sustains on its loop" + n());
      device(juce::MidiMessage::noteOff(2, 38));
      run(100);  // 0.53 s: the release is 0.4 s from its marker, 1 s from the head
      MP_CHECK(voices() == 0, "letting go of D plays the C pipe's release from its marker" + n());
      MP_CHECK(!proc.keyDownOnKeyboard(kGreat, 36), "and the C key comes up");

      // C and C# are mapped off the manual.
      device(juce::MidiMessage::noteOn(2, 36, 0.8f));
      device(juce::MidiMessage::noteOn(2, 37, 0.8f));
      MP_CHECK(voices() == 0, "a device's C and C# play nothing" + n());
      device(juce::MidiMessage::noteOff(2, 36));
      device(juce::MidiMessage::noteOff(2, 37));

      // The host's MIDI and the on-screen keys play the manual as drawn.
      juce::MidiBuffer on, off;
      on.addEvent(juce::MidiMessage::noteOn(2, 36, 0.8f), 0);
      off.addEvent(juce::MidiMessage::noteOff(2, 36), 0);
      run(4, on);
      MP_CHECK(voices() == 1, "the host's C sounds the C pipe" + n());
      run(4, off);
      run(100);
      MP_CHECK(voices() == 0, "and lets it go" + n());

      // A short-octave binding moves the notes itself; the map is not
      // applied on top. Its E (offset 4) comes down to C.
      mp::MidiMap::KeyboardBinding shortOctave;
      shortOctave.channel = 3;
      shortOctave.lowKey = 36;
      shortOctave.highKey = 41;
      shortOctave.shortOctave = true;
      shortOctave.keyboardId = kGreat;
      proc.midiMap().addKeyboardBinding(shortOctave);
      device(juce::MidiMessage::noteOn(3, 40, 0.8f));
      MP_CHECK(voices() == 1, "a short-octave binding's E sounds the C pipe" + n());
      device(juce::MidiMessage::noteOff(3, 40));
      run(100);
      MP_CHECK(voices() == 0, "and lets it go" + n());
      settings.deleteFile();
    }
    dir.deleteRecursively();
  }
};
GoMidiKeyMapTest g_goMidiKeyMap;

}  // namespace

#endif
