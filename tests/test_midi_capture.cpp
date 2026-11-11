// One MIDI capture at a time: a map learn, a manual learned from its keys, a
// key pick, the MIDI window's Listen and its on-and-off Listen. Starting one
// ends the one waiting, a load ends any, and a learn the audio thread takes
// goes into the map on the message thread, in serviceMidiRequests().
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace {

using mp::MidiCapture;
using mp::test::Category;

// One manual, one stop with one rank, and a second switch to learn.
juce::File writeOrgan(const juce::File& root) {
  root.deleteRecursively();
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();
  {
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::FileOutputStream> os(pkg.getChildFile("tone.wav").createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, {}, 0));
    juce::AudioBuffer<float> tone(1, 4800);
    for (int i = 0; i < 4800; ++i)
      tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 130.81 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 4800);
  }
  const auto odf = defs.getChildFile("capture-state.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Capture state</Identification_Name>"
      "<Identification_UniqueOrganID>37</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1102</SwitchID><Name>Spare</Name><Latching>Y</Latching></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

const char* nameOf(MidiCapture c) {
  switch (c) {
    case MidiCapture::None: return "none";
    case MidiCapture::MapLearn: return "map learn";
    case MidiCapture::KeyboardLearn: return "keyboard learn";
    case MidiCapture::KeyPick: return "key pick";
    case MidiCapture::Listen: return "Listen";
    case MidiCapture::ListenPair: return "Listen for on and off";
  }
  return "?";
}

class MidiCaptureTest final : public mp::test::Test {
public:
  MidiCaptureTest() : Test("functional.midi.one-capture-at-a-time", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-midi-capture");
    const juce::File odf = writeOrgan(root);
    // The player's own settings and MIDI files are not this test's to change.
    struct DataFolderBack {
      juce::File previous = mp::dataFolder();
      ~DataFolderBack() { mp::setDataFolder(previous); }
    } dataBack;
    mp::setDataFolder(root.getChildFile("data"));

    std::vector<std::string> failed;
    const auto check = [&](bool ok, const std::string& what) {
      if (!ok) failed.push_back(what);
    };
    {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
      proc.setLearnNoiseFilter(false);

      juce::AudioBuffer<float> buf(2, 256);
      const auto block = [&](juce::MidiBuffer m = {}) {
        buf.clear();
        proc.processBlock(buf, m);
      };
      // From a console: device 1, so a key pick takes it too.
      const auto note = [](bool down, int n, int channel = 1) {
        juce::MidiBuffer m;
        juce::MidiMessage msg = down ? juce::MidiMessage::noteOn(channel, n, 0.8f)
                                     : juce::MidiMessage::noteOff(channel, n);
        msg.setTimeStamp(0);
        m.addEvent(msg, 0);
        return m;
      };
      const auto press = [&](int n, int channel = 1) {
        block(note(true, n, channel));
        block(note(false, n, channel));
      };

      // Each capture begun, and how it is begun.
      const std::vector<std::pair<MidiCapture, std::function<void()>>> begins = {
          {MidiCapture::MapLearn, [&] { proc.beginLearn(mp::MidiTargetKind::Switch, 1102); }},
          {MidiCapture::KeyboardLearn, [&] { proc.beginKeyboardLearn(701); }},
          {MidiCapture::KeyPick, [&] { proc.beginKeyPick(); }},
          {MidiCapture::Listen, [&] { proc.beginMidiListen(); }},
          {MidiCapture::ListenPair, [&] { proc.beginMidiListenPair(); }},
      };
      // Whether the capture's own accessor says it still waits.
      const auto waits = [&](MidiCapture c) {
        switch (c) {
          case MidiCapture::MapLearn: return proc.midiMap().learning();
          case MidiCapture::KeyboardLearn: return proc.keyboardLearning() != 0;
          case MidiCapture::KeyPick: return proc.keyPicking();
          case MidiCapture::Listen:
          case MidiCapture::ListenPair: return proc.midiListening();
          case MidiCapture::None: return false;
        }
        return false;
      };

      // Begin one, then another: the first is over, the second waits.
      for (const auto& [first, beginFirst] : begins)
        for (const auto& [second, beginSecond] : begins) {
          if (first == second) continue;
          beginFirst();
          check(proc.midiCapture() == first, std::string(nameOf(first)) + " waits once begun");
          beginSecond();
          const std::string pair = std::string(nameOf(first)) + " then " + nameOf(second);
          check(proc.midiCapture() == second, pair + ": the second waits");
          // The two Listens share one accessor: the second waiting is the
          // first over.
          const auto listens = [](MidiCapture c) {
            return c == MidiCapture::Listen || c == MidiCapture::ListenPair;
          };
          if (!(listens(first) && listens(second))) check(!waits(first), pair + ": the first is over");
          proc.cancelMidiCapture();
          check(proc.midiCapture() == MidiCapture::None, pair + ": nothing waits after cancelling");
        }

      // A map learn begun on the map itself, as tests do, is seen as one.
      proc.midiMap().beginLearn(mp::MidiTargetKind::Switch, 1102);
      check(proc.midiCapture() == MidiCapture::MapLearn, "a learn begun on the map is a map learn");
      proc.beginMidiListen();
      check(!proc.midiMap().learning(), "and Listen ends it");
      proc.cancelMidiListen();

      // A load ends every capture.
      for (const auto& [c, begin] : begins) {
        begin();
        MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads again");
        check(proc.midiCapture() == MidiCapture::None,
              std::string("a load ends the ") + nameOf(c) + ", got " + nameOf(proc.midiCapture()));
        check(proc.keyboardLearning() == 0, std::string("no manual waits after a load, ") + nameOf(c));
      }

      // A stop's learn: the audio thread takes the press and leaves the map
      // alone; serviceMidiRequests writes it.
      const auto boundTo = [&](int n) {
        for (const mp::MidiBinding* b : proc.midiMap().bindingsFor(mp::MidiTargetKind::Switch, 1102))
          if (b->source.kind == mp::MidiSourceKind::Note && b->source.number == n) return true;
        return false;
      };
      proc.beginLearn(mp::MidiTargetKind::Switch, 1102);
      const std::string mapBefore = proc.midiMap().toText();
      press(60);
      check(proc.midiMap().toText() == mapBefore, "the audio thread leaves the map as it was");
      check(proc.midiCapture() == MidiCapture::MapLearn, "the stop shows as waiting until the learn is written");
      press(62);
      proc.serviceMidiRequests();
      check(boundTo(60), "serviceMidiRequests writes the learned press");
      check(!boundTo(62), "a press after the one taken is not learned");
      check(proc.midiCapture() == MidiCapture::None, "and then nothing waits");

      // A manual's learn: two keys, written by serviceMidiRequests.
      const auto rowOf = [&](int low, int high) {
        for (const auto& b : proc.midiMap().keyboardBindings())
          if (b.keyboardId == 701 && b.lowKey == low && b.highKey == high) return true;
        return false;
      };
      proc.beginKeyboardLearn(701);
      press(48, 3);
      press(72, 3);
      check(!rowOf(48, 72), "the manual's range is not in the map before serviceMidiRequests");
      check(proc.keyboardLearning() == 701, "the manual shows as waiting until the learn is written");
      proc.serviceMidiRequests();
      check(rowOf(48, 72), "serviceMidiRequests writes the manual's range");
      check(proc.keyboardLearning() == 0 && proc.midiCapture() == MidiCapture::None, "and then nothing waits");

      // A press taken for one organ is not written into the next.
      proc.beginLearn(mp::MidiTargetKind::Switch, 1102);
      press(64);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads again");
      proc.serviceMidiRequests();
      check(!boundTo(64), "a learn taken before a load is not written after it");

      // Beginning another capture writes the learn already taken first.
      proc.beginLearn(mp::MidiTargetKind::Switch, 1102);
      press(65);
      proc.beginMidiListen();
      check(boundTo(65), "a learn taken is written when another capture begins");
      check(proc.midiCapture() == MidiCapture::Listen, "and the new capture waits");
      proc.cancelMidiCapture();

      // A console that sends a message over and over in the background, ten
      // times a second (#1029): a learn passes it over and takes the button
      // the player presses.
      // The block size the clock counts by, set as a host sets it.
      proc.setRateAndBufferSizeDetails(48000.0, 256);
      const auto scan = [&] {
        juce::MidiBuffer m;
        m.addEvent(juce::MidiMessage::controllerEvent(1, 20, 127), 0);
        block(m);
        for (int i = 0; i < 18; ++i) block();  // 18 blocks of 256 at 48 kHz, about a tenth of a second
      };
      const auto boundToScan = [&] {
        for (const mp::MidiBinding* b : proc.midiMap().bindingsFor(mp::MidiTargetKind::Switch, 1102))
          if (b->source.kind == mp::MidiSourceKind::ControlChange && b->source.number == 20) return true;
        return false;
      };
      for (int i = 0; i < 5; ++i) scan();
      proc.beginLearn(mp::MidiTargetKind::Switch, 1102);
      for (int i = 0; i < 10; ++i) scan();
      proc.serviceMidiRequests();
      check(proc.midiCapture() == MidiCapture::MapLearn, "the background message is not learned");
      press(66);
      scan();
      proc.serviceMidiRequests();
      check(boundTo(66) && !boundToScan(), "the button pressed is learned, the background message is not");
      // The MIDI window's Listen passes it over too.
      proc.beginMidiListen();
      for (int i = 0; i < 10; ++i) scan();
      mp::MidiSource heard;
      int heardValue = 0;
      check(!proc.takeHeardMidi(heard, heardValue), "Listen hears nothing in the background message");
      proc.cancelMidiCapture();
      // A message sent once is learned as before.
      for (int i = 0; i < 400; ++i) block();
      proc.beginLearn(mp::MidiTargetKind::Switch, 1102);
      scan();
      proc.serviceMidiRequests();
      check(boundToScan(), "the same message sent once, after a pause, is learned");
    }

    std::string all;
    for (const auto& f : failed) all += "\n        " + f;
    MP_CHECK(failed.empty(), std::to_string(failed.size()) + " failed:" + all);
    root.deleteRecursively();
  }
};
static MidiCaptureTest g_midiCapture;

}  // namespace

#endif
