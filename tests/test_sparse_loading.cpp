// Sparse loading (#450): how many notes of a rank are loaded, and what the
// notes that are not play instead.
//
// The model function is JUCE-free and runs in the fast loop. The rest needs an
// organ with real samples, and runs in the full build.
#include "MpTest.h"

#include "../src/mp_core/OrganModel.h"
#include "../src/mp_core/SparseLoading.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(MP_TEST_HAS_AUDIO)
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/SampleLibrary.h"
#endif

using mp::test::Category;

namespace {

using mp::Id;

// A rank of pipes, each with one layer and one attack sample of its own.
void addRank(mp::OrganModel& model, Id rankId, int firstNote, int lastNote,
             Id sampleBase, Id pipeBase, bool noise = false, bool percussive = false,
             int harmonic = 8) {
  mp::Rank rank;
  rank.rankId = rankId;
  rank.name = "Rank " + std::to_string(rankId);
  rank.isNoise = noise;
  rank.percussive = percussive;
  for (int note = firstNote; note <= lastNote; ++note) {
    const int index = note - firstNote;
    mp::Pipe pipe;
    pipe.pipeId = pipeBase + index;
    pipe.midiNote = note;
    pipe.basePitch64ftHarmonicNum = harmonic;
    mp::PipeLayer layer;
    layer.layerId = pipe.pipeId * 10;
    mp::AttackSample attack;
    attack.id = layer.layerId + 1;
    attack.sample.sampleId = sampleBase + index;
    attack.sample.fileName = std::to_string(attack.sample.sampleId) + ".wav";
    layer.attacks.push_back(attack);
    pipe.layers.push_back(layer);
    rank.pipes.push_back(pipe);
    model.samples[attack.sample.sampleId] = attack.sample;
  }
  model.ranks[rankId] = std::move(rank);
}

const mp::Pipe* pipeForNote(const mp::OrganModel& model, Id rankId, int note) {
  const auto it = model.ranks.find(rankId);
  if (it == model.ranks.end()) return nullptr;
  for (const auto& pipe : it->second.pipes)
    if (pipe.midiNote == note) return &pipe;
  return nullptr;
}

const mp::Pipe* pipeForId(const mp::OrganModel& model, Id rankId, Id pipeId) {
  const auto it = model.ranks.find(rankId);
  if (it == model.ranks.end()) return nullptr;
  for (const auto& pipe : it->second.pipes)
    if (pipe.pipeId == pipeId) return &pipe;
  return nullptr;
}

// The sample ids a pipe's layers name.
std::vector<Id> sampleIdsOf(const mp::Pipe& pipe) {
  std::vector<Id> ids;
  for (const auto& layer : pipe.layers)
    for (const auto& a : layer.attacks) ids.push_back(a.sample.sampleId);
  return ids;
}

// Every sample is there to be read.
const auto kAllPresent = [](Id) { return true; };

}  // namespace

// The model function on a generated model: which pipes are kept, which
// neighbour each dropped pipe borrows from, and that every one changes
// nothing at all.
class SparseModelTest final : public mp::test::Test {
public:
  SparseModelTest() : Test("functional.sparse.model", Category::Functional) {}

  void run() override {
    // Every second note: 36, 38, ... 48 keep their own samples.
    {
      mp::OrganModel model;
      addRank(model, 201, 36, 48, 1001, 3001);
      const auto plan = mp::planSparseLoading(model, 2, {}, kAllPresent);
      MP_CHECK(plan.borrowed == 6, "six pipes of thirteen borrow a neighbour, got " +
                                       std::to_string(plan.borrowed));
      mp::applySparsePlan(model, plan);
      for (int note = 36; note <= 48; ++note) {
        const mp::Pipe* pipe = pipeForNote(model, 201, note);
        MP_CHECK(pipe != nullptr, "the pipe is still there");
        if (pipe == nullptr) continue;
        MP_CHECK(pipe->pipeId == static_cast<Id>(3001 + note - 36),
                 "the pipe keeps its own id");
        MP_CHECK(pipe->midiNote == note, "the pipe keeps its own note");
        if ((note - 36) % 2 == 0) {
          MP_CHECK(pipe->borrowedFromPipeId == 0, "a kept pipe plays its own samples");
          MP_CHECK(sampleIdsOf(*pipe) == std::vector<Id>{static_cast<Id>(1001 + note - 36)},
                   "and keeps its own sample");
          continue;
        }
        MP_CHECK(pipe->borrowedFromPipeId != 0, "a dropped pipe plays a neighbour's samples");
        const mp::Pipe* neighbour = pipeForId(model, 201, pipe->borrowedFromPipeId);
        MP_CHECK(neighbour != nullptr, "and the neighbour is a pipe of the rank");
        if (neighbour == nullptr) continue;
        MP_CHECK(std::abs(neighbour->midiNote - note) <= 1,
                 "the neighbour is at most a semitone away");
        MP_CHECK(sampleIdsOf(*pipe) == sampleIdsOf(*neighbour),
                 "the dropped pipe plays the neighbour's samples");
        MP_CHECK(neighbour->borrowedFromPipeId == 0, "the neighbour plays its own");
      }
    }

    // Every third note: 36, 39, 42, 45, 48 keep their own samples, and every
    // dropped pipe borrows from a neighbour at most a semitone away.
    {
      mp::OrganModel model;
      addRank(model, 201, 36, 48, 1001, 3001);
      const auto plan = mp::planSparseLoading(model, 3, {}, kAllPresent);
      mp::applySparsePlan(model, plan);
      for (int note = 36; note <= 48; ++note) {
        const mp::Pipe* pipe = pipeForNote(model, 201, note);
        if (pipe == nullptr) continue;
        if ((note - 36) % 3 == 0) {
          MP_CHECK(pipe->borrowedFromPipeId == 0, "36, 39, 42, 45 and 48 keep their samples");
          continue;
        }
        const mp::Pipe* neighbour = pipeForId(model, 201, pipe->borrowedFromPipeId);
        MP_CHECK(neighbour != nullptr, "a dropped pipe borrows from a neighbour");
        if (neighbour == nullptr) continue;
        MP_CHECK(std::abs(neighbour->midiNote - note) <= 1,
                 "the neighbour is at most a semitone away");
      }
    }

    // Every one: the model is left exactly as it was.
    {
      mp::OrganModel model;
      addRank(model, 201, 36, 48, 1001, 3001);
      std::vector<Id> idsBefore, samplesBefore;
      for (int note = 36; note <= 48; ++note) {
        const mp::Pipe* pipe = pipeForNote(model, 201, note);
        if (pipe == nullptr) continue;
        idsBefore.push_back(pipe->pipeId);
        const auto ids = sampleIdsOf(*pipe);
        samplesBefore.push_back(ids.empty() ? 0 : ids[0]);
      }
      const auto plan = mp::planSparseLoading(model, 1, {}, kAllPresent);
      MP_CHECK(plan.ranks.empty() && plan.borrowed == 0, "every note borrows nothing");
      mp::applySparsePlan(model, plan);
      size_t i = 0;
      for (int note = 36; note <= 48; ++note, ++i) {
        const mp::Pipe* pipe = pipeForNote(model, 201, note);
        if (pipe == nullptr) continue;
        MP_CHECK(pipe->borrowedFromPipeId == 0, "no pipe borrows");
        MP_CHECK(pipe->pipeId == idsBefore[i], "the pipe keeps its id");
        const auto ids = sampleIdsOf(*pipe);
        MP_CHECK((ids.empty() ? 0 : ids[0]) == samplesBefore[i], "the pipe keeps its samples");
      }
    }
  }
};
static SparseModelTest g_sparseModel;

// The ranks left alone, and the pipes that keep their own samples because a
// neighbour is no use.
class SparseRanksAloneTest final : public mp::test::Test {
public:
  SparseRanksAloneTest() : Test("functional.sparse.ranks-left-alone", Category::Functional) {}

  void run() override {
    // A noise, a percussive rank and a sampled tremulant's rank are left as
    // they are, whatever the setting says.
    {
      mp::OrganModel model;
      addRank(model, 201, 36, 48, 1001, 3001, /*noise*/ true);
      addRank(model, 202, 36, 48, 1101, 3101, /*percussive*/ true);
      addRank(model, 203, 36, 48, 1201, 3201);
      const std::unordered_set<Id> protectedRanks{203};
      const auto plan = mp::planSparseLoading(model, 2, protectedRanks, kAllPresent);
      MP_CHECK(plan.ranksConsidered == 0, "no rank is thinned");
      MP_CHECK(plan.ranksLeftAlone == 3, "all three are left alone");
      mp::applySparsePlan(model, plan);
      for (Id rankId : {Id(201), Id(202), Id(203)})
        for (int note = 36; note <= 48; ++note) {
          const mp::Pipe* pipe = pipeForNote(model, rankId, note);
          MP_CHECK(pipe != nullptr && pipe->borrowedFromPipeId == 0,
                   "the rank keeps its own samples");
        }
    }

    // A rank of six pipes is too small to thin out.
    {
      mp::OrganModel model;
      addRank(model, 204, 36, 41, 1301, 3301);
      const auto plan = mp::planSparseLoading(model, 2, {}, kAllPresent);
      MP_CHECK(plan.borrowed == 0, "a rank of six pipes is left alone");
      MP_CHECK(plan.ranksLeftAlone == 1, "and it is counted as left alone");
    }

    // A neighbour a semitone away but a different footage: borrowing across a
    // mixture break would jump an octave.
    {
      mp::OrganModel model;
      addRank(model, 205, 36, 48, 1401, 3401);
      auto& rank = model.ranks[205];
      for (auto& pipe : rank.pipes)
        if (pipe.midiNote == 37) pipe.basePitch64ftHarmonicNum = 16;
      const auto plan = mp::planSparseLoading(model, 2, {}, kAllPresent);
      mp::applySparsePlan(model, plan);
      const mp::Pipe* pipe = pipeForNote(model, 205, 37);
      MP_CHECK(pipe != nullptr && pipe->borrowedFromPipeId == 0,
               "a pipe of another footage keeps its own samples");
      const mp::Pipe* other = pipeForNote(model, 205, 39);
      MP_CHECK(other != nullptr && other->borrowedFromPipeId != 0,
               "the rest of the rank is thinned as usual");
    }

    // A pipe whose own sample is read anyway, by a pipe that keeps it:
    // borrowing would save nothing and change what the pipe sounds like.
    {
      mp::OrganModel model;
      addRank(model, 206, 36, 48, 1501, 3501);
      auto& rank = model.ranks[206];
      for (auto& pipe : rank.pipes)
        if (pipe.midiNote == 37) pipe.layers[0].attacks[0].sample.sampleId = 1501;
      const auto plan = mp::planSparseLoading(model, 2, {}, kAllPresent);
      mp::applySparsePlan(model, plan);
      const mp::Pipe* pipe = pipeForNote(model, 206, 37);
      MP_CHECK(pipe != nullptr && pipe->borrowedFromPipeId == 0,
               "a pipe whose sample is read anyway keeps its own");
    }

    // A neighbour whose samples cannot be read: a demo leaves them out, and
    // the pipe keeps its own (which is missing too, and stays missing).
    {
      mp::OrganModel model;
      addRank(model, 207, 36, 48, 1601, 3601);
      const auto present = [](Id id) { return id != 1601; };  // 36's sample is gone
      const auto plan = mp::planSparseLoading(model, 2, {}, present);
      mp::applySparsePlan(model, plan);
      const mp::Pipe* pipe = pipeForNote(model, 207, 37);
      MP_CHECK(pipe != nullptr && pipe->borrowedFromPipeId == 0,
               "a pipe whose neighbour's sample is missing keeps its own");
      const mp::Pipe* other = pipeForNote(model, 207, 39);
      MP_CHECK(other != nullptr && other->borrowedFromPipeId != 0,
               "the pipes whose neighbour is there are thinned as usual");
    }
  }
};
static SparseRanksAloneTest g_sparseRanksAlone;

#if defined(MP_TEST_HAS_AUDIO)

namespace {

// Concert pitch of a MIDI note.
double noteHz(int midi) { return 440.0 * std::pow(2.0, (midi - 69) / 12.0); }

// How the samples declare their pitch.
enum class PitchMode {
  Exact,  // the definition says the exact frequency
  None,   // the definition says the set applies no tuning
  File,   // the definition says the pitch is in the file
};

// One tone, as a WAV file. A loop of whole periods, so the loop point is
// silent, and the file's own note for the mode that declares the pitch there.
void writeTone(const juce::File& file, double hz, int frames, int midiNote) {
  file.getParentDirectory().createDirectory();
  file.deleteFile();
  juce::WavAudioFormat fmt;
  std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
  MP_CHECK(os != nullptr, "the temp wav is writable");
  juce::StringPairArray meta;
  const double period = 48000.0 / hz;
  const int loopLength = std::max(1, static_cast<int>(std::lround(30.0 * period)));
  // The loop sits late in the file, so the crossfade at its end falls outside
  // the second the pitch is measured over.
  const int loopStart = (frames * 3) / 4;
  if (loopStart + loopLength < frames) {
    // JUCE writes an smpl chunk from these flat metadata keys; see
    // juce_WavAudioFormat.cpp SMPLChunk.
    meta.set("NumSampleLoops", "1");
    meta.set("Loop0Start", juce::String(loopStart));
    meta.set("Loop0End", juce::String(loopStart + loopLength));
    meta.set("Loop0Type", "0");
    if (midiNote > 0) meta.set("MidiUnityNote", juce::String(midiNote));
  }
  std::unique_ptr<juce::AudioFormatWriter> w(
      fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
  MP_CHECK(w != nullptr, "the wav writer is made");
  juce::AudioBuffer<float> buf(1, frames);
  for (int i = 0; i < frames; ++i)
    buf.setSample(0, i, static_cast<float>(
                           0.4 * std::sin(2.0 * 3.141592653589793 * hz * i / 48000.0)));
  w->writeFromAudioSampleBuffer(buf, 0, frames);
}

// The frequency of a tone, from its zero crossings, with the place of each
// crossing worked out between the two samples that straddle it. A sine crosses
// zero twice in every period, so the count is halved.
double measureHz(const std::vector<float>& v, double sr) {
  double first = -1.0, last = -1.0;
  int crossings = 0;
  for (size_t i = 1; i < v.size(); ++i) {
    const double a = v[i - 1], b = v[i];
    if ((a < 0.0) == (b < 0.0)) continue;
    const double f = a / (a - b);
    const double pos = static_cast<double>(i - 1) + f;
    if (crossings == 0) first = pos;
    last = pos;
    ++crossings;
  }
  if (crossings < 2 || last <= first) return 0.0;
  return (crossings - 1) * sr / (2.0 * (last - first));
}

// Cents between two frequencies.
double cents(double a, double b) {
  if (a <= 0.0 || b <= 0.0) return 1e9;
  return 1200.0 * std::log2(a / b);
}

// The organ the tests play: one rank of thirteen pipes, each with its own
// recording at its own pitch, and its own release.
struct TestOrgan {
  juce::File root;
  juce::File odf;

  TestOrgan(const juce::String& name, PitchMode mode, int uniqueId) {
    root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
    root.deleteRecursively();
    const auto defs = root.getChildFile("OrganDefinitions");
    const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
    defs.createDirectory();
    pkg.createDirectory();
    const int attackFrames = 96000;   // two seconds
    const int releaseFrames = 48000;  // one second
    for (int note = 36; note <= 48; ++note) {
      const std::string base = (note < 10 ? "0" : "") + std::to_string(note);
      writeTone(pkg.getChildFile(base + "-c.wav"), noteHz(note), attackFrames,
                mode == PitchMode::File ? note : 0);
      writeTone(pkg.getChildFile(base + "-r.wav"), noteHz(note), releaseFrames, 0);
    }
    odf = defs.getChildFile("sparse" + juce::String(uniqueId) + ".Organ_Hauptwerk_xml");
    odf.replaceWithText(juce::String(organXml(mode, uniqueId)));
  }

  static std::string organXml(PitchMode mode, int uniqueId) {
    const char* method = mode == PitchMode::Exact ? "4" : mode == PitchMode::None ? "0" : "1";
    std::string xml =
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Sparse</Identification_Name>"
        "<Identification_UniqueOrganID>" + std::to_string(uniqueId) +
        "</Identification_UniqueOrganID></_General></ObjectList>"
        "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
        "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
        "<ObjectList ObjectType=\"Sample\">";
    for (int note = 36; note <= 48; ++note) {
      const std::string base = (note < 10 ? "0" : "") + std::to_string(note);
      for (int which = 0; which < 2; ++which) {
        xml += "<Sample><SampleID>" + std::to_string((which == 0 ? 1000 : 2000) + note) +
               "</SampleID><InstallationPackageID>1</InstallationPackageID>"
               "<SampleFilename>" + base + (which == 0 ? "-c.wav" : "-r.wav") + "</SampleFilename>"
               "<Pitch_SpecificationMethodCode>" + method + "</Pitch_SpecificationMethodCode>";
        if (mode == PitchMode::Exact)
          xml += "<Pitch_ExactSamplePitch>" + std::to_string(noteHz(note)) +
                 "</Pitch_ExactSamplePitch>";
        xml += "</Sample>";
      }
    }
    xml += "</ObjectList>"
           "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>Principal</Name></Rank></ObjectList>"
           "<ObjectList ObjectType=\"Pipe_SoundEngine01\">";
    for (int note = 36; note <= 48; ++note)
      xml += "<Pipe_SoundEngine01><PipeID>" + std::to_string(3000 + note) +
             "</PipeID><RankID>201</RankID><NormalMIDINoteNumber>" + std::to_string(note) +
             "</NormalMIDINoteNumber></Pipe_SoundEngine01>";
    xml += "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">";
    for (int note = 36; note <= 48; ++note)
      xml += "<Pipe_SoundEngine01_Layer><LayerID>" + std::to_string(30000 + note) +
             "</LayerID><PipeID>" + std::to_string(3000 + note) + "</PipeID></Pipe_SoundEngine01_Layer>";
    xml += "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">";
    for (int note = 36; note <= 48; ++note)
      xml += "<Pipe_SoundEngine01_AttackSample><UniqueID>" + std::to_string(40000 + note) +
             "</UniqueID><LayerID>" + std::to_string(30000 + note) + "</LayerID><SampleID>" +
             std::to_string(1000 + note) + "</SampleID></Pipe_SoundEngine01_AttackSample>";
    xml += "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_ReleaseSample\">";
    for (int note = 36; note <= 48; ++note)
      xml += "<Pipe_SoundEngine01_ReleaseSample><UniqueID>" + std::to_string(50000 + note) +
             "</UniqueID><LayerID>" + std::to_string(30000 + note) + "</LayerID><SampleID>" +
             std::to_string(2000 + note) + "</SampleID></Pipe_SoundEngine01_ReleaseSample>";
    xml +=
        "</ObjectList>"
        "<ObjectList ObjectType=\"Keyboard\">"
        "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
        "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
        "</ObjectList>"
        "<ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
        "<ObjectList ObjectType=\"KeyAction\">"
        "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision>"
        "<DestDivisionID>801</DestDivisionID><ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode>"
        "<MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
        "</ObjectList>"
        "<ObjectList ObjectType=\"Stop\">"
        "<Stop><StopID>901</StopID><Name>Principal 8</Name><DivisionID>801</DivisionID>"
        "<ControllingSwitchID>1101</ControllingSwitchID></Stop></ObjectList>"
        "<ObjectList ObjectType=\"StopRank\">"
        "<StopRank><StopID>901</StopID><RankID>201</RankID>"
        "<MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
        "<NumberOfMappedDivisionInputNodes>13</NumberOfMappedDivisionInputNodes></StopRank></ObjectList>"
        "<ObjectList ObjectType=\"Switch\">"
        "<Switch><SwitchID>1101</SwitchID><Name>Principal 8</Name><Latching>Y</Latching></Switch>"
        "</ObjectList></Hauptwerk>";
    return xml;
  }
};

// A file the test writes, put back as it was afterwards: a test must not
// leave the player's own settings changed.
struct KeptFile {
  juce::File file;
  bool had = false;
  juce::String before;

  explicit KeptFile(const juce::File& f) : file(f) {
    had = file.existsAsFile();
    before = had ? file.loadFileAsString() : juce::String();
    // The file starts empty: a test must not read what a previous run left
    // behind.
    file.getParentDirectory().createDirectory();
    file.deleteFile();
  }
  ~KeptFile() {
    if (had)
      file.replaceWithText(before);
    else
      file.deleteFile();
  }
};

// Where an organ's own settings live.
juce::File settingsFor(const juce::File& odf) {
  mp::MasterpieceProcessor probe;
  return probe.settingsFileFor(odf);
}

// The organ, loaded, with a stop drawn and a keyboard on channel 1.
struct LoadedOrgan {
  mp::MasterpieceProcessor proc;
  juce::AudioBuffer<float> buf;

  explicit LoadedOrgan(const juce::File& odf) {
    buf.setSize(2, 256);
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
    proc.setStopEngaged(901, true);
    proc.clearChannelAssignments();
    proc.setKeyboardForChannel(1, 701);
  }

  void block(juce::MidiBuffer& midi) {
    buf.clear();
    proc.processBlock(buf, midi);
  }

  // The rendered sound, as one channel of it.
  std::vector<float> render(int frames) {
    std::vector<float> out;
    out.reserve(static_cast<size_t>(frames));
    while (static_cast<int>(out.size()) < frames) {
      juce::MidiBuffer empty;
      block(empty);
      const int take = std::min(frames - static_cast<int>(out.size()), 256);
      for (int i = 0; i < take; ++i) out.push_back(buf.getSample(0, i));
    }
    return out;
  }
};

// The organ loaded with every second note: its own settings file says so.
std::unique_ptr<LoadedOrgan> loadSparse(const juce::File& odf, int every, KeptFile& kept) {
  // The settings file is written after the first load, which is what tells
  // the program where the organ's own settings live.
  auto probe = std::make_unique<LoadedOrgan>(odf);
  kept.file.getParentDirectory().createDirectory();
  MP_CHECK(kept.file.replaceWithText("sparse " + juce::String(every) + "\n"),
           "the organ's settings file is written");
  auto organ = std::make_unique<LoadedOrgan>(odf);
  MP_CHECK(organ->proc.sparseEvery() == every, "the organ is loaded with the setting");
  return organ;
}

// A note held down, and then let go: the attack, then the release tail.
std::vector<float> playNote(LoadedOrgan& organ, int note, int attackFrames, int tailFrames) {
  juce::MidiBuffer on;
  on.addEvent(juce::MidiMessage::noteOn(1, note, 0.8f), 0);
  organ.block(on);
  std::vector<float> out = organ.render(attackFrames);
  juce::MidiBuffer off;
  off.addEvent(juce::MidiMessage::noteOff(1, note), 0);
  organ.block(off);
  const std::vector<float> tail = organ.render(tailFrames);
  out.insert(out.end(), tail.begin(), tail.end());
  return out;
}

}  // namespace

// A note that is not loaded plays its neighbour's samples, retuned to its own
// pitch. The same must hold whichever way the samples declare their pitch.
class SparsePitchTest final : public mp::test::Test {
public:
  SparsePitchTest() : Test("functional.sparse.pitch", Category::Functional) {}

  void run() override {
    int id = 451;
    for (const auto mode : {PitchMode::Exact, PitchMode::None, PitchMode::File}) {
      const char* what = mode == PitchMode::Exact  ? "the definition gives the exact pitch"
                         : mode == PitchMode::None ? "the definition applies no tuning"
                                                   : "the file's own note gives the pitch";
      TestOrgan organ("mp-sparse-pitch", mode, id++);
      KeptFile kept(settingsFor(organ.odf));
      auto loaded = loadSparse(organ.odf, 2, kept);
      const mp::Pipe* pipe = pipeForNote(loaded->proc.organModel(), 201, 37);
      MP_CHECK(pipe != nullptr && pipe->borrowedFromPipeId != 0,
               std::string("the note that is not loaded borrows a neighbour (") + what + ")");
      const std::vector<float> sound = playNote(*loaded, 37, 48000, 0);
      const double hz = measureHz(sound, 48000.0);
      MP_CHECK(std::fabs(cents(hz, noteHz(37))) < 1.0,
               std::string("the borrowed note sounds at its own pitch (") + what + "), got " +
                   std::to_string(hz) + " Hz");
      organ.root.deleteRecursively();
    }
  }
};
static SparsePitchTest g_sparsePitch;

// The release of a borrowed note is at the borrowed note's pitch too.
class SparseReleaseTest final : public mp::test::Test {
public:
  SparseReleaseTest() : Test("functional.sparse.release", Category::Functional) {}

  void run() override {
    TestOrgan organ("mp-sparse-release", PitchMode::None, 455);
    KeptFile kept(settingsFor(organ.odf));
    auto loaded = loadSparse(organ.odf, 2, kept);
    // Half a second of the note, then the tail.
    const std::vector<float> sound = playNote(*loaded, 37, 24000, 24000);
    const std::vector<float> tail(sound.end() - 12000, sound.end());
    const double hz = measureHz(tail, 48000.0);
    MP_CHECK(std::fabs(cents(hz, noteHz(37))) < 1.0,
             "the release tail is at the borrowed note's pitch, got " + std::to_string(hz) + " Hz");
    organ.root.deleteRecursively();
  }
};
static SparseReleaseTest g_sparseRelease;

// With a temperament of its own, a borrowed note sounds at the same pitch as
// a loaded one: the retuning is the same on both sides of the borrow.
class SparseTemperamentTest final : public mp::test::Test {
public:
  SparseTemperamentTest() : Test("functional.sparse.temperament", Category::Functional) {}

  void run() override {
    TestOrgan organ("mp-sparse-temperament", PitchMode::Exact, 456);
    KeptFile kept(settingsFor(organ.odf));
    MP_CHECK(mp::findTemperament("Vallotti") != nullptr, "Vallotti is in the library");

    double full = 0.0, sparse = 0.0;
    {
      LoadedOrgan loaded(organ.odf);
      loaded.proc.setTemperament("Vallotti", nullptr, false);
      const std::vector<float> sound = playNote(loaded, 37, 48000, 0);
      full = measureHz(sound, 48000.0);
    }
    {
      auto sparseOrgan = loadSparse(organ.odf, 2, kept);
      sparseOrgan->proc.setTemperament("Vallotti", nullptr, false);
      const std::vector<float> sound = playNote(*sparseOrgan, 37, 48000, 0);
      sparse = measureHz(sound, 48000.0);
    }
    MP_CHECK(full > 0.0 && sparse > 0.0, "both notes sounded");
    MP_CHECK(std::fabs(cents(sparse, full)) < 1.0,
             "a borrowed note sounds at the same pitch as a loaded one, got " +
                 std::to_string(sparse) + " Hz and " + std::to_string(full) + " Hz");
    organ.root.deleteRecursively();
  }
};
static SparseTemperamentTest g_sparseTemperament;

// A sparse load reads fewer samples: that is the whole of it.
class SparseMemoryTest final : public mp::test::Test {
public:
  SparseMemoryTest() : Test("functional.sparse.memory", Category::Functional) {}

  void run() override {
    TestOrgan organ("mp-sparse-memory", PitchMode::Exact, 457);
    KeptFile kept(settingsFor(organ.odf));
    int full = 0, sparse = 0;
    {
      LoadedOrgan loaded(organ.odf);
      full = static_cast<int>(loaded.proc.sampleLibrary().residentCount());
      MP_CHECK(full == 26, "every sample of the rank is read, got " + std::to_string(full));
    }
    {
      auto sparseOrgan = loadSparse(organ.odf, 2, kept);
      sparse = static_cast<int>(sparseOrgan->proc.sampleLibrary().residentCount());
    }
    MP_CHECK(sparse > 0 && sparse < full, "a sparse load reads fewer samples, got " +
                                              std::to_string(sparse));
    const double share = static_cast<double>(sparse) / static_cast<double>(full);
    MP_CHECK(share > 0.45 && share < 0.60,
             "the sparse load reads between 45% and 60% of the samples, got " +
                 std::to_string(share));
    organ.root.deleteRecursively();
  }
};
static SparseMemoryTest g_sparseMemory;

// The cache of a full load and the cache of a sparse load are different
// things, and neither is ever read as the other.
class SparseCacheTest final : public mp::test::Test {
public:
  SparseCacheTest() : Test("functional.sparse.cache", Category::Functional) {}

  void run() override {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("mp-sparse-cache");
    root.deleteRecursively();
    root.createDirectory();
    const auto cacheDir = root.getChildFile("cache");
    cacheDir.createDirectory();

    // A rank of thirteen pipes, each with a sample of its own.
    mp::OrganModel model;
    addRank(model, 201, 36, 48, 1001, 3001);
    for (int note = 36; note <= 48; ++note)
      writeTone(root.getChildFile(std::to_string(1001 + note - 36) + ".wav"), noteHz(note),
                4800, 0);
    // The sparse model: the pipes that are not kept play a neighbour's
    // samples, and their own are read by nobody.
    mp::OrganModel thinned = model;
    const auto plan = mp::planSparseLoading(thinned, 2, {}, kAllPresent);
    mp::applySparsePlan(thinned, plan);

    auto load = [&](const mp::OrganModel& m, int every) {
      mp::SampleLibrary lib;
      lib.setLoadThreads(1);
      lib.setCacheDir(cacheDir.getFullPathName().toStdString());
      lib.setCacheMode(mp::SampleLibrary::CacheMode::Single);
      lib.setCacheIdentity("sparse-cache", "stamp");
      lib.setSparseEvery(every);
      const auto report = lib.loadAll(m, root.getFullPathName().toStdString(), 0,
                                      mp::LoopSelection::Longest, nullptr);
      lib.finishCacheWrite(false);
      return std::make_pair(report.loaded, lib.cacheBytesRead());
    };

    {
      const auto [loaded, read] = load(model, 1);
      MP_CHECK(loaded == 13, "the whole rank is read");
      MP_CHECK(read == 0, "there is no cache to read yet");
    }
    {
      const auto [loaded, read] = load(thinned, 2);
      MP_CHECK(loaded == 7, "the sparse load reads seven samples, got " + std::to_string(loaded));
      MP_CHECK(read == 0, "a sparse load does not read the full cache");
    }
    {
      const auto [loaded, read] = load(model, 1);
      MP_CHECK(loaded == 13, "the whole rank is read again");
      MP_CHECK(read == 0, "a full load does not read the sparse cache");
    }
    {
      const auto [loaded, read] = load(model, 1);
      MP_CHECK(read > 0, "and the full cache is read the next time");
    }
    root.deleteRecursively();
  }
};
static SparseCacheTest g_sparseCache;

// The setting is saved with the organ, and another organ starts with every
// note.
class SparseSettingsTest final : public mp::test::Test {
public:
  SparseSettingsTest() : Test("functional.sparse.settings", Category::Functional) {}

  void run() override {
    TestOrgan organ("mp-sparse-settings", PitchMode::Exact, 458);
    KeptFile kept(settingsFor(organ.odf));
    {
      LoadedOrgan loaded(organ.odf);
      MP_CHECK(loaded.proc.sparseEvery() == 1, "a new organ loads every note");
      loaded.proc.setSparseEvery(2);
      MP_CHECK(loaded.proc.saveSettings(), "the organ's settings are written");
      MP_CHECK(kept.file.loadFileAsString().contains("sparse 2"),
               "the file says every second note");
    }
    {
      LoadedOrgan loaded(organ.odf);
      MP_CHECK(loaded.proc.sparseEvery() == 2, "and the organ comes back with it");
    }
    organ.root.deleteRecursively();
  }
};
static SparseSettingsTest g_sparseSettings;

#endif  // MP_TEST_HAS_AUDIO
