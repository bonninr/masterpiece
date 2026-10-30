// A controller no one has learned moves nothing on the organ. A MIDI file's
// breath controller (CC2) swung the Ashton demo's Solo swell in time with the
// music, because an unmapped controller set the control whose id is its
// number; on Nancy the same file's CC100 and CC101 set the bellows pressure.
// A learned controller moves its control as before.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

namespace {

using mp::test::Category;

// One manual, one stop, and continuous controls with ids 2 and 7.
juce::File writeOrgan(const juce::File& root) {
  root.deleteRecursively();
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();
  {
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::FileOutputStream> os(pkg.getChildFile("tone.wav").createOutputStream());
    juce::StringPairArray meta;
    std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
    juce::AudioBuffer<float> tone(1, 4800);
    for (int i = 0; i < 4800; ++i)
      tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 130.81 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 4800);
  }
  const auto odf = defs.getChildFile("controllers.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Controllers</Identification_Name>"
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
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>2</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "</ObjectList><ObjectList ObjectType=\"ContinuousControl\">"
      "<ContinuousControl><ControlID>2</ControlID><Name>Swell</Name><DefaultValue>40</DefaultValue></ContinuousControl>"
      "<ContinuousControl><ControlID>7</ControlID><Name>Wind</Name><DefaultValue>64</DefaultValue></ContinuousControl>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

class UnmappedControllersTest final : public mp::test::Test {
public:
  UnmappedControllersTest() : Test("functional.midi.unmapped-controllers", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-unmapped-cc");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");

    juce::AudioBuffer<float> buf(2, 256);
    const auto send = [&](std::initializer_list<juce::MidiMessage> msgs) {
      juce::MidiBuffer m;
      for (const auto& msg : msgs) m.addEvent(msg, 0);
      buf.clear();
      proc.processBlock(buf, m);
    };
    const int swell = proc.continuousControlValue(2), wind = proc.continuousControlValue(7);

    // What the reporter's file sends: breath controller, volume, RPN.
    for (int ch : {1, 3})
      send({juce::MidiMessage::controllerEvent(ch, 2, 127), juce::MidiMessage::controllerEvent(ch, 7, 0),
            juce::MidiMessage::controllerEvent(ch, 101, 0), juce::MidiMessage::controllerEvent(ch, 100, 0)});
    send({juce::MidiMessage::controllerEvent(1, 2, 3)});
    MP_CHECK(proc.continuousControlValue(2) == swell,
             "an unlearned CC2 leaves control 2 at " + std::to_string(swell) + ", now " +
                 std::to_string(proc.continuousControlValue(2)));
    MP_CHECK(proc.continuousControlValue(7) == wind,
             "an unlearned CC7 leaves control 7 at " + std::to_string(wind) + ", now " +
                 std::to_string(proc.continuousControlValue(7)));

    // A controller learned for the swell moves it.
    mp::MidiBinding b;
    b.source.kind = mp::MidiSourceKind::ControlChange;
    b.source.channel = 1;
    b.source.number = 11;
    b.targetKind = mp::MidiTargetKind::ContinuousControl;
    b.targetId = 2;
    proc.midiMap().bind(b);
    send({juce::MidiMessage::controllerEvent(1, 11, 100)});
    MP_CHECK(proc.continuousControlValue(2) == 100,
             "a learned CC11 sets the swell to 100, now " + std::to_string(proc.continuousControlValue(2)));

    proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static UnmappedControllersTest g_unmappedControllers;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
