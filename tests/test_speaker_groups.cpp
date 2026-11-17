// Speaker groups, mono pairs and copies of a pair on further channels.
//
// A rank sent to a speaker group plays each pipe through one of the group's
// pairs, chosen at note-on by the key and the rank: a note sounds from the
// same output every time it is played (#1023). A mono pair plays left
// and right at half level each on its one channel; a pair's further channels
// carry exactly what its own carry.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

using mp::test::Category;

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr mp::Id kRank = 201;

// `ranks` ranks of twelve pipes, keys 36 to 47, each a looped tone, each on a
// stop of its own: rank 201 on stop 901, 202 on 902, and so on. With
// `oneStop`, every rank on stop 901, as a stop plays its perspectives.
struct Organ {
  juce::File odf;
  juce::File root;

  explicit Organ(const juce::String& name, int ranks = 1, bool oneStop = false) {
    root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
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
      std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), kRate, 1, 16, meta, 0));
      juce::AudioBuffer<float> tone(1, 48000);
      for (int i = 0; i < 48000; ++i)
        tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * kPi * 130.81 * i / kRate)));
      w->writeFromAudioSampleBuffer(tone, 0, 48000);
    }
    juce::String pipes, layers, attacks, rankRows, stops, stopRanks, switches;
    for (int r = 0; r < ranks; ++r) {
      const juce::String rank(201 + r), stop(oneStop ? 901 : 901 + r), sw(oneStop ? 1101 : 1101 + r);
      rankRows << "<Rank><RankID>" << rank << "</RankID><Name>R" << rank << "</Name></Rank>";
      if (!oneStop || r == 0) {
        stops << "<Stop><StopID>" << stop << "</StopID><Name>S" << stop << "</Name><DivisionID>801</DivisionID>"
              << "<ControllingSwitchID>" << sw << "</ControllingSwitchID></Stop>";
        switches << "<Switch><SwitchID>" << sw << "</SwitchID><Name>W" << sw << "</Name><Latching>Y</Latching></Switch>";
      }
      stopRanks << "<StopRank><StopID>" << stop << "</StopID><RankID>" << rank
                << "</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
                << "<NumberOfMappedDivisionInputNodes>12</NumberOfMappedDivisionInputNodes></StopRank>";
      for (int k = 0; k < 12; ++k) {
        const juce::String key(36 + k), pipe(3000 + 100 * r + k), layer(4000 + 100 * r + k),
            attack(5000 + 100 * r + k);
        pipes << "<Pipe_SoundEngine01><PipeID>" << pipe << "</PipeID><RankID>" << rank
              << "</RankID><NormalMIDINoteNumber>" << key << "</NormalMIDINoteNumber></Pipe_SoundEngine01>";
        layers << "<Pipe_SoundEngine01_Layer><LayerID>" << layer << "</LayerID><PipeID>" << pipe
               << "</PipeID></Pipe_SoundEngine01_Layer>";
        attacks << "<Pipe_SoundEngine01_AttackSample><UniqueID>" << attack << "</UniqueID><LayerID>" << layer
                << "</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>";
      }
    }
    odf = defs.getChildFile("groups.Organ_Hauptwerk_xml");
    odf.replaceWithText(
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Groups</Identification_Name>"
        "<Identification_UniqueOrganID>4402</Identification_UniqueOrganID></_General></ObjectList>"
        "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
        "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
        "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
        "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
        "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
        "<ObjectList ObjectType=\"Rank\">" +
        rankRows + "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01\">" + pipes +
        "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">" + layers +
        "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">" + attacks +
        "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
        "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
        "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
        "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
        "<ObjectList ObjectType=\"KeyAction\">"
        "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
        "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
        "</ObjectList><ObjectList ObjectType=\"Stop\">" +
        stops + "</ObjectList><ObjectList ObjectType=\"StopRank\">" + stopRanks +
        "</ObjectList><ObjectList ObjectType=\"Switch\">" + switches + "</ObjectList></Hauptwerk>");
  }
};

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

using Channels = std::vector<std::vector<float>>;

// The organ through `outputs` channels with the mixer `setUp` makes, the
// stops drawn and the keys pressed in the first block: every channel, twenty
// blocks of it.
Channels render(const juce::File& odf, int outputs, const std::vector<int>& keys,
                const std::function<void(mp::MasterpieceProcessor&)>& setUp,
                const std::vector<mp::Id>& stops = {901}, int blocks = 20) {
  mp::MasterpieceProcessor proc;
  juce::AudioProcessor::BusesLayout layout;
  layout.outputBuses.add(outputs == 2 ? juce::AudioChannelSet::stereo()
                                      : juce::AudioChannelSet::discreteChannels(outputs));
  MP_CHECK(proc.setBusesLayout(layout), "the processor takes " + std::to_string(outputs) + " outputs");
  proc.prepareToPlay(kRate, kBlock);
  MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
  setUp(proc);
  for (mp::Id stop : stops) proc.setStopEngaged(stop, true);

  juce::AudioBuffer<float> buffer(outputs, kBlock);
  juce::MidiBuffer on;
  for (int k : keys) on.addEvent(juce::MidiMessage::noteOn(1, k, 0.8f), 0);
  juce::MidiBuffer none;
  Channels out(static_cast<size_t>(outputs));
  for (int block = 0; block < blocks; ++block) {
    buffer.clear();
    proc.processBlock(buffer, block == 0 ? on : none);
    for (int ch = 0; ch < outputs; ++ch)
      out[static_cast<size_t>(ch)].insert(out[static_cast<size_t>(ch)].end(), buffer.getReadPointer(ch),
                                          buffer.getReadPointer(ch) + kBlock);
  }
  proc.releaseResources();
  return out;
}

float peakOf(const std::vector<float>& c) {
  float peak = 0.0f;
  for (float v : c) peak = std::max(peak, std::fabs(v));
  return peak;
}

// `pairs` stereo pairs on consecutive channels, and the rank sent to `dest`.
void pairsTo(mp::MasterpieceProcessor& p, int pairs, std::variant<mp::BusId, int> dest) {
  p.setPairCount(pairs);
  p.setRankRoutes({{kRank, dest}});
}

// The pairs of a stereo layout that carry sound.
std::vector<int> soundingPairs(const Channels& out) {
  std::vector<int> pairs;
  for (size_t ch = 0; ch + 1 < out.size(); ch += 2)
    if (peakOf(out[ch]) > 0.01f || peakOf(out[ch + 1]) > 0.01f) pairs.push_back(static_cast<int>(ch / 2) + 1);
  return pairs;
}

std::string list(const std::vector<int>& v) {
  std::string s;
  for (int x : v) s += (s.empty() ? "" : ",") + std::to_string(x);
  return s.empty() ? "none" : s;
}

// Across the octave and Chromatic: each key on the pair the method names,
// and only there, with the signal a pair of its own carries.
class SpreadTest final : public mp::test::Test {
public:
  SpreadTest() : Test("functional.output.speaker-groups.spread", Category::Functional) {}
  void run() override {
    const Organ organ("mp-speaker-groups-spread");
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organSettings(probe.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();

    for (const auto algo : {mp::AllocationAlgorithm::StaticOctaveCycled, mp::AllocationAlgorithm::StaticChromatic}) {
      mp::BusGroup group;
      group.groupId = 1;
      group.members = {mp::BusId{1}, mp::BusId{2}, mp::BusId{3}, mp::BusId{4}};
      group.algorithm = algo;
      std::vector<int> used;
      for (int key = 36; key < 48; key += 3) {
        const auto out = render(organ.odf, 8, {key}, [&](mp::MasterpieceProcessor& p) {
          pairsTo(p, 4, 1);
          p.setSpeakerGroups({group});
        });
        // The salt is the rank's stop family: 0, the organ's only one.
        const int want = mp::allocateBus(group, key, 0, algo, 0).value;
        const auto got = soundingPairs(out);
        MP_CHECK(got == std::vector<int>{want}, std::string(mp::allocationToken(algo)) + ": key " +
                                                    std::to_string(key) + " sounds on pair " + std::to_string(want) +
                                                    ", it sounds on " + list(got));
        used.push_back(want);
        // The same signal a rank sent straight to that pair gives.
        const auto direct = render(organ.odf, 8, {key}, [&](mp::MasterpieceProcessor& p) {
          pairsTo(p, 4, mp::BusId{want});
        });
        const auto at = static_cast<size_t>(2 * (want - 1));
        MP_CHECK(out[at] == direct[at] && out[at + 1] == direct[at + 1],
                 "and it is bit for bit the rank sent to that pair");
      }
      std::sort(used.begin(), used.end());
      used.erase(std::unique(used.begin(), used.end()), used.end());
      MP_CHECK(used.size() > 1, std::string(mp::allocationToken(algo)) + ": the keys use more than one pair: " +
                                    list(used));
    }
    organ.root.deleteRecursively();
  }
};
static SpreadTest g_spread;

// A note sounds from the same output every time it is played, whatever else
// is held: alone, or with three other keys down, key 38 is on one pair, for
// each method (#1023). The first previews' "busy" method reads as Across the
// octave.
class SameSpeakerTest final : public mp::test::Test {
public:
  SameSpeakerTest() : Test("functional.output.speaker-groups.same-speaker", Category::Functional) {}
  void run() override {
    MP_CHECK(mp::allocationFromToken("busy") == mp::AllocationAlgorithm::StaticOctaveCycled,
             "the old Least busy reads as Across the octave");
    const Organ organ("mp-speaker-groups-same");
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organSettings(probe.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();

    for (auto method : {mp::AllocationAlgorithm::StaticOctaveCycled, mp::AllocationAlgorithm::StaticChromatic,
                        mp::AllocationAlgorithm::StaticByRank}) {
      mp::BusGroup group;
      group.groupId = 7;
      group.members = {mp::BusId{1}, mp::BusId{2}, mp::BusId{3}, mp::BusId{4}};
      group.algorithm = method;
      const auto setUp = [&](mp::MasterpieceProcessor& p) {
        pairsTo(p, 4, 7);
        p.setSpeakerGroups({group});
      };
      const auto alone = soundingPairs(render(organ.odf, 8, {38}, setUp));
      MP_CHECK(alone.size() == 1, std::string("key 38 alone sounds on one pair (") + mp::allocationToken(method) +
                                      "): " + list(alone));
      // Played with three keys already down, the first of them in the same
      // block: the pair is the same.
      const auto withOthers = soundingPairs(render(organ.odf, 8, {36, 37, 40, 38}, setUp));
      MP_CHECK(!alone.empty() && std::find(withOthers.begin(), withOthers.end(), alone.front()) != withOthers.end(),
               std::string("with others held, key 38 still has pair ") + list(alone) + " (" +
                   mp::allocationToken(method) + "): " + list(withOthers));
      const auto again = soundingPairs(render(organ.odf, 8, {38}, setUp));
      MP_CHECK(again == alone, std::string("played again, the same pair (") + mp::allocationToken(method) + ")");
    }
    organ.root.deleteRecursively();
  }
};
static SameSpeakerTest g_sameSpeaker;

// A mono pair: left and right at half level each on its one channel. A
// pair's copies carry exactly what its own channels carry.
class MonoAndCopiesTest final : public mp::test::Test {
public:
  MonoAndCopiesTest() : Test("functional.output.mono-and-copies", Category::Functional) {}
  void run() override {
    const Organ organ("mp-speaker-groups-mono");
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organSettings(probe.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();

    const auto stereo = render(organ.odf, 2, {36}, [](mp::MasterpieceProcessor& p) { pairsTo(p, 1, mp::BusId{1}); });
    MP_CHECK(peakOf(stereo[0]) > 0.01f, "the key sounds in stereo");

    // Pair 1 in mono on channel 3 of four.
    const auto mono = render(organ.odf, 4, {36}, [](mp::MasterpieceProcessor& p) {
      pairsTo(p, 1, mp::BusId{1});
      p.setPairLayout(1, true, {2});
    });
    std::vector<float> half(stereo[0].size());
    for (size_t i = 0; i < half.size(); ++i) half[i] = 0.0f + stereo[0][i] * 0.5f + stereo[1][i] * 0.5f;
    float worst = 0.0f;
    for (size_t i = 0; i < half.size(); ++i) worst = std::max(worst, std::fabs(half[i] - mono[2][i]));
    MP_CHECK(worst < 1.0e-6f, "channel 3 carries half of left plus half of right, off by " + std::to_string(worst));
    for (int ch : {0, 1, 3})
      MP_CHECK(peakOf(mono[static_cast<size_t>(ch)]) == 0.0f, "and channel " + std::to_string(ch + 1) + " is silent");

    // Two mono pairs on a stereo device: a speaker on each channel.
    const auto two = render(organ.odf, 2, {36}, [](mp::MasterpieceProcessor& p) {
      pairsTo(p, 2, mp::BusId{2});
      p.setPairLayout(1, true, {0});
      p.setPairLayout(2, true, {1});
    });
    MP_CHECK(peakOf(two[0]) == 0.0f && peakOf(two[1]) > 0.01f,
             "pair 2 in mono on channel 2 of a stereo device sounds there alone");

    // Pair 1 on 1-2 with a copy on 5-6, of six channels.
    const auto copied = render(organ.odf, 6, {36}, [](mp::MasterpieceProcessor& p) {
      pairsTo(p, 1, mp::BusId{1});
      p.setPairLayout(1, false, {0, 1, 4, 5});
    });
    MP_CHECK(copied[0] == stereo[0] && copied[1] == stereo[1], "the pair's own channels carry the stereo signal");
    MP_CHECK(copied[4] == stereo[0] && copied[5] == stereo[1], "and the copy on 5-6 carries it bit for bit");
    MP_CHECK(peakOf(copied[2]) == 0.0f && peakOf(copied[3]) == 0.0f, "3 and 4 are silent");

    // A mono pair with a copy: the same half sum on both of its channels.
    const auto monoCopy = render(organ.odf, 4, {36}, [](mp::MasterpieceProcessor& p) {
      pairsTo(p, 1, mp::BusId{1});
      p.setPairLayout(1, true, {0, 3});
    });
    MP_CHECK(monoCopy[0] == monoCopy[3] && peakOf(monoCopy[0]) > 0.01f, "a mono pair's copy carries what it carries");

    // What the device is asked for counts the copies.
    mp::MasterpieceProcessor p;
    p.setPairLayout(1, false, {0, 1, 10, 11});
    MP_CHECK(p.outputChannelsWanted() == 12, "a copy on 11-12 wants twelve channels");
    organ.root.deleteRecursively();
  }
};
static MonoAndCopiesTest g_monoAndCopies;

// The pairs and the speaker groups are kept in the global file and come back
// at the next start; a file from before reads as it always did.
class SettingsTest final : public mp::test::Test {
public:
  SettingsTest() : Test("functional.output.speaker-groups.settings", Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor proc;
    Keep global(proc.globalSettingsFile());
    global.file.getParentDirectory().createDirectory();

    proc.setPairCount(3);
    proc.setPairLayout(2, true, {4, 5});
    proc.setPairLayout(3, false, {6, 7, 10, 11});
    mp::BusGroup a, b;
    a.groupId = 1;
    a.members = {mp::BusId{1}, mp::BusId{2}};
    b.groupId = 2;
    b.members = {mp::BusId{2}, mp::BusId{3}};
    b.algorithm = mp::AllocationAlgorithm::StaticByRank;
    proc.setSpeakerGroups({a, b});
    MP_CHECK(proc.saveGlobalDefaults(), "the global file is written");
    const auto text = global.file.loadFileAsString();
    MP_CHECK(text.contains("buses 1:0,1 2m:4,5 3:6,7,10,11"), "the pairs line: " + text.toStdString());
    MP_CHECK(text.contains("groups 1:1,2 2:2,3/rank"), "the groups line: " + text.toStdString());

    mp::MasterpieceProcessor next;
    next.loadGlobalDefaults();
    const auto& m = next.mixer();
    MP_CHECK(m.buses.size() == 3, "three pairs come back");
    MP_CHECK(m.buses.size() == 3 && m.buses[1].mono && m.buses[1].deviceChannels == (std::vector<int>{4, 5}),
             "pair 2 in mono on 5 and 6");
    MP_CHECK(m.buses.size() == 3 && !m.buses[2].mono &&
                 m.buses[2].deviceChannels == (std::vector<int>{6, 7, 10, 11}),
             "pair 3 with its copy");
    MP_CHECK(m.groups.size() == 2 && m.groups[0].algorithm == mp::AllocationAlgorithm::StaticOctaveCycled &&
                 m.groups[1].algorithm == mp::AllocationAlgorithm::StaticByRank &&
                 m.groups[1].members == (std::vector<mp::BusId>{mp::BusId{2}, mp::BusId{3}}),
             "the two groups with their methods");

    // A file from before: no mono, no method.
    global.file.replaceWithText("buses 1:0,1 2:2,3\ngroups 5:1,2\n");
    mp::MasterpieceProcessor old;
    old.loadGlobalDefaults();
    MP_CHECK(old.mixer().buses.size() == 2 && !old.mixer().buses[1].mono, "an older file's pairs are stereo");
    MP_CHECK(old.mixer().groups.size() == 1 && old.mixer().groups[0].groupId == 5 &&
                 old.mixer().groups[0].algorithm == mp::AllocationAlgorithm::StaticOctaveCycled,
             "and its group spreads across the octave");

    // A pair taken away leaves the groups, and a rank sent to it goes to pair 1.
    old.setRankRoutes({{kRank, mp::BusId{2}}});
    old.setPairCount(1);
    MP_CHECK(old.mixer().groups[0].members == std::vector<mp::BusId>{mp::BusId{1}}, "the group keeps pair 1 only");
    // routingFor returns a copy: the destination is copied out of it.
    const auto dest = old.mixer().routingFor(kRank).perspectives[0].dest;
    MP_CHECK(std::holds_alternative<mp::BusId>(dest) && std::get<mp::BusId>(dest).value == 1,
             "the rank on pair 2 goes to pair 1");
    old.setRankRoutes({{kRank, 5}});
    old.setSpeakerGroups({});
    const auto gone = old.mixer().routingFor(kRank).perspectives[0].dest;
    MP_CHECK(std::holds_alternative<mp::BusId>(gone) && std::get<mp::BusId>(gone).value == 1,
             "a rank sent to a group that is gone goes to pair 1");

    // Only what the file can hold: no output 0, no group 0. Found by the
    // fuzz (seeds 2003, 2012, 2020): a member 0 was saved, then dropped on
    // the next start.
    mp::BusGroup odd, none;
    odd.groupId = 3;
    odd.members = {mp::BusId{1}, mp::BusId{0}, mp::BusId{1}};
    none.groupId = 0;
    none.members = {mp::BusId{1}};
    old.setSpeakerGroups({odd, none});
    MP_CHECK(old.mixer().groups.size() == 1 &&
                 old.mixer().groups[0].members == (std::vector<mp::BusId>{mp::BusId{1}, mp::BusId{1}}),
             "the group without output 0, and no group 0");
    MP_CHECK(old.saveGlobalDefaults(), "the global file is written");
    const auto saved = global.file.loadFileAsString();
    mp::MasterpieceProcessor reread;
    reread.loadGlobalDefaults();
    MP_CHECK(reread.saveGlobalDefaults() && global.file.loadFileAsString() == saved,
             "and the file reads back to itself");
  }
};
static SettingsTest g_settings;

// By rank: every note of a rank on one pair, the group's ranks taking its
// pairs in turn by id.
class ByRankTest final : public mp::test::Test {
public:
  ByRankTest() : Test("functional.output.speaker-groups.by-rank", Category::Functional) {}
  void run() override {
    const Organ organ("mp-speaker-groups-by-rank", 4);
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organSettings(probe.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();

    mp::BusGroup group;
    group.groupId = 1;
    group.members = {mp::BusId{1}, mp::BusId{2}, mp::BusId{3}, mp::BusId{4}};
    group.algorithm = mp::AllocationAlgorithm::StaticByRank;
    const auto setUp = [&](mp::MasterpieceProcessor& p) {
      p.setPairCount(4);
      p.setSpeakerGroups({group});
      p.setRankRoutes({{201, 1}, {202, 1}, {203, 1}, {204, 1}});
    };
    // Each rank alone: all its keys on one pair, rank 201 on the first.
    std::vector<int> pairs;
    for (int r = 0; r < 4; ++r) {
      const mp::Id stop = 901 + r;
      const auto out = render(organ.odf, 8, {36, 37, 40, 45}, setUp, {stop});
      const auto got = soundingPairs(out);
      MP_CHECK(got.size() == 1, "rank " + std::to_string(201 + r) + ": four keys sound on one pair, on " + list(got));
      pairs.push_back(got.empty() ? 0 : got[0]);
    }
    MP_CHECK(pairs == (std::vector<int>{1, 2, 3, 4}), "the four ranks take the pairs in turn: " + list(pairs));
    // Across the octave puts the same keys of one rank on several pairs.
    group.algorithm = mp::AllocationAlgorithm::StaticOctaveCycled;
    const auto octave = render(organ.odf, 8, {36, 37, 40, 45}, setUp, {901});
    MP_CHECK(soundingPairs(octave).size() > 1, "and across the octave the same keys spread: " +
                                                   list(soundingPairs(octave)));
    group.algorithm = mp::AllocationAlgorithm::StaticByRank;

    // A rank in the group's list and one sent straight to a pair: the place
    // counts the group's ranks only.
    const auto two = render(organ.odf, 8, {36}, [&](mp::MasterpieceProcessor& p) {
      p.setPairCount(4);
      p.setSpeakerGroups({group});
      p.setRankRoutes({{201, mp::BusId{4}}, {202, 1}, {203, 1}});
    }, {902, 903});
    MP_CHECK(soundingPairs(two) == (std::vector<int>{1, 2}),
             "ranks 202 and 203 alone in the group take pairs 1 and 2: " + list(soundingPairs(two)));
    MP_CHECK(std::string(mp::allocationToken(mp::AllocationAlgorithm::StaticByRank)) == "rank" &&
                 mp::allocationFromToken("rank") == mp::AllocationAlgorithm::StaticByRank,
             "its word in the settings file is \"rank\"");
    organ.root.deleteRecursively();
  }
};
static ByRankTest g_byRank;

// One key of one stop sounds from one member of a speaker group, however many
// ranks the stop plays (its perspectives); a second stop starts one member
// further on. The ranks go there by the default destination, with no route of
// their own (#1023).
class OneKeyOneOutputTest final : public mp::test::Test {
public:
  OneKeyOneOutputTest() : Test("functional.output.speaker-groups.one-key-one-output", Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    global.file.getParentDirectory().createDirectory();

    mp::BusGroup group;
    group.groupId = 1;
    group.members = {mp::BusId{1}, mp::BusId{2}, mp::BusId{3}, mp::BusId{4}};
    const auto setUp = [&](mp::MasterpieceProcessor& p) {
      p.setPairCount(4);
      p.setSpeakerGroups({group});
      p.setDefaultDestination(1);
    };

    // Three ranks on one stop.
    {
      const Organ organ("mp-speaker-groups-one-stop", 3, true);
      Keep organSettings(probe.settingsFileFor(organ.odf));
      std::vector<int> used;
      for (int key = 36; key < 48; ++key) {
        const auto got = soundingPairs(render(organ.odf, 8, {key}, setUp, {901}, 6));
        MP_CHECK(got.size() == 1, "key " + std::to_string(key) + " of a three-rank stop sounds on one pair, on " +
                                      list(got));
        if (!got.empty()) used.push_back(got[0]);
      }
      std::sort(used.begin(), used.end());
      used.erase(std::unique(used.begin(), used.end()), used.end());
      MP_CHECK(used.size() == 4, "and the twelve keys use all four pairs: " + list(used));
      organ.root.deleteRecursively();
    }
    // Two stops of one rank each: one key, two pairs.
    {
      const Organ organ("mp-speaker-groups-two-stops", 2);
      Keep organSettings(probe.settingsFileFor(organ.odf));
      const auto got = soundingPairs(render(organ.odf, 8, {36}, setUp, {901, 902}, 6));
      MP_CHECK(got.size() == 2, "one key on two stops sounds on two pairs, on " + list(got));
      organ.root.deleteRecursively();
    }
  }
};
static OneKeyOneOutputTest g_oneKeyOneOutput;

// The stop families: the ranks one stop plays are one family, a rank two
// stops share joins them, an alternate (tremmed) rank goes with its stop, and
// a rank no stop names is a family of its own.
class RankFamiliesTest final : public mp::test::Test {
public:
  RankFamiliesTest() : Test("functional.output.speaker-groups.families", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    for (mp::Id r : {11, 12, 13, 14, 15, 16, 17}) m.ranks[r].rankId = r;
    const auto stop = [&m](mp::Id id, std::vector<std::pair<mp::Id, mp::Id>> ranks) {
      mp::Stop s;
      s.stopId = id;
      for (const auto& [r, alt] : ranks) {
        mp::StopRankEntry e;
        e.rankId = r;
        e.alternateRankId = alt;
        s.ranks.push_back(e);
      }
      m.stops[id] = s;
    };
    stop(1, {{11, 0}, {12, 0}});  // a stop of two perspectives
    stop(2, {{13, 16}});          // a stop with its tremmed rank
    stop(3, {{12, 0}, {14, 0}});  // a stop sharing rank 12 with stop 1
    const auto f = mp::rankFamilies(m);
    MP_CHECK(f.at(11) == f.at(12) && f.at(12) == f.at(14), "stops 1 and 3 share rank 12: one family");
    MP_CHECK(f.at(13) == f.at(16), "a stop's tremmed rank is in its family");
    MP_CHECK(f.at(11) != f.at(13) && f.at(15) != f.at(17) && f.at(15) != f.at(11),
             "other stops and unplayed ranks are families of their own");
    MP_CHECK(f.at(11) == 0 && f.at(13) == 1 && f.at(15) == 2 && f.at(17) == 3,
             "numbered in the order of their lowest rank");
  }
};
static RankFamiliesTest g_rankFamilies;

// An output that also plays into another: four speaker outputs feeding the
// main one. Its signal reaches both, bit for bit.
class OutputSendsTest final : public mp::test::Test {
public:
  OutputSendsTest() : Test("functional.output.sends", Category::Functional) {}
  void run() override {
    const Organ organ("mp-output-sends");
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organSettings(probe.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();

    const auto alone = render(organ.odf, 4, {36}, [](mp::MasterpieceProcessor& p) { pairsTo(p, 2, mp::BusId{2}); });
    MP_CHECK(peakOf(alone[0]) == 0.0f && peakOf(alone[2]) > 0.01f, "output 2 alone sounds on 3-4 only");
    const auto sent = render(organ.odf, 4, {36}, [](mp::MasterpieceProcessor& p) {
      pairsTo(p, 2, mp::BusId{2});
      p.setOutputSends(2, {1});
    });
    MP_CHECK(sent[2] == alone[2] && sent[3] == alone[3], "output 2 still sounds on 3-4 as it did");
    MP_CHECK(sent[0] == alone[2] && sent[1] == alone[3], "and plays into output 1 on 1-2, bit for bit");

    // A send at -6 dB: output 2 as it was, output 1 at half its level.
    const auto lower = render(organ.odf, 4, {36}, [](mp::MasterpieceProcessor& p) {
      pairsTo(p, 2, mp::BusId{2});
      p.setOutputSends(2, {1});
      p.setOutputSendLevel(2, 1, -6.0f);
    });
    MP_CHECK(lower[2] == alone[2], "a lower send leaves output 2 as it was");
    const float ratio = peakOf(lower[0]) / peakOf(alone[2]);
    MP_CHECK(std::fabs(ratio - 0.501f) < 0.002f, "the send at -6 dB: output 1 at " + std::to_string(ratio));
    // Output 1 made only of what output 2 sends: its own fader sets its level
    // (#1188). Down to nothing, it falls silent once the fader has glided
    // there; output 2 plays on as it was.
    const auto tailPeak = [](const std::vector<float>& c) {
      return peakOf(std::vector<float>(c.end() - static_cast<std::ptrdiff_t>(c.size() / 4), c.end()));
    };
    const auto faded = render(
        organ.odf, 4, {36},
        [](mp::MasterpieceProcessor& p) {
          pairsTo(p, 2, mp::BusId{2});
          p.setOutputSends(2, {1});
          p.setLivePairLevel(0, 0);
        },
        {901}, 80);
    const auto open = render(
        organ.odf, 4, {36},
        [](mp::MasterpieceProcessor& p) {
          pairsTo(p, 2, mp::BusId{2});
          p.setOutputSends(2, {1});
        },
        {901}, 80);
    MP_CHECK(tailPeak(open[0]) > 0.01f, "with its fader at full, output 1 sounds: " + std::to_string(tailPeak(open[0])));
    MP_CHECK(tailPeak(faded[0]) < 1.0e-4f,
             "its fader down, output 1 falls silent: " + std::to_string(tailPeak(faded[0])));
    MP_CHECK(faded[2] == open[2], "and output 2 plays on as it was");
    // Two outputs playing into each other: one of the sends is left out, and
    // nothing feeds back.
    const auto loop = render(organ.odf, 4, {36}, [](mp::MasterpieceProcessor& p) {
      pairsTo(p, 2, mp::BusId{2});
      p.setOutputSends(2, {1});
      p.setOutputSends(1, {2});
    });
    MP_CHECK(loop[2] == alone[2] && loop[0] == alone[2], "a loop of sends plays once: " +
                                                             std::to_string(peakOf(loop[0])) + " " +
                                                             std::to_string(peakOf(loop[2])));

    mp::MasterpieceProcessor p;
    p.setPairCount(2);
    p.setOutputSends(2, {1, 2});
    p.setOutputSendLevel(2, 1, -4.5f);
    p.setOutputName(1, "Main mix");
    MP_CHECK(p.mixer().buses[1].sends == std::vector<int>{1}, "an output does not play into itself");
    MP_CHECK(p.saveGlobalDefaults(), "the global file is written");
    const auto text = global.file.loadFileAsString();
    MP_CHECK(text.contains("buses 1:0,1 2:2,3>1@-4.5") && text.contains("busname 1 Main mix"),
             "the sends and the name are saved: " + text.toStdString());
    mp::MasterpieceProcessor next;
    next.loadGlobalDefaults();
    MP_CHECK(next.mixer().buses.size() == 2 && next.mixer().buses[1].sends == std::vector<int>{1} &&
                 next.mixer().buses[1].sendLevelDb(1) == -4.5f && next.mixer().buses[0].name == "Main mix",
             "and come back, with the send's level");
    organ.root.deleteRecursively();
  }
};
static OutputSendsTest g_outputSends;

// The outputs, speaker groups and default destination are the machine's: in
// the global file, the same for every organ. An organ file's own copy, from
// before, is read only while the machine has none, and then becomes it.
class MachineMixerTest final : public mp::test::Test {
public:
  MachineMixerTest() : Test("functional.output.machine-mixer", Category::Functional) {}
  void run() override {
    const Organ organ("mp-machine-mixer", 2);
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organSettings(probe.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();
    organSettings.file.getParentDirectory().createDirectory();

    // The machine has four outputs and a group as the default.
    {
      mp::MasterpieceProcessor p;
      p.setPairCount(4);
      mp::BusGroup g;
      g.groupId = 1;
      g.members = {mp::BusId{1}, mp::BusId{2}, mp::BusId{3}, mp::BusId{4}};
      p.setSpeakerGroups({g});
      p.setDefaultDestination(1);
      MP_CHECK(p.saveGlobalDefaults(), "the global file is written");
      const auto text = global.file.loadFileAsString();
      MP_CHECK(text.contains("defaultroute group 1") && text.contains("groups 1:1,2,3,4"),
               "the default and the group are in it: " + text.toStdString());
    }
    // An organ file with outputs of its own, from before.
    organSettings.file.replaceWithText("buses 1:0,1 2:2,3\nroute 201 bus 2\n");
    {
      mp::MasterpieceProcessor p;
      p.loadGlobalDefaults();
      MP_CHECK(p.loadOrgan(organ.odf, 0, false).ok, "the organ loads");
      MP_CHECK(p.mixer().buses.size() == 4, "the machine's four outputs stay, the organ file's two are left alone");
      const auto d = p.mixer().defaultDest;
      MP_CHECK(std::holds_alternative<int>(d) && std::get<int>(d) == 1, "the default is the group");
      const auto r = p.mixer().routingFor(201).perspectives[0].dest;
      MP_CHECK(std::holds_alternative<mp::BusId>(r) && std::get<mp::BusId>(r).value == 2,
               "the organ's own route to output 2 holds");
      const auto other = p.mixer().routingFor(999).perspectives[0].dest;
      MP_CHECK(std::holds_alternative<int>(other) && std::get<int>(other) == 1,
               "a rank with no route follows the default");
    }
    // 0.9.0 and the preview wrote a route to output 1 for every rank, the
    // only default they had (#1023). Read from a file of then, output 1
    // follows the default; any other output holds.
    organSettings.file.replaceWithText("route 201 bus 1\nroute 202 bus 2\n");
    {
      mp::MasterpieceProcessor p;
      p.loadGlobalDefaults();
      MP_CHECK(p.loadOrgan(organ.odf, 0, false).ok, "the organ loads with routes from before");
      const auto r1 = p.mixer().routingFor(201).perspectives[0].dest;
      MP_CHECK(std::holds_alternative<int>(r1) && std::get<int>(r1) == 1,
               "an old route to output 1 follows the default, the group");
      const auto r2 = p.mixer().routingFor(202).perspectives[0].dest;
      MP_CHECK(std::holds_alternative<mp::BusId>(r2) && std::get<mp::BusId>(r2).value == 2,
               "an old route to output 2 holds");
      // Chosen by hand now, output 1 is saved as a choice.
      p.setRankRoutes({{201, mp::BusId{1}}});
      MP_CHECK(p.saveSettings(), "the organ's settings are saved");
      const auto text = organSettings.file.loadFileAsString();
      MP_CHECK(text.contains("routing 2") && text.contains("route 201 bus 1"),
               "the file says its routes are chosen by hand: " + text.toStdString());
    }
    {
      mp::MasterpieceProcessor p;
      p.loadGlobalDefaults();
      MP_CHECK(p.loadOrgan(organ.odf, 0, false).ok, "the organ loads with routes chosen by hand");
      const auto r1 = p.mixer().routingFor(201).perspectives[0].dest;
      MP_CHECK(std::holds_alternative<mp::BusId>(r1) && std::get<mp::BusId>(r1).value == 1,
               "a route to output 1 chosen by hand holds");
    }
    // A machine with no mixer of its own takes the organ file's.
    global.file.replaceWithText("");
    organSettings.file.replaceWithText("buses 1:0,1 2:2,3 3:4,5\n");
    {
      mp::MasterpieceProcessor p;
      p.loadGlobalDefaults();
      MP_CHECK(p.loadOrgan(organ.odf, 0, false).ok, "the organ loads again");
      MP_CHECK(p.mixer().buses.size() == 3, "the organ file's three outputs become the machine's");
      MP_CHECK(p.saveSettingsIfDirty(), "and are saved");
      MP_CHECK(global.file.loadFileAsString().contains("buses 1:0,1 2:2,3 3:4,5"),
               "to the global file: " + global.file.loadFileAsString().toStdString());
    }
    organ.root.deleteRecursively();
  }
};
static MachineMixerTest g_machineMixer;

float worstDiff(const Channels& a, const Channels& b, size_t from) {
  float worst = 0.0f;
  for (size_t ch = 0; ch < a.size() && ch < b.size(); ++ch)
    for (size_t i = from; i < a[ch].size() && i < b[ch].size(); ++i)
      worst = std::max(worst, std::fabs(a[ch][i] - b[ch][i]));
  return worst;
}

// A mix's fader, mute and solo reach its own ranks and no others. Every rank
// outside a mix plays as it did.
class MixStripTest final : public mp::test::Test {
public:
  MixStripTest() : Test("functional.output.mixes.strip", Category::Functional) {}
  void run() override {
    const Organ organ("mp-mixes-strip", 2);
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organSettings(probe.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();

    const std::vector<mp::Id> both = {901, 902};
    const auto mixOf202 = [](mp::MasterpieceProcessor& p) {
      mp::MasterpieceProcessor::RankMix m;
      m.id = 3;
      m.name = "Swell";
      m.ranks = {202};
      p.setRankMixes({m});
    };
    const auto none = [](mp::MasterpieceProcessor&) {};
    // Eighty blocks, 427 ms: a fader glides to where it is set over some 30 ms
    // and lands on it within 1e-4, some 52 blocks in.
    constexpr int blocks = 80;
    const auto a = render(organ.odf, 2, {36}, none, {901}, blocks);
    const auto b = render(organ.odf, 2, {36}, none, {902}, blocks);
    const auto ab = render(organ.odf, 2, {36}, none, both, blocks);
    const size_t tail = 60 * kBlock;

    const auto mixed = render(organ.odf, 2, {36}, mixOf202, both, blocks);
    MP_CHECK(mixed == ab, "a mix at full changes nothing, bit for bit");
    const auto down = render(organ.odf, 2, {36}, [&](mp::MasterpieceProcessor& p) {
      mixOf202(p);
      MP_CHECK(p.liveMixOfRank(p.liveRankIndexOf(202)) == 2 && p.liveMixOfRank(p.liveRankIndexOf(201)) == -1,
               "rank 202 is in mix 3, rank 201 in none");
      p.setLiveMixLevel(2, 0);
    }, both, blocks);
    MP_CHECK(worstDiff(down, a, tail) < 1.0e-5f,
             "mix 3 at 0: rank 201 alone, off by " + std::to_string(worstDiff(down, a, tail)));
    const auto muted = render(organ.odf, 2, {36}, [&](mp::MasterpieceProcessor& p) {
      mixOf202(p);
      p.setLiveMuted(mp::LiveLevels::Strip::Mix, 2, true);
    }, both, blocks);
    MP_CHECK(worstDiff(muted, a, tail) < 1.0e-5f,
             "mix 3 muted: rank 201 alone, off by " + std::to_string(worstDiff(muted, a, tail)));
    // Both move as a glide: some of rank 202 still sounds ten blocks in.
    for (size_t from : {size_t{2} * kBlock, size_t{10} * kBlock, size_t{30} * kBlock})
      std::printf("        glide from block %d: fader off by %.6f, mute off by %.6f\n", static_cast<int>(from / kBlock),
                  worstDiff(down, a, from), worstDiff(muted, a, from));
    const auto solo = render(organ.odf, 2, {36}, [&](mp::MasterpieceProcessor& p) {
      mixOf202(p);
      p.setLiveSoloed(mp::LiveLevels::Strip::Mix, 2, true);
    }, both, blocks);
    MP_CHECK(worstDiff(solo, b, tail) < 1.0e-5f,
             "mix 3 soloed: rank 202 alone, off by " + std::to_string(worstDiff(solo, b, tail)));
    const auto other = render(organ.odf, 2, {36}, [&](mp::MasterpieceProcessor& p) {
      mixOf202(p);
      p.setLiveMixLevel(0, 0);
      p.setLiveMuted(mp::LiveLevels::Strip::Mix, 5, true);
    }, both, blocks);
    MP_CHECK(worstDiff(other, ab, 0) < 1.0e-6f, "another mix's fader and mute, with no ranks, change nothing");

    // A knob learned for mix 3.
    mp::MasterpieceProcessor p;
    p.prepareToPlay(kRate, kBlock);
    MP_CHECK(p.loadOrgan(organ.odf, 0, false).ok, "the organ loads");
    mp::MidiBinding knob;
    knob.source.kind = mp::MidiSourceKind::ControlChange;
    knob.source.channel = 2;
    knob.source.number = 11;
    knob.targetKind = mp::MidiTargetKind::MixLevel;
    knob.targetId = 3;
    p.midiMap().bind(knob);
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer cc;
    cc.addEvent(juce::MidiMessage::controllerEvent(2, 11, 40), 0);
    p.processBlock(buffer, cc);
    MP_CHECK(p.liveMixLevel(2) == 40 && p.liveMixLevel(0) == 127, "the knob sets mix 3's fader and no other");
    MP_CHECK(mp::MidiMap::isContinuousTarget(mp::MidiTargetKind::MixLevel) &&
                 !mp::MidiMap::isConsoleTarget(mp::MidiTargetKind::MixLevel),
             "a mix's level is continuous and belongs to the organ");
    const std::string text = p.midiMap().toText();
    mp::MidiMap again;
    MP_CHECK(text.find("mix-level") != std::string::npos && again.fromText(text) &&
                 again.bindingFor(mp::MidiTargetKind::MixLevel, 3) != nullptr,
             "named mix-level in the map file, and read back");
    p.releaseResources();

    // Mute and solo buttons: a mix's target is apart from every other strip's.
    const mp::Id muteMix = mp::stripTarget(mp::kStripMix, 3);
    MP_CHECK(mp::stripTargetStrip(muteMix) == mp::kStripMix && mp::stripTargetLevelId(muteMix) == 3,
             "a mix's strip target carries the mix");
    for (int strip = 0; strip < 4; ++strip)
      for (mp::Id level : {mp::Id{1}, mp::Id{3}, mp::Id{201}, mp::Id{99999}}) {
        const mp::Id t = mp::stripTarget(strip, level);
        MP_CHECK(t == level * 4 + strip && mp::stripTargetStrip(t) == strip && mp::stripTargetLevelId(t) == level,
                 "strip " + std::to_string(strip) + " level " + std::to_string(level) + " keeps its old target");
      }
    organ.root.deleteRecursively();
  }
};
static MixStripTest g_mixStrip;

// The mixes are kept with the organ: names, ranks and ids come back, a rank
// stays in the first mix that names it, and a settings file without mixes
// reads as it did.
class MixSettingsTest final : public mp::test::Test {
public:
  MixSettingsTest() : Test("functional.output.mixes.settings", Category::Functional) {}
  void run() override {
    const Organ organ("mp-mixes-settings", 3);
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organSettings(probe.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();
    organSettings.file.deleteFile();

    {
      mp::MasterpieceProcessor p;
      p.prepareToPlay(kRate, kBlock);
      MP_CHECK(p.loadOrgan(organ.odf, 0, false).ok, "the organ loads");
      MP_CHECK(p.rankMixes().empty(), "a new organ has no mixes");
      mp::MasterpieceProcessor::RankMix pedal, swell, bad;
      pedal.id = 5;
      pedal.name = "Pedal reeds";
      pedal.ranks = {201};
      swell.id = 2;
      swell.name = "Swell";
      swell.ranks = {202, 201};
      bad.id = 99;
      bad.name = "Too far";
      bad.ranks = {203};
      p.setRankMixes({pedal, swell, bad});
      const auto& kept = p.rankMixes();
      MP_CHECK(kept.size() == 2 && kept[0].id == 2 && kept[1].id == 5, "ids 2 and 5 kept in order, 99 dropped");
      MP_CHECK(kept.size() == 2 && kept[0].ranks == std::vector<mp::Id>{202} &&
                   kept[1].ranks == std::vector<mp::Id>{201},
               "rank 201 stays in the first mix that names it");
      MP_CHECK(p.saveSettings(), "the organ's settings are written");
      p.releaseResources();
    }
    const auto text = organSettings.file.loadFileAsString();
    MP_CHECK(text.contains("mix 2 202 Swell") && text.contains("mix 5 201 Pedal reeds"),
             "the mix lines: " + text.toStdString());

    mp::MasterpieceProcessor next;
    next.prepareToPlay(kRate, kBlock);
    MP_CHECK(next.loadOrgan(organ.odf, 0, false).ok, "the organ loads again");
    const auto& back = next.rankMixes();
    MP_CHECK(back.size() == 2 && back[0].name == "Swell" && back[1].name == "Pedal reeds" &&
                 back[1].ranks == std::vector<mp::Id>{201},
             "both mixes come back with their names and ranks");
    MP_CHECK(next.liveMixOfRank(next.liveRankIndexOf(202)) == 1 && next.liveMixOfRank(next.liveRankIndexOf(201)) == 4 &&
                 next.liveMixOfRank(next.liveRankIndexOf(203)) == -1,
             "and the engine plays each rank through its mix");
    next.releaseResources();

    // An empty mix keeps its place.
    organSettings.file.replaceWithText("mix 1 - Empty one\n");
    mp::MasterpieceProcessor empty;
    empty.prepareToPlay(kRate, kBlock);
    MP_CHECK(empty.loadOrgan(organ.odf, 0, false).ok, "the organ loads with an empty mix");
    MP_CHECK(empty.rankMixes().size() == 1 && empty.rankMixes()[0].ranks.empty() &&
                 empty.rankMixes()[0].name == "Empty one",
             "a mix with no ranks comes back");
    empty.releaseResources();
    organ.root.deleteRecursively();
  }
};
static MixSettingsTest g_mixSettings;

// The crossover changed while the organ plays (#1159, #1188): each output's
// filters, delay and polarity chosen over and over, as the panel's boxes do,
// while another thread renders twelve pairs on 24 channels with every key
// held, under the callback lock as the device's player renders. Every block
// stays finite, and the last setting is the one in force.
class CrossoverWhilePlayingTest final : public mp::test::Test {
public:
  CrossoverWhilePlayingTest() : Test("functional.output.crossover-while-playing", Category::Functional) {}
  void run() override {
    const Organ organ("mp-crossover-while-playing");
    mp::MasterpieceProcessor proc;
    Keep global(proc.globalSettingsFile());
    Keep organSettings(proc.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();
    constexpr int kOutputs = 24;
    juce::AudioProcessor::BusesLayout layout;
    layout.outputBuses.add(juce::AudioChannelSet::discreteChannels(kOutputs));
    MP_CHECK(proc.setBusesLayout(layout), "the processor takes 24 outputs");
    proc.prepareToPlay(kRate, kBlock);
    MP_CHECK(proc.loadOrgan(organ.odf, 0, false).ok, "the organ loads");
    proc.setPairCount(12);
    proc.setRankRoutes({{kRank, mp::BusId{1}}});
    proc.setStopEngaged(901, true);

    std::atomic<bool> stop{false};
    std::atomic<int> blocks{0};
    std::atomic<bool> finite{true};
    std::thread audio([&] {
      juce::AudioBuffer<float> buffer(kOutputs, kBlock);
      juce::MidiBuffer on, none;
      for (int k = 36; k < 48; ++k) on.addEvent(juce::MidiMessage::noteOn(1, k, 0.8f), 0);
      bool first = true;
      while (!stop.load()) {
        buffer.clear();
        {
          const juce::ScopedLock lock(proc.getCallbackLock());
          proc.processBlock(buffer, first ? on : none);
        }
        first = false;
        for (int ch = 0; ch < kOutputs; ++ch)
          for (int i = 0; i < kBlock; ++i)
            if (!std::isfinite(buffer.getSample(ch, i))) finite = false;
        ++blocks;
      }
    });
    const int frequencies[] = {0, 40, 80, 200, 1000, 3000, 12000, 20000};
    mp::BusFilters last;
    for (int turn = 0; turn < 4000; ++turn) {
      mp::BusFilters f;
      f.highPassHz = static_cast<float>(frequencies[turn % 8]);
      f.highPassType = turn % 4;
      f.lowPassHz = static_cast<float>(frequencies[(turn / 8) % 8]);
      f.lowPassType = (turn / 4) % 4;
      f.delayMs = static_cast<float>(turn % 5) * 2.5f;
      f.invert = turn % 3 == 0;
      proc.setBusFilters(1 + turn % 12, f);
      if (turn % 64 == 0) std::this_thread::yield();
    }
    last.highPassHz = 80.0f;
    last.delayMs = 1.0f;
    proc.setBusFilters(1, last);
    const int before = blocks.load();
    while (blocks.load() < before + 8) std::this_thread::yield();
    stop = true;
    audio.join();
    MP_CHECK(finite.load(), "every block finite while the crossovers change");
    MP_CHECK(proc.busFilters(1) == last, "output 1 ends with a high-pass at 80 Hz and 1 ms of delay");
    proc.releaseResources();
    organ.root.deleteRecursively();
  }
};
static CrossoverWhilePlayingTest g_crossoverWhilePlaying;

// Every output setting changed at random while the organ plays (#1023): the
// output count, stereo and mono, channels the device has and channels it
// lacks, names, sends in chains and in loops at every level, speaker groups,
// routes to outputs and groups that are gone, mixes, bass. Between rounds the
// device goes away and comes back with another channel count and block size,
// as a player unplugging an interface does. Every block stays finite; the
// settings file reads back to itself; the same settings render the same
// twice. MP_FUZZ_SEED picks another sequence.
class OutputFuzzTest final : public mp::test::Test {
public:
  OutputFuzzTest() : Test("functional.output.fuzz-while-playing", Category::Functional) {}
  void run() override {
    const Organ organ("mp-output-fuzz", 4);
    mp::MasterpieceProcessor proc;
    Keep global(proc.globalSettingsFile());
    Keep organSettings(proc.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();
    const auto seedText = juce::SystemStats::getEnvironmentVariable("MP_FUZZ_SEED", "1023");
    std::mt19937 rng(static_cast<unsigned>(seedText.getLargeIntValue()));
    const auto roll = [&rng](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    const char* names[] = {"Front", "Gallery left", "a>b@c,d:e 1:2", "Gal\xc3\xa9rie", "line\nbreak\ttab", "  ", ""};

    struct Device {
      int channels, block;
    };
    const Device devices[] = {{24, 256}, {2, 64}, {8, 512}, {32, 128}, {1, 256}, {64, 1024}, {6, 96}};
    bool loaded = false;
    std::string firstFailure;
    for (const auto& device : devices) {
      // The device as the player opens it.
      juce::AudioProcessor::BusesLayout layout;
      layout.outputBuses.add(device.channels == 2   ? juce::AudioChannelSet::stereo()
                             : device.channels == 1 ? juce::AudioChannelSet::mono()
                                                    : juce::AudioChannelSet::discreteChannels(device.channels));
      if (!proc.setBusesLayout(layout)) continue;
      proc.prepareToPlay(kRate, device.block);
      if (!loaded) {
        MP_CHECK(proc.loadOrgan(organ.odf, 0, false).ok, "the organ loads");
        for (mp::Id stop : {901, 902, 903, 904}) proc.setStopEngaged(stop, true);
        loaded = true;
      }

      std::atomic<bool> stop{false};
      std::atomic<int> blocks{0};
      std::atomic<bool> finite{true};
      std::thread audio([&] {
        juce::AudioBuffer<float> buffer(device.channels, device.block);
        juce::MidiBuffer on, off, none;
        for (int k = 36; k < 48; ++k) on.addEvent(juce::MidiMessage::noteOn(1, k, 0.8f), 0);
        for (int k = 36; k < 48; k += 3) off.addEvent(juce::MidiMessage::noteOff(1, k), 0);
        int n = 0;
        while (!stop.load()) {
          // Whole blocks and part blocks, as a driver gives them.
          const int frames = n % 5 == 4 ? 1 + n % device.block : device.block;
          juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), device.channels, frames);
          view.clear();
          {
            const juce::ScopedLock lock(proc.getCallbackLock());
            proc.processBlock(view, n % 40 == 0 ? on : n % 40 == 20 ? off : none);
          }
          for (int ch = 0; ch < device.channels; ++ch)
            for (int i = 0; i < frames; ++i)
              if (!std::isfinite(view.getSample(ch, i))) finite = false;
          ++n;
          ++blocks;
        }
      });

      for (int turn = 0; turn < 1500; ++turn) {
        const int outputs = static_cast<int>(proc.mixer().buses.size());
        const int bus = roll(1, std::max(1, outputs) + 1);  // now and then one that is gone
        switch (roll(0, 11)) {
          case 0: proc.setPairCount(roll(1, 12)); break;
          case 1: {
            std::vector<int> ch;
            for (int i = roll(0, 4); i > 0; --i) ch.push_back(roll(0, 70));  // past the device too
            proc.setPairLayout(bus, roll(0, 1) == 1, ch);
            break;
          }
          case 2: {
            std::vector<int> sends;
            for (int i = roll(0, 3); i > 0; --i) sends.push_back(roll(1, outputs + 1));
            proc.setOutputSends(bus, sends);
            break;
          }
          case 3: proc.setOutputSendLevel(bus, roll(1, outputs + 1), static_cast<float>(roll(-90, 20))); break;
          case 4: proc.setOutputName(bus, names[roll(0, 6)]); break;
          case 5: {
            std::vector<mp::BusGroup> groups;
            for (int g = roll(0, 3); g > 0; --g) {
              mp::BusGroup group;
              group.groupId = g;
              for (int m = roll(0, 4); m > 0; --m) group.members.push_back(mp::BusId{roll(0, outputs + 1)});
              group.algorithm = static_cast<mp::AllocationAlgorithm>(roll(0, 2));
              groups.push_back(group);
            }
            proc.setSpeakerGroups(groups);
            break;
          }
          case 6: {
            std::vector<std::pair<mp::Id, std::variant<mp::BusId, int>>> routes;
            for (mp::Id r : {201, 202, 203, 204})
              if (roll(0, 2) > 0)
                routes.push_back({r, roll(0, 1) == 0 ? std::variant<mp::BusId, int>(mp::BusId{roll(0, outputs + 1)})
                                                     : std::variant<mp::BusId, int>(roll(1, 4))});
            proc.setRankRoutes(routes);
            break;
          }
          case 7:
            proc.setDefaultDestination(roll(0, 1) == 0 ? std::variant<mp::BusId, int>(mp::BusId{roll(1, outputs)})
                                                       : std::variant<mp::BusId, int>(roll(1, 4)));
            break;
          case 8: {
            std::vector<mp::MasterpieceProcessor::RankMix> mixes;
            for (int m = roll(0, 3); m > 0; --m) {
              mp::MasterpieceProcessor::RankMix mix;
              mix.id = m;
              mix.name = names[roll(0, 6)];
              for (mp::Id r : {201, 202, 203, 204})
                if (roll(0, 1) == 1) mix.ranks.push_back(r);
              mixes.push_back(mix);
            }
            proc.setRankMixes(mixes);
            break;
          }
          case 9: {
            mp::BusFilters f;
            f.highPassHz = static_cast<float>(roll(0, 4) * 40);
            f.highPassType = roll(-1, 5);
            f.lowPassHz = static_cast<float>(roll(0, 4) * 5000);
            f.delayMs = static_cast<float>(roll(-1, 120));
            f.invert = roll(0, 1) == 1;
            proc.setBusFilters(bus, f);
            break;
          }
          case 10: proc.setLivePairLevel(roll(0, 11), roll(0, 203)); break;
          default: std::this_thread::yield(); break;
        }
        if (turn % 50 == 0) {
          const int before = blocks.load();
          while (blocks.load() < before + 1) std::this_thread::yield();
        }
      }
      const int before = blocks.load();
      while (blocks.load() < before + 4) std::this_thread::yield();
      stop = true;
      audio.join();
      if (!finite.load() && firstFailure.empty())
        firstFailure = std::to_string(device.channels) + " channels, block " + std::to_string(device.block);
      // The device goes away.
      proc.releaseResources();
    }
    MP_CHECK(loaded, "a layout was taken");
    MP_CHECK(firstFailure.empty(), "every block finite, first failure on " + firstFailure + ", seed " +
                                       seedText.toStdString());

    // The settings read back to themselves.
    MP_CHECK(proc.saveGlobalDefaults(), "the global file is written");
    const auto first = global.file.loadFileAsString();
    MP_CHECK(!first.contains("line\n") && !first.contains("\t"), "a name stays on its line: " + first.toStdString());
    {
      mp::MasterpieceProcessor next;
      next.loadGlobalDefaults();
      MP_CHECK(next.saveGlobalDefaults(), "the global file is written again");
    }
    const auto second = global.file.loadFileAsString();
    MP_CHECK(first == second, "the settings file reads back to itself, seed " + seedText.toStdString() + ":\n" +
                                  first.toStdString() + "\n---\n" + second.toStdString());

    // The settings the fuzz left render the same twice.
    const auto again = [&] {
      return render(organ.odf, 24, {36, 38, 41, 43}, [](mp::MasterpieceProcessor& p) { p.loadGlobalDefaults(); },
                    {901, 902, 903, 904}, 12);
    };
    const auto a = again(), b = again();
    MP_CHECK(a == b, "the same settings render the same twice");
    organ.root.deleteRecursively();
  }
};
static OutputFuzzTest g_outputFuzz;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
