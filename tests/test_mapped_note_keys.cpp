// Manuals I and II swapped on the console (#1124): the Positif on the channel
// the Great had and the Great on the Positif's. A note held on the Great went
// silent when the same note was let go on the Positif.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <functional>
#include <string>

namespace {

using mp::test::Category;

// Two seconds of a looped 100 Hz tone, long enough to hold.
void writeTone(const juce::File& file) {
  juce::StringPairArray meta;
  meta.set("MidiUnityNote", "60");
  meta.set("NumSampleLoops", "1");
  meta.set("Loop0Start", "4800");
  meta.set("Loop0End", "91199");
  juce::WavAudioFormat fmt;
  std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
  std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
  juce::AudioBuffer<float> tone(1, 96000);
  for (int i = 0; i < 96000; ++i)
    tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 100.0 * i / 48000.0)));
  w->writeFromAudioSampleBuffer(tone, 0, 96000);
}

// A pedal and two manuals, as a GrandOrgue set numbers them: the pedal is
// keyboard 1 on channel 1, the Great keyboard 2 on channel 2, the Positif
// keyboard 3 on channel 3. One stop on each manual, a pipe on middle C.
std::string organText() {
  std::string s =
      "[Organ]\r\nChurchName=Swapped manuals\r\nHasPedals=Y\r\nNumberOfManuals=2\r\n"
      "NumberOfWindchestGroups=1\r\n[WindchestGroup001]\r\nName=G\r\n"
      "[Manual000]\r\nName=Pedal\r\nNumberOfLogicalKeys=32\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
      "NumberOfAccessibleKeys=32\r\nNumberOfStops=0\r\n";
  const char* names[] = {"Great", "Positif"};
  for (int m = 1; m <= 2; ++m) {
    const std::string n = "00" + std::to_string(m);
    s += "[Manual" + n + "]\r\nName=" + names[m - 1] +
         "\r\nNumberOfLogicalKeys=61\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
         "NumberOfAccessibleKeys=61\r\nNumberOfStops=1\r\nStop001=" + n + "\r\n";
  }
  for (int m = 1; m <= 2; ++m) {
    s += "[Stop00" + std::to_string(m) + "]\r\nName=Bourdon " + std::to_string(m) +
         "\r\nNumberOfLogicalPipes=61\r\nNumberOfAccessiblePipes=61\r\n"
         "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nAcceptsRetuning=N\r\n";
    for (int p = 1; p <= 61; ++p) {
      char key[16];
      std::snprintf(key, sizeof key, "Pipe%03d", p);
      s += std::string(key) + (p == 25 ? "=c.wav\r\n" : "=DUMMY\r\n");
    }
  }
  return s;
}

class MappedNoteKeysTest final : public mp::test::Test {
public:
  MappedNoteKeysTest() : Test("functional.midi.swapped-manuals-hold-apart", Category::Functional) {}
  void run() override {
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-swapped-manuals");
    dir.deleteRecursively();
    dir.createDirectory();
    writeTone(dir.getChildFile("c.wav"));
    const juce::File file = dir.getChildFile("swapped.organ");
    file.replaceWithText(juce::String(organText()));

    constexpr mp::Id kGreat = 2, kPositif = 3;
    constexpr int kC = 60;
    constexpr int kConsole = 3;  // a MIDI device; 1 is the split keyboard's own

    // Each setup: the bindings it makes, the channels the Great and the
    // Positif are played on and the devices they come from (0: the host).
    struct Setup {
      const char* name;
      std::function<void(mp::MasterpieceProcessor&)> bind;
      int greatChannel, positifChannel, greatDevice, positifDevice;
    };
    const Setup setups[] = {
        {"both manuals set for the console, swapped",
         [](mp::MasterpieceProcessor& p) {
           p.setKeyboardForChannel(2, kPositif, kConsole);
           p.setKeyboardForChannel(3, kGreat, kConsole);
         },
         3, 2, kConsole, kConsole},
        {"both manuals set for any device, swapped",
         [](mp::MasterpieceProcessor& p) {
           p.setKeyboardForChannel(2, kPositif);
           p.setKeyboardForChannel(3, kGreat);
         },
         3, 2, kConsole, kConsole},
        {"the Great set to channel 5, the Positif on its own channel 3, from the host",
         [](mp::MasterpieceProcessor& p) { p.setKeyboardForChannel(5, kGreat); }, 5, 3, 0, 0},
        {"the Positif set to channel 2 from the host, the Great on channel 3",
         [](mp::MasterpieceProcessor& p) {
           p.setKeyboardForChannel(2, kPositif);
           p.setKeyboardForChannel(3, kGreat);
         },
         3, 2, 0, 0},
        {"the Great set for the console on channel 3, the Positif on channel 2 by the host",
         [](mp::MasterpieceProcessor& p) {
           p.setKeyboardForChannel(3, kGreat, kConsole);
           p.setKeyboardForChannel(2, kPositif);
         },
         3, 2, kConsole, 0},
        // The Great still bound to the channel the Positif took: that channel
        // plays both manuals, and the Great's C, already speaking, a second
        // time. Its release lets go of what that channel pressed, and the
        // Great's own channel holds on.
        {"the Great left on the Positif's channel 2 as well as its own 3",
         [](mp::MasterpieceProcessor& p) {
           p.setKeyboardForChannel(2, kPositif, kConsole);
           p.setKeyboardForChannel(3, kGreat, kConsole);
           mp::MidiMap::KeyboardBinding b;
           b.keyboardId = kGreat;
           b.deviceId = kConsole;
           b.channel = 2;
           p.editMidiMap([&](mp::MidiMap& map) { map.addKeyboardBinding(b); });
         },
         3, 2, kConsole, kConsole},
    };

    for (const Setup& setup : setups) {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(file);
      const juce::File map = proc.midiMapFileFor(file);
      settings.deleteFile();
      map.deleteFile();
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(file, 0, false).ok, "the organ loads");
      proc.clearChannelAssignments();
      setup.bind(proc);
      proc.setStopEngaged(2001, true);
      proc.setStopEngaged(2002, true);

      juce::AudioBuffer<float> buf(2, 256);
      const auto run = [&](int blocks, const juce::MidiMessage* msg, int device) {
        for (int b = 0; b < blocks; ++b) {
          buf.clear();
          juce::MidiBuffer m;
          if (b == 0 && msg != nullptr) {
            if (device > 0) proc.pushMidi(device, *msg);
            else m.addEvent(*msg, 0);
          }
          proc.processBlock(buf, m);
        }
      };
      const auto send = [&](const juce::MidiMessage& msg, int device) { run(8, &msg, device); };
      const auto voices = [&] { return proc.voiceStats().activeVoices; };
      const std::string at = std::string(" -- ") + setup.name;

      send(juce::MidiMessage::noteOn(setup.greatChannel, kC, 0.8f), setup.greatDevice);
      MP_CHECK(voices() == 1 && proc.keyDownOnKeyboard(kGreat, kC),
               "the Great's C sounds (" + std::to_string(voices()) + " voice(s))" + at);
      send(juce::MidiMessage::noteOn(setup.positifChannel, kC, 0.8f), setup.positifDevice);
      MP_CHECK(voices() == 2 && proc.keyDownOnKeyboard(kPositif, kC),
               "the Positif's C sounds with it (" + std::to_string(voices()) + " voice(s))" + at);
      send(juce::MidiMessage::noteOff(setup.positifChannel, kC), setup.positifDevice);
      run(100, nullptr, 0);
      MP_CHECK(voices() == 1 && proc.keyDownOnKeyboard(kGreat, kC),
               "letting go on the Positif leaves the Great's C sounding (" + std::to_string(voices()) +
                   " voice(s))" + at);
      send(juce::MidiMessage::noteOff(setup.greatChannel, kC), setup.greatDevice);
      run(100, nullptr, 0);
      MP_CHECK(voices() == 0, "and the Great's C lets go on its own note-off" + at);
      settings.deleteFile();
      map.deleteFile();
    }
    dir.deleteRecursively();
  }
};
MappedNoteKeysTest g_mappedNoteKeys;

// The same through a key switch, as a definition with key action plays: key
// C2's switch 5036 closes the division's input 33036. One manual on channels
// 1 and 2; the note held on channel 1 sounds on when channel 2 lets go of it.
juce::File writeKeySwitchOrgan(const juce::File& root) {
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
  const auto odf = defs.getChildFile("keyswitch.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Key switch</Identification_Name>"
      "<Identification_UniqueOrganID>36</Identification_UniqueOrganID></_General></ObjectList>"
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
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey>"
      "<Hint_PrimaryAssociatedDivisionID>801</Hint_PrimaryAssociatedDivisionID></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyboardKey\"><KeyboardKey><KeyboardID>701</KeyboardID><SwitchID>5036</SwitchID>"
      "<NormalMIDINoteNumber>36</NormalMIDINoteNumber></KeyboardKey></ObjectList>"
      "<ObjectList ObjectType=\"DivisionInput\"><DivisionInput><DivisionID>801</DivisionID><SwitchID>33036</SwitchID>"
      "<NormalMIDINoteNumber>36</NormalMIDINoteNumber></DivisionInput></ObjectList>"
      "<ObjectList ObjectType=\"SwitchLinkage\"><SwitchLinkage><SourceSwitchID>5036</SourceSwitchID>"
      "<DestSwitchID>33036</DestSwitchID><SourceSwitchLinkIfEngaged>Y</SourceSwitchLinkIfEngaged>"
      "<EngageLinkActionCode>1</EngageLinkActionCode><DisengageLinkActionCode>2</DisengageLinkActionCode>"
      "</SwitchLinkage></ObjectList>"
      "<ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>5036</SwitchID><Name>Key 36</Name><Latching>N</Latching></Switch>"
      "<Switch><SwitchID>33036</SwitchID><Name>Input 36</Name><Latching>N</Latching></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

class KeySwitchTwoChannelsTest final : public mp::test::Test {
public:
  KeySwitchTwoChannelsTest() : Test("functional.midi.key-switch-two-channels", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-key-switch-two-channels");
    const juce::File odf = writeKeySwitchOrgan(root);
    mp::MasterpieceProcessor proc;
    const juce::File settings = proc.settingsFileFor(odf);
    const juce::File map = proc.midiMapFileFor(odf);
    settings.deleteFile();
    map.deleteFile();
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
    proc.clearChannelAssignments();
    proc.editMidiMap([](mp::MidiMap& m) {
      for (int channel : {1, 2}) {
        mp::MidiMap::KeyboardBinding b;
        b.keyboardId = 701;
        b.channel = channel;
        m.addKeyboardBinding(b);
      }
    });
    proc.setStopEngaged(901, true);

    juce::AudioBuffer<float> buf(2, 256);
    const auto send = [&](const juce::MidiMessage* msg, int blocks) {
      for (int b = 0; b < blocks; ++b) {
        buf.clear();
        juce::MidiBuffer m;
        if (b == 0 && msg != nullptr) m.addEvent(*msg, 0);
        proc.processBlock(buf, m);
      }
    };
    const auto voices = [&] { return proc.voiceStats().activeVoices; };
    const auto on1 = juce::MidiMessage::noteOn(1, 36, 0.8f), on2 = juce::MidiMessage::noteOn(2, 36, 0.8f);
    const auto off1 = juce::MidiMessage::noteOff(1, 36), off2 = juce::MidiMessage::noteOff(2, 36);

    send(&on1, 8);
    MP_CHECK(voices() == 1, "channel 1's C2 sounds through the key switch (" + std::to_string(voices()) + ")");
    send(&on2, 8);
    send(&off2, 8);
    send(nullptr, 100);
    MP_CHECK(voices() == 1 && proc.keyDownOnKeyboard(701, 36),
             "channel 2 letting go of C2 leaves channel 1's sounding (" + std::to_string(voices()) + ")");
    send(&off1, 8);
    send(nullptr, 100);
    MP_CHECK(voices() == 0, "and channel 1's note-off lets it go");
    settings.deleteFile();
    map.deleteFile();
    root.deleteRecursively();
  }
};
KeySwitchTwoChannelsTest g_keySwitchTwoChannels;

}  // namespace

#endif
