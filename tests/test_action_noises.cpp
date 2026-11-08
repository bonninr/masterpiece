// Key and stop action noises written as StopRank rows (#998). ActionTypeCode 1
// is the division's keys, 21 the stop's own switch; ActionEffectCode 2 sounds
// once as it engages, 3 once as it disengages, 1 while it is engaged. Hebron
// writes its key "on" and "off" noises and its drawstop clicks this way, and
// Masterpiece played both key noises as the key went down and no stop click
// at all.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <array>
#include <cmath>
#include <initializer_list>
#include <map>
#include <string>

namespace {

using mp::test::Category;

void writeWave(const juce::File& file, int frames, double level, bool looped) {
  juce::WavAudioFormat fmt;
  std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
  juce::StringPairArray meta;
  if (looped) {
    meta.set("NumSampleLoops", "1");
    meta.set("Loop0Start", "1000");
    meta.set("Loop0End", juce::String(frames - 1000));
  }
  std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
  juce::AudioBuffer<float> tone(1, frames);
  for (int i = 0; i < frames; ++i)
    tone.setSample(0, i, static_cast<float>(level * std::sin(2.0 * 3.14159265 * 130.81 * i / 48000.0)));
  w->writeFromAudioSampleBuffer(tone, 0, frames);
}

std::string sample(int id, const char* file) {
  return "<Sample><SampleID>" + std::to_string(id) + "</SampleID><InstallationPackageID>1</InstallationPackageID>"
         "<SampleFilename>" + file + "</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
         "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample>";
}

// Pipes with one layer and one attack each, all numbered from the pipe's id:
// {pipe, rank, note, sample}.
std::string pipes(std::initializer_list<std::array<int, 4>> list) {
  std::string p, l, a;
  for (const auto& [id, rank, note, sampleId] : list) {
    const std::string n = std::to_string(id);
    p += "<Pipe_SoundEngine01><PipeID>" + n + "</PipeID><RankID>" + std::to_string(rank) +
         "</RankID><NormalMIDINoteNumber>" + std::to_string(note) + "</NormalMIDINoteNumber></Pipe_SoundEngine01>";
    l += "<Pipe_SoundEngine01_Layer><LayerID>" + n + "</LayerID><PipeID>" + n + "</PipeID></Pipe_SoundEngine01_Layer>";
    a += "<Pipe_SoundEngine01_AttackSample><UniqueID>" + n + "</UniqueID><LayerID>" + n + "</LayerID><SampleID>" +
         std::to_string(sampleId) + "</SampleID></Pipe_SoundEngine01_AttackSample>";
  }
  return "<ObjectList ObjectType=\"Pipe_SoundEngine01\">" + p +
         "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">" + l +
         "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">" + a + "</ObjectList>";
}

std::string stopRank(int stop, int rank, int type, int effect, int first, int count, int increment) {
  return "<StopRank><StopID>" + std::to_string(stop) + "</StopID><RankID>" + std::to_string(rank) +
         "</RankID><ActionTypeCode>" + std::to_string(type) + "</ActionTypeCode><ActionEffectCode>" +
         std::to_string(effect) + "</ActionEffectCode><MIDINoteNumOfFirstMappedDivisionInputNode>" +
         std::to_string(first) + "</MIDINoteNumOfFirstMappedDivisionInputNode><NumberOfMappedDivisionInputNodes>" +
         std::to_string(count) + "</NumberOfMappedDivisionInputNodes><MIDINoteNumIncrementFromDivisionToRank>" +
         std::to_string(increment) + "</MIDINoteNumIncrementFromDivisionToRank></StopRank>";
}

// One manual. Stop 901 Principal: rank 201, and its drawstop's two clicks,
// pipes 36 and 37 of rank 203. Stop 902, drawn from the start and never
// shown: the key noises, pipe 36 of rank 202 going down and 37 coming up.
// Stop 903 Blower, drawn from the start: rank 204's hum while drawn, and a
// click from rank 203 (pipe 38) as it is drawn.
juce::File writeOrgan(const juce::File& root) {
  root.deleteRecursively();
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();
  writeWave(pkg.getChildFile("tone.wav"), 48000, 0.3, true);
  writeWave(pkg.getChildFile("click.wav"), 12000, 0.3, false);  // a quarter second
  writeWave(pkg.getChildFile("hum.wav"), 48000, 0.1, true);
  const auto odf = defs.getChildFile("noises.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Action noises</Identification_Name>"
      "<Identification_UniqueOrganID>998</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\">" +
      sample(101, "tone.wav") + sample(102, "click.wav") + sample(103, "hum.wav") +
      "</ObjectList><ObjectList ObjectType=\"Rank\">"
      "<Rank><RankID>201</RankID><Name>Principal</Name></Rank>"
      "<Rank><RankID>202</RankID><Name>Noises: Key Action</Name></Rank>"
      "<Rank><RankID>203</RankID><Name>Noises: Stop Action</Name></Rank>"
      "<Rank><RankID>204</RankID><Name>Noises: Blower</Name></Rank>"
      "</ObjectList>" +
      pipes({{301, 201, 60, 101}, {302, 202, 60, 102}, {303, 202, 61, 102}, {304, 203, 36, 102},
             {305, 203, 37, 102}, {306, 203, 38, 102}, {307, 204, 60, 103}}) +
      "<ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>Principal</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "<Stop><StopID>902</StopID><Name>DivisionKeyAction</Name><DivisionID>801</DivisionID><ControllingSwitchID>1102</ControllingSwitchID></Stop>"
      "<Stop><StopID>903</StopID><Name>Blower</Name><DivisionID>801</DivisionID><ControllingSwitchID>1103</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">" +
      stopRank(901, 201, 1, 1, 36, 61, 0) + stopRank(901, 203, 21, 2, 0, 0, 36) +
      stopRank(901, 203, 21, 3, 0, 0, 37) + stopRank(902, 202, 1, 2, 36, 61, 0) +
      stopRank(902, 202, 1, 3, 36, 61, 1) + stopRank(903, 204, 21, 1, 0, 0, 60) +
      stopRank(903, 203, 21, 2, 0, 0, 38) +
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>Principal</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1102</SwitchID><Name>Key action</Name><Latching>Y</Latching><DefaultToEngaged>Y</DefaultToEngaged></Switch>"
      "<Switch><SwitchID>1103</SwitchID><Name>Blower</Name><Latching>Y</Latching><DefaultToEngaged>Y</DefaultToEngaged></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

class ActionNoisesTest final : public mp::test::Test {
public:
  ActionNoisesTest() : Test("functional.playing.stoprank-action-noises", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-action-noises");
    const juce::File odf = writeOrgan(root);
    {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(odf);
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");

      juce::AudioBuffer<float> buf(2, 256);
      float peak = 0.0f;
      const auto run = [&](int n, juce::MidiBuffer first = {}) {
        peak = 0.0f;
        for (int b = 0; b < n; ++b) {
          buf.clear();
          juce::MidiBuffer m;
          if (b == 0) m = first;
          proc.processBlock(buf, m);
          for (int c = 0; c < buf.getNumChannels(); ++c)
            for (int i = 0; i < buf.getNumSamples(); ++i) peak = std::max(peak, std::fabs(buf.getSample(c, i)));
        }
      };
      const auto voices = [&] { return proc.voiceStats().activeVoices; };
      const auto at = [&] {
        return " (" + std::to_string(voices()) + " voice(s), peak " + std::to_string(peak) + ")";
      };
      juce::MidiBuffer on, off;
      on.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
      off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);

      std::map<mp::Id, bool> playable;
      for (const auto& e : proc.stopList()) playable[e.stopId] = e.playable;
      MP_CHECK(playable[901] && playable[903], "the Principal and the Blower can be drawn");

      // Loaded with the Blower drawn: its hum runs, and no click sounds for
      // a stop the organ starts with.
      run(4);
      MP_CHECK(voices() == 1, "the blower runs from the start, with no click" + at());
      proc.setStopEngaged(903, false);
      run(400);
      MP_CHECK(voices() == 0, "pushed in, the blower stops" + at());
      proc.setStopEngaged(903, true);
      run(4);
      MP_CHECK(voices() == 2, "drawn again, it runs and clicks" + at());
      run(190);
      MP_CHECK(voices() == 1, "the click ends with its recording; the hum runs on" + at());
      proc.setStopEngaged(903, false);
      run(400);
      MP_CHECK(voices() == 0, "and stops again" + at());

      // A key with no stop of pipework drawn: its noise going down, and the
      // other one coming up.
      run(4, on);
      MP_CHECK(voices() == 1, "the key going down sounds one noise" + at());
      run(4, off);
      MP_CHECK(voices() == 2, "coming up at once, the first rings on and the second sounds" + at());
      run(190);
      MP_CHECK(voices() == 0, "both end with their recordings" + at());
      run(4, on);
      run(190);
      MP_CHECK(voices() == 0, "held, the key's noise has ended" + at());
      run(4, off);
      MP_CHECK(voices() == 1, "letting go sounds the other noise" + at());
      MP_CHECK(peak > 0.02f, "and it is heard" + at());
      run(190);

      // The Principal's drawstop clicks both ways; held keys play its pipe
      // with the key noise beside it.
      proc.setStopEngaged(901, true);
      run(4);
      MP_CHECK(voices() == 1, "drawing the stop sounds its click" + at());
      MP_CHECK(peak > 0.02f, "and it is heard" + at());
      run(190);
      MP_CHECK(voices() == 0, "the click ends with its recording" + at());
      run(4, on);
      MP_CHECK(voices() == 2, "a key sounds the pipe and the key noise" + at());
      run(190);
      MP_CHECK(voices() == 1, "the pipe speaks on while the key is held" + at());
      run(4, off);
      run(400);
      MP_CHECK(voices() == 0, "let go, the pipe and the noise end" + at());
      proc.setStopEngaged(901, false);
      run(4);
      MP_CHECK(voices() == 1, "pushing the stop in sounds its other click" + at());
      MP_CHECK(peak > 0.02f, "and it is heard" + at());
      settings.deleteFile();
    }
    root.deleteRecursively();
  }
};
ActionNoisesTest g_actionNoises;

}  // namespace

#endif
