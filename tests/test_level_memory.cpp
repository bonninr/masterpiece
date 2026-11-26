// The mixer's faders kept (#1197): a division's, a rank's and an output's
// come back when the organ loads again, the output's in the machine's file
// and the others in the organ's; a fader at full writes nothing. And the
// desk's fields (#1188): a pipe's brightness and bass set through them.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/VoicingSelection.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

// Two ranks on two stops in two divisions, one pipe each, sounding a tone.
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
    meta.set("MidiUnityNote", "36");
    std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
    juce::AudioBuffer<float> tone(1, 48000);
    for (int i = 0; i < 48000; ++i)
      tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 65.406 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 48000);
  }
  const auto odf = defs.getChildFile("levels.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Levels</Identification_Name>"
      "<Identification_UniqueOrganID>11970</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank><Rank><RankID>202</RankID><Name>B</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "<Pipe_SoundEngine01><PipeID>302</PipeID><RankID>202</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
      "<Pipe_SoundEngine01_Layer><LayerID>402</LayerID><PipeID>302</PipeID></Pipe_SoundEngine01_Layer>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>502</UniqueID><LayerID>402</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "<Keyboard><KeyboardID>702</KeyboardID><Name>II</Name><DefaultInputOutputKeyboardAsgnCode>2</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division>"
      "<Division><DivisionID>802</DivisionID><Name>S</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "<KeyAction><SourceKeyboardID>702</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>802</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "<Stop><StopID>902</StopID><Name>B</Name><DivisionID>802</DivisionID><ControllingSwitchID>1102</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "<StopRank><StopID>902</StopID><RankID>202</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1102</SwitchID><Name>B</Name><Latching>Y</Latching></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

// A file put back as it was when the test is over.
struct Keep {
  juce::File file;
  bool existed = false;
  juce::String content;
  explicit Keep(juce::File f) : file(std::move(f)) {
    existed = file.existsAsFile();
    if (existed) content = file.loadFileAsString();
  }
  ~Keep() {
    if (existed)
      file.replaceWithText(content);
    else
      file.deleteFile();
  }
};

class LevelMemoryTest final : public mp::test::Test {
public:
  LevelMemoryTest() : Test("functional.levels.kept", Category::Functional) {}
  void run() override {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-level-memory");
    const auto odf = writeOrgan(root);
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organ(probe.settingsFileFor(odf));
    global.file.getParentDirectory().createDirectory();
    organ.file.deleteFile();
    {
      mp::MasterpieceProcessor p;
      p.prepareToPlay(48000.0, 256);
      MP_CHECK(p.loadOrgan(odf, 0, false).ok, "the organ loads");
      p.setPairCount(2);
      p.setLiveDivisionLevel(p.liveDivisionIndexOf(802), 90);
      p.setLiveRankLevel(p.liveRankIndexOf(201), 60);
      p.setLivePairLevel(1, 150);
      p.saveSettingsIfDirty();
      const auto organText = organ.file.loadFileAsString();
      MP_CHECK(organText.contains("livelevel division 802 90") && organText.contains("livelevel rank 201 60"),
               "the organ's file keeps the division's and the rank's faders: " + organText.toStdString());
      MP_CHECK(!organText.contains("livelevel division 801") && !organText.contains("livelevel rank 202"),
               "a fader at full writes nothing");
      MP_CHECK(global.file.loadFileAsString().contains("livepair 2 150"),
               "the machine's file keeps output 2's fader: " + global.file.loadFileAsString().toStdString());
      p.releaseResources();
    }
    {
      mp::MasterpieceProcessor p;
      p.loadGlobalDefaults();
      p.prepareToPlay(48000.0, 256);
      MP_CHECK(p.loadOrgan(odf, 0, false).ok, "the organ loads again");
      MP_CHECK(p.liveDivisionLevel(p.liveDivisionIndexOf(802)) == 90, "the division's fader comes back");
      MP_CHECK(p.liveRankLevel(p.liveRankIndexOf(201)) == 60, "the rank's fader comes back");
      MP_CHECK(p.liveDivisionLevel(p.liveDivisionIndexOf(801)) == mp::LiveLevels::kFull, "the other stays at full");
      MP_CHECK(p.livePairLevel(1) == 150, "output 2's fader comes back: " + std::to_string(p.livePairLevel(1)));
      // A voicing change is saved with no button pressed (#1188).
      mp::PipeVoicing pv;
      pv.gainDb = -2.0f;
      p.voicing().live().setRank(202, pv);
      p.voicingChanged();
      p.saveSettingsIfDirty();
      MP_CHECK(organ.file.loadFileAsString().contains("voicingadj a rank 202 -2.000"),
               "a voicing change is saved on the next flush: " + organ.file.loadFileAsString().toStdString());
      p.releaseResources();
    }
    root.deleteRecursively();
  }
};
static LevelMemoryTest g_levelMemory;

// The desk's fields: each reads and writes its own value of a pipe, and says
// which voicing value it is.
class VoicingFieldsTest final : public mp::test::Test {
public:
  VoicingFieldsTest() : Test("functional.voicing.fields", Category::Functional) {}
  void run() override {
    using F = mp::VoicingField;
    mp::PipeVoicing v;
    mp::setVoicingValue(v, F::Level, -1.5f);
    mp::setVoicingValue(v, F::Tuning, 3.0f);
    mp::setVoicingValue(v, F::Brightness, 4.5f);
    mp::setVoicingValue(v, F::Bass, -6.0f);
    MP_CHECK(v.gainDb == -1.5f && v.tuningCents == 3.0f && v.brightnessDb == 4.5f && v.bassDb == -6.0f,
             "each field writes its own value");
    MP_CHECK(mp::voicingValue(v, F::Brightness) == 4.5f && mp::voicingValue(v, F::Bass) == -6.0f,
             "and reads it back");
    MP_CHECK(mp::voicingRange(F::Tuning) == 50.0f && mp::voicingRange(F::Bass) == 24.0f, "the ranges");
    MP_CHECK(mp::voicingValuesOf(F::Brightness) == mp::kVoicingBrightness &&
                 mp::voicingValuesOf(F::Bass) == mp::kVoicingBass && mp::voicingValuesOf(F::Level) == mp::kVoicingLevel,
             "each field names its voicing value");
  }
};
static VoicingFieldsTest g_voicingFields;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
