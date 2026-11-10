// A key held when the program starts waiting for a MIDI message (a learn, the
// MIDI window's Listen, a manual learned from its keys) stops sounding when it
// is let go. The wait takes presses; the release of a note that sounds is the
// player's, and it reaches the organ. The release of a press the wait took
// stays the wait's.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

// One manual, one stop with one rank, and a second switch for a learn to wait on.
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
    meta.set("NumSampleLoops", "1");
    meta.set("Loop0Start", "1000");
    meta.set("Loop0End", "47000");
    std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
    juce::AudioBuffer<float> tone(1, 48000);
    for (int i = 0; i < 48000; ++i)
      tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 130.81 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 48000);
  }
  const auto odf = defs.getChildFile("capture.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Capture</Identification_Name>"
      "<Identification_UniqueOrganID>36</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "<Pipe_SoundEngine01><PipeID>302</PipeID><RankID>201</RankID><NormalMIDINoteNumber>37</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
      "<Pipe_SoundEngine01_Layer><LayerID>402</LayerID><PipeID>302</PipeID></Pipe_SoundEngine01_Layer>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>502</UniqueID><LayerID>402</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
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
      "<NumberOfMappedDivisionInputNodes>2</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1102</SwitchID><Name>Spare</Name><Latching>Y</Latching></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

// The global settings file as it was: the noise filter is a global setting.
struct KeepFile {
  juce::File file;
  bool existed = false;
  juce::String content;
  explicit KeepFile(juce::File f) : file(std::move(f)) {
    existed = file.existsAsFile();
    if (existed) content = file.loadFileAsString();
  }
  ~KeepFile() {
    if (existed) file.replaceWithText(content);
    else file.deleteFile();
  }
};

class CaptureHeldKeysTest final : public mp::test::Test {
public:
  CaptureHeldKeysTest() : Test("functional.midi.capture-lets-held-keys-go", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-capture-held-keys");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    KeepFile keepGlobal(proc.globalSettingsFile());
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
    proc.setLearnNoiseFilter(false);

    juce::AudioBuffer<float> buf(2, 256);
    const auto block = [&](juce::MidiBuffer m = {}) {
      buf.clear();
      proc.processBlock(buf, m);
    };
    const auto voices = [&] { return proc.voiceStats().activeVoices; };
    const auto run = [&](int n) {
      for (int i = 0; i < n; ++i) block();
    };
    const auto note = [](bool down, int n = 36) {
      juce::MidiBuffer m;
      m.addEvent(down ? juce::MidiMessage::noteOn(1, n, 0.8f) : juce::MidiMessage::noteOff(1, n), 0);
      return m;
    };
    // Every case is checked, and each starts from silence, so one run names
    // every capture that keeps a key sounding.
    std::vector<std::string> failed;
    const auto check = [&](bool ok, const std::string& what) {
      if (!ok) failed.push_back(what);
    };
    const auto silence = [&] {
      proc.releaseAllKeys();
      run(400);
    };
    proc.setStopEngaged(901, true);

    // A stop waits for its button.
    block(note(true));
    run(4);
    check(voices() == 1, "the held key sounds");
    proc.midiMap().beginLearn(mp::MidiTargetKind::Switch, 1102);
    block(note(false));
    run(400);
    check(voices() == 0, "let go while a stop waits for its button, the key stops sounding");
    check(proc.midiMap().learning(), "and the stop still waits");
    proc.midiMap().cancelLearn();
    silence();

    // A level waits for a pedal: a key pressed meanwhile plays, and stops.
    proc.midiMap().beginLearn(mp::MidiTargetKind::ReverbWet, 0, false);
    block(note(true));
    run(4);
    check(voices() == 1, "a key pressed while a level waits for its pedal plays");
    block(note(false));
    run(400);
    check(voices() == 0, "and let go, it stops sounding");
    proc.midiMap().cancelLearn();
    silence();

    // The MIDI window listens.
    block(note(true));
    run(4);
    check(voices() == 1, "the held key sounds");
    proc.beginMidiListen();
    block(note(false));
    run(400);
    check(voices() == 0, "let go while the MIDI window listens, the key stops sounding");
    check(proc.midiListening(), "and the window still listens");
    proc.cancelMidiListen();
    silence();

    // The MIDI window listens for an on and an off: a key held from before is
    // neither.
    block(note(true));
    run(4);
    proc.beginMidiListenPair();
    block(note(false));
    run(400);
    check(voices() == 0, "let go while the window waits for an on and an off, the key stops sounding");
    check(proc.midiListening() && proc.midiListenPairStep() == 0, "and the window still waits for the on");
    proc.cancelMidiListen();
    silence();

    // A manual waits for its lowest key.
    block(note(true));
    run(4);
    check(voices() == 1, "the held key sounds");
    proc.beginKeyboardLearn(701);
    block(note(false));
    run(400);
    check(voices() == 0, "let go while a manual waits for its keys, the key stops sounding");
    check(proc.keyboardLearning() == 701 && !proc.keyboardLearnHasLowKey(), "and the manual still waits");
    // The lowest key, pressed now, is the learn's: it does not sound, and its
    // release leaves the learn waiting for the highest.
    const int sounding = voices();
    block(note(true, 37));
    run(4);
    check(voices() == sounding, "the key the manual's learn takes does not sound");
    block(note(false, 37));
    run(4);
    check(proc.keyboardLearning() == 701 && proc.keyboardLearnHasLowKey(),
          "its release is the learn's, and the learn waits for the highest key");
    proc.cancelKeyboardLearn();
    silence();

    // With the noise filter on, a learn holds keys back a quarter second; a
    // key that sounded before the learn is let go all the same.
    proc.setLearnNoiseFilter(true);
    block(note(true));
    run(4);
    check(voices() == 1, "the held key sounds");
    proc.midiMap().beginLearn(mp::MidiTargetKind::Switch, 1102);
    block(note(false));
    run(400);
    check(voices() == 0, "noise filter on: let go while a stop waits, the key stops sounding");
    check(proc.midiMap().learning(), "and the stop still waits");
    proc.midiMap().cancelLearn();
    proc.setLearnNoiseFilter(false);
    silence();

    std::string all;
    for (const auto& f : failed) all += "\n        " + f;
    MP_CHECK(failed.empty(), std::to_string(failed.size()) + " failed:" + all);

    proc.settingsFileFor(odf).deleteFile();
    proc.midiMapFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static CaptureHeldKeysTest g_captureHeldKeys;

}  // namespace

#endif
