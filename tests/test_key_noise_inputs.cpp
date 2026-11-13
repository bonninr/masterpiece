// Key-action noises wired to the division's inputs (#967). Friesach plays its
// divisions straight from the keys, and gives each division a DivisionInput
// switch per note; its key-action noise pipes belong to no stop and open on
// that switch as their pallet. Nothing closed those switches, so the noises
// never sounded. The key closes its division's input now, and the pipe on it
// speaks.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

void writeWave(const juce::File& file, int frames, double level) {
  juce::WavAudioFormat fmt;
  std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
  std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, {}, 0));
  juce::AudioBuffer<float> tone(1, frames);
  for (int i = 0; i < frames; ++i)
    tone.setSample(0, i, static_cast<float>(level * std::sin(2.0 * 3.14159265 * 130.81 * i / 48000.0)));
  w->writeFromAudioSampleBuffer(tone, 0, frames);
}

std::string pipe(int id, int rank, int note, int pallet) {
  const std::string n = std::to_string(id);
  return "<Pipe_SoundEngine01><PipeID>" + n + "</PipeID><RankID>" + std::to_string(rank) +
         "</RankID><NormalMIDINoteNumber>" + std::to_string(note) +
         "</NormalMIDINoteNumber>" +
         (pallet != 0 ? "<ControllingPalletSwitchID>" + std::to_string(pallet) + "</ControllingPalletSwitchID>"
                      : std::string()) +
         "</Pipe_SoundEngine01>";
}

std::string layer(int id) {
  const std::string n = std::to_string(id);
  return "<Pipe_SoundEngine01_Layer><LayerID>" + n + "</LayerID><PipeID>" + n + "</PipeID></Pipe_SoundEngine01_Layer>";
}

std::string attack(int id) {
  const std::string n = std::to_string(id);
  return "<Pipe_SoundEngine01_AttackSample><UniqueID>" + n + "</UniqueID><LayerID>" + n +
         "</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>";
}

std::string input(int note, int sw) {
  return "<DivisionInput><DivisionID>801</DivisionID><SwitchID>" + std::to_string(sw) +
         "</SwitchID><NormalMIDINoteNumber>" + std::to_string(note) + "</NormalMIDINoteNumber></DivisionInput>";
}

std::string switchRow(int sw, const char* name) {
  return "<Switch><SwitchID>" + std::to_string(sw) + "</SwitchID><Name>" + name +
         "</Name><Latching>N</Latching></Switch>";
}

// One manual played straight into division 801. Rank 202, "Key Action": no
// stop, one noise pipe on C4 and one on D4, each on its note's input switch.
// C#4 has an input switch with no pipe on it.
juce::File writeOrgan(const juce::File& root) {
  root.deleteRecursively();
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();
  writeWave(pkg.getChildFile("click.wav"), 12000, 0.3);  // a quarter second
  const auto odf = defs.getChildFile("keynoise.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Key noise inputs</Identification_Name>"
      "<Identification_UniqueOrganID>967</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>click.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>202</RankID><Name>Key Action</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">" +
      pipe(302, 202, 60, 1260) + pipe(304, 202, 62, 1262) +
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">" + layer(302) + layer(304) +
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">" + attack(302) + attack(304) +
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"DivisionInput\">" +
      input(60, 1260) + input(61, 1261) + input(62, 1262) +
      "</ObjectList><ObjectList ObjectType=\"Switch\">" +
      switchRow(1260, "Keys Div:1 MIDI:60") + switchRow(1261, "Keys Div:1 MIDI:61") +
      switchRow(1262, "Keys Div:1 MIDI:62") + "</ObjectList></Hauptwerk>");
  return odf;
}

class KeyNoiseInputsTest final : public mp::test::Test {
public:
  KeyNoiseInputsTest() : Test("functional.playing.key-noise-division-inputs", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-key-noise-inputs");
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
      const auto msg = [](bool down, int note) {
        juce::MidiBuffer b;
        b.addEvent(down ? juce::MidiMessage::noteOn(1, note, 0.8f) : juce::MidiMessage::noteOff(1, note), 0);
        return b;
      };

      run(4, msg(true, 60));
      MP_CHECK(voices() == 1, "C4 going down sounds its key noise" + at());
      MP_CHECK(peak > 0.02f, "and it is heard" + at());
      run(4, msg(false, 60));
      run(190);
      MP_CHECK(voices() == 0, "the noise ends with its recording" + at());

      run(4, msg(true, 61));
      MP_CHECK(voices() == 0, "C#4's input carries no pipe, and the key is silent" + at());
      run(4, msg(false, 61));

      // Two keys at once: each opens its own input.
      juce::MidiBuffer both = msg(true, 60);
      both.addEvent(juce::MidiMessage::noteOn(1, 62, 0.8f), 0);
      run(4, both);
      MP_CHECK(voices() == 2, "C4 and D4 together sound both noises" + at());
      run(190);
      juce::MidiBuffer up = msg(false, 60);
      up.addEvent(juce::MidiMessage::noteOff(1, 62), 0);
      run(4, up);
      run(4, msg(true, 60));
      MP_CHECK(voices() == 1, "let go and pressed again, C4 sounds its noise again" + at());
      run(4, msg(false, 60));
      run(190);
      MP_CHECK(voices() == 0, "and it ends" + at());
      settings.deleteFile();
    }
    root.deleteRecursively();
  }
};
KeyNoiseInputsTest g_keyNoiseInputs;

}  // namespace

#endif
