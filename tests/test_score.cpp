// The score panel: Verovio's engraving, the page as the panel reads it, the
// player that hands the notes to the organ, and the panel playing a score
// through a small organ.
//
// The player is checked to the sample: each note goes into the block it
// falls in, at its own offset, and a score drives the organ exactly as live
// MIDI at the same moments does, so the two renders are bit-identical.
#ifdef MP_TEST_HAS_SCORE

#include "MpTest.h"

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/ScorePlayer.h"
#include "../src/mp_score/ScoreEngraver.h"
#include "../src/mp_ui/ScorePanel.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>

using mp::test::Category;

namespace {

std::string readScoreFixture(const char* name) {
  std::ifstream f(std::string(MP_TEST_FIXTURES_DIR) + "/" + name, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

size_t countOf(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
  return n;
}

// The picture of the panel, for a look: MP_TEST_SCORE_PNG names a folder.
void savePicture(const juce::Image& image, const char* name) {
  const char* dir = std::getenv("MP_TEST_SCORE_PNG");
  if (dir == nullptr) return;
  const auto out = juce::File(dir).getChildFile(name);
  out.deleteFile();
  juce::FileOutputStream os(out);
  juce::PNGImageFormat().writeImageToStream(image, os);
}

// A small organ: a pedal (assignment code 1) and one manual (code 2), each
// with a stop of its own drawn, and a real sample for every pipe.
struct ScoreOrgan {
  juce::File root;
  juce::File odf;

  ScoreOrgan() {
    root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-score-organ");
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
    juce::String pipes, layers, attacks;
    // Every key of both keyboards, 36..96, on a rank of its own per keyboard.
    for (int rank = 0; rank < 2; ++rank)
      for (int n = 36; n <= 96; ++n) {
        const int pipe = 10000 + rank * 1000 + n;
        pipes << "<Pipe_SoundEngine01><PipeID>" << pipe << "</PipeID><RankID>" << (201 + rank)
              << "</RankID><NormalMIDINoteNumber>" << n << "</NormalMIDINoteNumber></Pipe_SoundEngine01>";
        layers << "<Pipe_SoundEngine01_Layer><LayerID>" << pipe << "</LayerID><PipeID>" << pipe
               << "</PipeID></Pipe_SoundEngine01_Layer>";
        attacks << "<Pipe_SoundEngine01_AttackSample><UniqueID>" << pipe << "</UniqueID><LayerID>" << pipe
                << "</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>";
      }
    odf = defs.getChildFile("score.Organ_Hauptwerk_xml");
    odf.replaceWithText(
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Score</Identification_Name>"
        "<Identification_UniqueOrganID>78</Identification_UniqueOrganID></_General></ObjectList>"
        "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
        "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
        "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
        "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
        "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
        "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>Subbass</Name></Rank>"
        "<Rank><RankID>202</RankID><Name>Principal</Name></Rank></ObjectList>"
        "<ObjectList ObjectType=\"Pipe_SoundEngine01\">" + pipes + "</ObjectList>"
        "<ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">" + layers + "</ObjectList>"
        "<ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">" + attacks + "</ObjectList>"
        "<ObjectList ObjectType=\"Keyboard\">"
        "<Keyboard><KeyboardID>701</KeyboardID><Name>Pedal</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
        "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
        "<Keyboard><KeyboardID>702</KeyboardID><Name>Great</Name><DefaultInputOutputKeyboardAsgnCode>2</DefaultInputOutputKeyboardAsgnCode>"
        "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
        "</ObjectList><ObjectList ObjectType=\"Division\">"
        "<Division><DivisionID>801</DivisionID><Name>Pedal</Name></Division>"
        "<Division><DivisionID>802</DivisionID><Name>Great</Name></Division></ObjectList>"
        "<ObjectList ObjectType=\"KeyAction\">"
        "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
        "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
        "<KeyAction><SourceKeyboardID>702</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>802</DestDivisionID>"
        "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
        "</ObjectList><ObjectList ObjectType=\"Stop\">"
        "<Stop><StopID>901</StopID><Name>Subbass 16</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
        "<Stop><StopID>902</StopID><Name>Principal 8</Name><DivisionID>802</DivisionID><ControllingSwitchID>1102</ControllingSwitchID></Stop>"
        "</ObjectList><ObjectList ObjectType=\"StopRank\">"
        "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
        "<NumberOfMappedDivisionInputNodes>61</NumberOfMappedDivisionInputNodes></StopRank>"
        "<StopRank><StopID>902</StopID><RankID>202</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
        "<NumberOfMappedDivisionInputNodes>61</NumberOfMappedDivisionInputNodes></StopRank>"
        "</ObjectList><ObjectList ObjectType=\"Switch\">"
        "<Switch><SwitchID>1101</SwitchID><Name>Subbass 16</Name><Latching>Y</Latching></Switch>"
        "<Switch><SwitchID>1102</SwitchID><Name>Principal 8</Name><Latching>Y</Latching></Switch>"
        "</ObjectList></Hauptwerk>");
  }

  ~ScoreOrgan() { root.deleteRecursively(); }
};

constexpr int kBlock = 256;

void runBlock(mp::MasterpieceProcessor& proc, juce::AudioBuffer<float>& out, juce::MidiBuffer& midi) {
  out.clear();
  proc.processBlock(out, midi);
}

void runBlock(mp::MasterpieceProcessor& proc) {
  juce::AudioBuffer<float> out(2, kBlock);
  juce::MidiBuffer none;
  runBlock(proc, out, none);
}

struct Played {
  int64_t at;
  int channel, note;
  bool on;
};

// Runs a player alone over `blocks` blocks, and lists what it emitted at
// which sample of the playing.
std::vector<Played> drive(mp::ScorePlayer& player, int blocks) {
  std::vector<Played> out;
  for (int b = 0; b < blocks; ++b) {
    juce::MidiBuffer midi;
    player.process(midi, kBlock);
    for (const auto meta : midi)
      if (meta.getMessage().isNoteOnOrOff())
        out.push_back({static_cast<int64_t>(b) * kBlock + meta.samplePosition, meta.getMessage().getChannel(),
                       meta.getMessage().getNoteNumber(), meta.getMessage().isNoteOn()});
  }
  return out;
}

juce::File unpackedFonts() {
  const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-score-fonts");
  if (!dir.getChildFile(".complete").existsAsFile()) mp::ui::unpackScoreFonts(dir);
  return dir;
}

}  // namespace

// The engraver takes MusicXML and gives back SVG: one page for two bars, with
// a drawn note for each of the six in the file. Input that is not a score
// fails with a reason and leaves the engraver usable.
class ScoreEngraverTest final : public mp::test::Test {
public:
  ScoreEngraverTest() : Test("functional.score.engraver", Category::Functional) {}

  void run() override {
    mp::ScoreEngraver eng(MP_TEST_SCORE_DATA_DIR);
    MP_CHECK(eng.ready(), "the font folder is found");
    MP_CHECK(eng.pageCount() == 0 && eng.renderPage(1).empty(), "nothing is loaded yet");

    const std::string xml = readScoreFixture("two-bars.musicxml");
    MP_CHECK(!xml.empty(), "the fixture is present");
    eng.setPage(800, 600, 40);
    const auto t0 = std::chrono::steady_clock::now();
    MP_CHECK(eng.load(xml), "the two-bar score loads: " + eng.error());
    MP_CHECK(eng.pageCount() == 1, "two bars fit one page");
    const std::string svg = eng.renderPage(1);
    std::printf("score engraving: load + page, %.1f ms\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    MP_CHECK(svg.rfind("<?xml", 0) == 0 && svg.find("<svg") != std::string::npos, "the page is an SVG document");
    MP_CHECK(countOf(svg, "class=\"note\"") == 6,
             "six notes are drawn, got " + std::to_string(countOf(svg, "class=\"note\"")));
    MP_CHECK(svg.find("width=\"800px\"") != std::string::npos, "the page is 800 px wide, as asked: " + svg.substr(0, 160));
    MP_CHECK(eng.renderPage(0).empty() && eng.renderPage(2).empty(), "pages outside the score give nothing");

    // A narrower page breaks the same bars differently but keeps every note.
    eng.setPage(200, 600, 40);
    MP_CHECK(eng.pageCount() >= 1, "the score is engraved again for a new size");
    size_t notes = 0;
    for (int p = 1; p <= eng.pageCount(); ++p) notes += countOf(eng.renderPage(p), "class=\"note\"");
    MP_CHECK(notes == 6, "no note is lost when the page narrows");
    // The second page's number sits between two en dashes, U+2013.
    {
      mp::ScoreEngraver trio(MP_TEST_SCORE_DATA_DIR);
      trio.setPage(300, 300, 100);
      MP_CHECK(trio.load(readScoreFixture("organ-trio.musicxml")) && trio.pageCount() >= 2,
               "the trio on a small page takes several pages, got " + std::to_string(trio.pageCount()));
      const std::string second = trio.renderPage(2);
      MP_CHECK(second.find("\xe2\x80\x93") != std::string::npos && second.find("\xc3\xa2") == std::string::npos,
               "the page number's dashes are en dashes");
    }

    MP_CHECK(!eng.midiBase64().empty(), "the score is given as MIDI");
    MP_CHECK(eng.timemapJson().find("tstamp") != std::string::npos, "and as a time map");

    MP_CHECK(!eng.load("this is not a score"), "text that is not XML is refused");
    MP_CHECK(!eng.error().empty(), "and says so");
    MP_CHECK(eng.pageCount() == 0 && eng.renderPage(1).empty(), "a refused file leaves no page behind");
    MP_CHECK(!eng.load(std::string()), "an empty file is refused");
    MP_CHECK(eng.load(xml), "a good score loads after the bad ones");

    mp::ScoreEngraver lost("/this/folder/does/not/exist");
    MP_CHECK(!lost.ready() && !lost.load(xml), "a missing font folder is reported, and nothing is drawn without fonts");
  }
};
static ScoreEngraverTest g_scoreEngraver;

// Two tempos on the same beat, as a score from a notation program can carry:
// a marking that says 120 and a plain tempo of 60 after it. The MIDI keeps
// the last and Verovio's own time map the first, so the cursor ran at twice
// the speed of the keys (#964). The time map and the cursor now keep to the
// MIDI, which is what the keys play.
class ScoreTwoTemposTest final : public mp::test::Test {
public:
  ScoreTwoTemposTest() : Test("functional.score.two-tempos", Category::Functional) {}

  void run() override {
    std::string notes;
    for (const char* step : {"C", "D", "E", "F"})
      notes += std::string("<note><pitch><step>") + step +
               "</step><octave>4</octave></pitch><duration>1</duration><type>quarter</type></note>";
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><score-partwise version=\"4.0\">"
        "<part-list><score-part id=\"P1\"><part-name>Organ</part-name></score-part></part-list>"
        "<part id=\"P1\"><measure number=\"1\"><attributes><divisions>1</divisions>"
        "<time><beats>4</beats><beat-type>4</beat-type></time><clef><sign>G</sign><line>2</line></clef>"
        "</attributes><direction placement=\"above\"><direction-type><words>Adagio</words></direction-type>"
        "<sound tempo=\"120\"/></direction><sound tempo=\"60\"/>" +
        notes + "</measure></part></score-partwise>";
    mp::ScoreEngraver eng(MP_TEST_SCORE_DATA_DIR);
    eng.setPage(800, 600, 40);
    MP_CHECK(eng.load(xml), "the score loads: " + eng.error());

    // When the MIDI plays each note, in seconds.
    std::vector<double> played;
    juce::MemoryOutputStream decoded;
    MP_CHECK(juce::Base64::convertFromBase64(decoded, juce::String(eng.midiBase64())), "the MIDI decodes");
    juce::MemoryInputStream in(decoded.getData(), decoded.getDataSize(), false);
    juce::MidiFile file;
    MP_CHECK(file.readFrom(in), "the MIDI reads");
    file.convertTimestampTicksToSeconds();
    for (int t = 0; t < file.getNumTracks(); ++t)
      for (const auto* ev : *file.getTrack(t))
        if (ev->message.isNoteOn()) played.push_back(ev->message.getTimeStamp());
    std::sort(played.begin(), played.end());
    MP_CHECK(played.size() == 4 && std::fabs(played[1] - 1.0) < 0.01,
             "the keys play at 60 a minute: the second note at " +
                 std::to_string(played.size() > 1 ? played[1] : -1.0) + " s");

    // The time map, where each note starts.
    std::vector<std::pair<double, std::string>> starts;
    const auto map = juce::JSON::parse(juce::String(eng.timemapJson()));
    if (const auto* entries = map.getArray())
      for (const auto& entry : *entries)
        if (const auto* on = entry["on"].getArray())
          for (const auto& id : *on) starts.emplace_back(static_cast<double>(entry["tstamp"]), id.toString().toStdString());
    MP_CHECK(starts.size() == 4, "four notes in the time map, got " + std::to_string(starts.size()));
    if (starts.size() != 4) return;
    for (size_t i = 0; i < 4; ++i)
      MP_CHECK(std::fabs(starts[i].first - played[i] * 1000.0) < 2.0,
               "note " + std::to_string(i + 1) + " starts in the time map when the MIDI plays it: " +
                   std::to_string(starts[i].first) + " ms, played at " + std::to_string(played[i] * 1000.0));

    // The cursor half-way through the third note shows the third note.
    const auto at = juce::JSON::parse(juce::String(eng.elementsAtTime(2500)));
    bool third = false;
    if (const auto* ids = at["notes"].getArray())
      for (const auto& id : *ids) third = third || id.toString().toStdString() == starts[2].second;
    MP_CHECK(third, "at 2.5 s the cursor is on the third note: " + eng.elementsAtTime(2500));
  }
};
static ScoreTwoTemposTest g_scoreTwoTempos;

// The player alone: idle it adds nothing; each note lands at its own sample;
// a key held by two staves at once sounds once and is let go by the last;
// Stop and the end of the score let go of every key held.
class ScorePlayerTest final : public mp::test::Test {
public:
  ScorePlayerTest() : Test("functional.score.player", Category::Functional) {}

  void run() override {
    mp::ScorePlayer player;
    player.prepare(48000.0);

    juce::MidiBuffer live;
    live.addEvent(juce::MidiMessage::noteOn(3, 64, static_cast<juce::uint8>(90)), 17);
    player.process(live, kBlock);
    MP_CHECK(live.getNumEvents() == 1, "idle, the block's own MIDI is left as it is");
    MP_CHECK(!player.isPlaying(), "and nothing plays");

    using E = mp::ScorePlayer::Event;
    std::vector<E> events = {
        {0.010, 2, 60, true},  {0.020, 2, 60, false},  // 480 to 960
        {0.020, 2, 60, true},  {0.030, 2, 60, false},  // the same key again at once
        {0.015, 1, 48, true},  {0.016, 1, 48, true},   // two staves on one key
        {0.025, 1, 48, false}, {0.040, 1, 48, false},
    };
    mp::ScorePlayer::orderEvents(events);
    player.play(events, 0.0);
    MP_CHECK(player.isPlaying(), "playing");
    const auto got = drive(player, 12);
    auto find = [&](int64_t at, int ch, int note, bool on) {
      for (const auto& p : got)
        if (p.at == at && p.channel == ch && p.note == note && p.on == on) return true;
      return false;
    };
    MP_CHECK(find(480, 2, 60, true), "the first note starts at sample 480");
    MP_CHECK(find(960, 2, 60, false) && find(960, 2, 60, true), "and is played again at 960, let go first");
    MP_CHECK(find(1440, 2, 60, false), "and let go at 1440");
    MP_CHECK(find(720, 1, 48, true), "the shared key goes down once, at 720");
    MP_CHECK(find(1920, 1, 48, false), "and comes up with the last staff to let go, at 1920");
    int shared = 0;
    for (const auto& p : got)
      if (p.note == 48) ++shared;
    MP_CHECK(shared == 2, "the shared key sends one note-on and one note-off, got " + std::to_string(shared));
    MP_CHECK(!player.isPlaying(), "the end of the list ends the playing");

    // Stop with keys held: they come up in the next block.
    player.play({{0.0, 4, 70, true}, {5.0, 4, 70, false}}, 0.0);
    auto held = drive(player, 2);
    MP_CHECK(held.size() == 1 && held[0].on, "the long note is down");
    player.stop();
    held = drive(player, 1);
    MP_CHECK(held.size() == 1 && !held[0].on && held[0].note == 70, "Stop lets go of it");
    MP_CHECK(drive(player, 2).empty(), "and nothing follows");

    // A final chord the file never lets go of is let go at the end.
    player.play({{0.0, 5, 72, true}, {0.001, 5, 76, true}}, 0.0);
    held = drive(player, 1);
    MP_CHECK(held.size() == 4, "both notes go down and come up at the end, got " + std::to_string(held.size()));

    // From the middle: what lies before the start is not played. The playing
    // begins at sample 864 (18 ms), so the note at 960 lands 96 samples in.
    player.play(events, 0.018);
    const auto late = drive(player, 12);
    MP_CHECK(!late.empty() && late[0].on && late[0].note == 60 && late[0].at == 96,
             "a start at 18 ms plays first the note at 20 ms, 96 samples in");
    int ons = 0;
    for (const auto& p : late)
      if (p.on) ++ons;
    MP_CHECK(ons == 1, "and nothing from before it, got " + std::to_string(ons) + " note-ons");
  }
};
static ScorePlayerTest g_scorePlayer;

// The page as the panel reads it: every engraved note found with a place on
// the page and a system, the systems with their staff lines, and the SVG
// drawable by JUCE.
class ScoreGeometryTest final : public mp::test::Test {
public:
  ScoreGeometryTest() : Test("functional.score.geometry", Category::Functional) {}

  void run() override {
    juce::ScopedJuceInitialiser_GUI gui;
    mp::ScoreEngraver eng(MP_TEST_SCORE_DATA_DIR);
    eng.setPage(900, 600, 70);
    MP_CHECK(eng.load(readScoreFixture("organ-trio.musicxml")), "the trio loads: " + eng.error());
    MP_CHECK(eng.pageCount() >= 1, "it has a page");
    const std::string svg = eng.renderPage(1);
    mp::ui::ScorePageGeometry geo;
    const auto drawable = mp::ui::parseScorePage(svg, geo);
    MP_CHECK(drawable != nullptr, "JUCE reads the page");
    MP_CHECK(std::abs(geo.width - 900.0f) < 1.0f && std::abs(geo.height - 600.0f) < 1.0f,
             "the page is 900 by 600, got " + std::to_string(geo.width) + " by " + std::to_string(geo.height));
    const size_t drawn = countOf(svg, "class=\"note\"");
    MP_CHECK(drawn > 0 && geo.notes.size() == drawn,
             "every note drawn is found: " + std::to_string(geo.notes.size()) + " of " + std::to_string(drawn));
    MP_CHECK(!geo.systems.empty(), "the systems are found");
    for (const auto& s : geo.systems) {
      MP_CHECK(s.lineGap > 2.0f && s.lineGap < 40.0f, "a staff space is a few pixels, got " + std::to_string(s.lineGap));
      MP_CHECK(s.area.getWidth() > 400.0f, "a system spans the page");
    }
    for (const auto& [id, n] : geo.notes) {
      MP_CHECK(n.system >= 0 && static_cast<size_t>(n.system) < geo.systems.size(), "note " + id + " is in a system");
      MP_CHECK(n.head.x > 0 && n.head.x < geo.width && n.head.y > 0 && n.head.y < geo.height,
               "note " + id + " is on the page");
      const auto& area = geo.systems[static_cast<size_t>(n.system)].area;
      const float gap = geo.systems[static_cast<size_t>(n.system)].lineGap;
      MP_CHECK(n.head.y > area.getY() - 6 * gap && n.head.y < area.getBottom() + 6 * gap,
               "note " + id + " is by its own system");
    }
  }
};
static ScoreGeometryTest g_scoreGeometry;

// The first page opens with the title, and only the first. A metronome mark's
// note is text in Bravura and the words are in Liberation Serif: the program
// carries both, JUCE finds them by name, and the page is drawn with them.
class ScoreTitleAndTempoTest final : public mp::test::Test {
public:
  ScoreTitleAndTempoTest() : Test("functional.score.title-and-tempo", Category::Functional) {}

  void run() override {
    juce::ScopedJuceInitialiser_GUI gui;
    mp::ScoreEngraver eng(MP_TEST_SCORE_DATA_DIR);
    eng.setPage(900, 600, 70);
    MP_CHECK(eng.load(readScoreFixture("titled-tempo.musicxml")), "the score loads: " + eng.error());
    const std::string first = eng.renderPage(1);
    MP_CHECK(first.find("class=\"pgHead") != std::string::npos, "the first page has a heading");
    MP_CHECK(first.find("Short Prelude") != std::string::npos, "with the title in it");
    const std::string quarter = "\xee\xb2\xa5";  // U+ECA5, metNoteQuarterUp
    MP_CHECK(first.find("font-family=\"Bravura\"") != std::string::npos && first.find(quarter) != std::string::npos,
             "the metronome mark's note is text in Bravura");
    MP_CHECK(first.find("@font-face") == std::string::npos, "and the page carries no copy of the font");

    // The later pages carry the page number alone.
    eng.setPage(300, 120, 70);
    MP_CHECK(eng.pageCount() >= 2, "a small page breaks the score in two, got " + std::to_string(eng.pageCount()));
    MP_CHECK(eng.renderPage(2).find("Short Prelude") == std::string::npos, "the second page has no title");

    MP_CHECK(mp::ui::registerScoreFonts(), "the program's fonts load");
    const juce::Font font(juce::FontOptions("Bravura", 20.0f, juce::Font::plain));
    const auto face = font.getTypefacePtr();
    MP_CHECK(face != nullptr && face->getName() == "Bravura",
             "JUCE finds Bravura by name, got " + (face != nullptr ? face->getName() : juce::String("nothing")).toStdString());
    MP_CHECK(face != nullptr && face->getNominalGlyphForCodepoint(0xECA5).value_or(0) != 0, "with the quarter note in it");
    for (const int style : {juce::Font::plain, juce::Font::bold, juce::Font::italic}) {
      const auto serif = juce::Font(juce::FontOptions(mp::ui::kScoreTextFont, 20.0f, style)).getTypefacePtr();
      MP_CHECK(serif != nullptr && serif->getName() == mp::ui::kScoreTextFont,
               "JUCE finds the serif by name in style " + std::to_string(style));
    }

    // And the page as the panel draws it: the title in the serif, the
    // metronome mark's note in Bravura.
    eng.setPage(900, 600, 70);
    mp::ui::ScorePageGeometry geo;
    const auto page = mp::ui::parseScorePage(eng.renderPage(1), geo);
    MP_CHECK(page != nullptr, "JUCE reads the page");
    juce::String titleFont, noteFont;
    std::function<void(const juce::Drawable&)> walk = [&](const juce::Drawable& d) {
      if (auto* t = dynamic_cast<const juce::DrawableText*>(&d)) {
        if (t->getText().contains("Short Prelude")) titleFont = t->getFont().getTypefaceName();
        if (t->getText().containsChar(static_cast<juce::juce_wchar>(0xECA5))) noteFont = t->getFont().getTypefaceName();
      }
      if (auto* g = dynamic_cast<const juce::DrawableComposite*>(&d))
        for (int i = 0; i < g->getNumChildren(); ++i) walk(g->getChild(i));
    };
    if (page != nullptr) walk(*page);
    MP_CHECK(titleFont == mp::ui::kScoreTextFont, "the title is drawn in the serif, got " + titleFont.toStdString());
    MP_CHECK(noteFont == "Bravura", "the note is drawn in Bravura, got " + noteFont.toStdString());
  }
};
static ScoreTitleAndTempoTest g_scoreTitleAndTempo;

// The panel with an organ: the staves get their manuals, playing puts the
// keys down on those manuals with a cursor on the page, the pages turn by
// themselves, and the panel's other buttons do what they say.
class ScorePanelTest final : public mp::test::Test {
public:
  ScorePanelTest() : Test("functional.score.panel", Category::Functional) {}

  void run() override {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto fonts = unpackedFonts();
    MP_CHECK(fonts.getChildFile("Bravura.xml").existsAsFile() && fonts.getChildFile("text").isDirectory(),
             "the built-in fonts unpack");

    ScoreOrgan organ;
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, kBlock);
    MP_CHECK(proc.loadOrgan(organ.odf, 0, false).ok, "the organ loads");
    proc.setStopEngaged(901, true);
    proc.setStopEngaged(902, true);
    runBlock(proc);

    mp::ui::ScorePanel panel(proc, fonts);
    panel.setSize(1000, 700);
    const std::string xml = readScoreFixture("organ-trio.musicxml");
    MP_CHECK(panel.openData(juce::MemoryBlock(xml.data(), xml.size()), "organ-trio.musicxml"), "the trio opens");
    MP_CHECK(panel.staves().size() == 3, "three staves, got " + std::to_string(panel.staves().size()));
    MP_CHECK(panel.keyboardForStaff(3) == 701, "the last of three staves plays the pedal");
    // The pedal here is as wide as the manual, so the organ's preferred
    // keyboard may be the pedal: the hands' staves still go to a manual.
    MP_CHECK(panel.keyboardForStaff(1) == 702 && panel.keyboardForStaff(2) == 702,
             "the others play the manual, got " + std::to_string(panel.keyboardForStaff(1)) + " and " +
                 std::to_string(panel.keyboardForStaff(2)));

    auto page = panel.render();
    savePicture(page, "panel-trio.png");
    int dark = 0;
    for (int y = 60; y < page.getHeight() - 10; ++y)
      for (int x = 20; x < page.getWidth() - 20; ++x)
        if (page.getPixelAt(x, y).getBrightness() < 0.3f) ++dark;
    MP_CHECK(dark > 2000, "the score is drawn, dark pixels: " + std::to_string(dark));

    // Play: the first notes go down on both keyboards. Bar 1 opens with C5
    // over G3 over C3; the left hand's C4/E4 follows on beat 3.
    panel.play();
    for (int b = 0; b < 4; ++b) runBlock(proc);
    MP_CHECK(proc.keyDownOnKeyboard(702, 72), "the right hand's C5 is down on the Great");
    MP_CHECK(proc.keyDownOnKeyboard(702, 55) && !proc.keyDownOnKeyboard(702, 60), "and the left hand's G3");
    MP_CHECK(proc.keyDownOnKeyboard(701, 48), "the pedal's C3 is down on the pedal");
    MP_CHECK(!proc.keyDownOnKeyboard(701, 72), "and the right hand is not on the pedal");
    page = panel.render();
    savePicture(page, "panel-trio-playing.png");
    MP_CHECK(!panel.cursorNotes().empty(), "the cursor marks the notes that sound");
    int blue = 0;
    for (int y = 0; y < page.getHeight(); ++y)
      for (int x = 0; x < page.getWidth(); ++x) {
        const auto c = page.getPixelAt(x, y);
        if (c.getBlue() > c.getRed() + 40) ++blue;
      }
    MP_CHECK(blue > 200, "and is drawn, blue pixels: " + std::to_string(blue));
    // Each ring sits on its notehead: ink close round the point the cursor
    // marks (a half note's head is hollow, so its outline is what is found).
    for (const auto& id : panel.cursorNotes()) {
      const auto at = panel.notePosition(id).toInt();
      bool ink = false;
      for (int y = at.y - 4; y <= at.y + 4 && !ink; ++y)
        for (int x = at.x - 7; x <= at.x + 7 && !ink; ++x)
          if (page.getBounds().contains(x, y) && page.getPixelAt(x, y).getBrightness() < 0.35f) ink = true;
      MP_CHECK(ink, "the cursor's ring for " + id + " sits on its notehead, at " + at.toString().toStdString());
    }

    // Half a second on, the right hand's second quarter: D5.
    for (int b = 0; b < 48000 / 2 / kBlock + 4; ++b) runBlock(proc);
    MP_CHECK(proc.keyDownOnKeyboard(702, 74) && !proc.keyDownOnKeyboard(702, 72), "the melody moves on to D5");
    // The band stands on the beat: at D5, struck last, and away from the
    // pedal's whole note and the G3, held from beat 1.
    {
      page = panel.render();
      float first = 1.0e9f, last = -1.0e9f;
      for (const auto& id : panel.cursorNotes()) {
        first = std::min(first, panel.notePosition(id).x);
        last = std::max(last, panel.notePosition(id).x);
      }
      MP_CHECK(last - first > 20.0f, "the notes sounding span two beats, " + std::to_string(first) + " to " +
                                         std::to_string(last));
      const auto tinted = [&page](int x) {
        int n = 0;
        for (int y = 0; y < page.getHeight(); ++y) {
          const auto c = page.getPixelAt(x, y);
          if (c.getBlue() > c.getRed() + 25) ++n;
        }
        return n;
      };
      const int atLast = tinted(juce::roundToInt(last)), atFirst = tinted(juce::roundToInt(first));
      MP_CHECK(atLast > 100 && atFirst < 40, "the band is at the newest note: " + std::to_string(atLast) +
                                                 " tinted pixels there, " + std::to_string(atFirst) + " at the held ones");
    }
    // Another half second: beat 3, the left hand's chord in place of the G3.
    for (int b = 0; b < 48000 / 2 / kBlock; ++b) runBlock(proc);
    MP_CHECK(proc.keyDownOnKeyboard(702, 60) && proc.keyDownOnKeyboard(702, 64) && !proc.keyDownOnKeyboard(702, 55),
             "on beat 3 the left hand's C4/E4 sounds and the G3 is let go");

    panel.stop();
    runBlock(proc);
    MP_CHECK(!proc.keyDownOnKeyboard(702, 76) && !proc.keyDownOnKeyboard(702, 60) && !proc.keyDownOnKeyboard(701, 48),
             "Stop lets go of every key");
    MP_CHECK(panel.startSeconds() > 0.9, "and the next Play starts where it stopped");

    // No manual for the pedal staff: it is left out.
    panel.setKeyboardForStaff(3, 0);
    panel.setStart(0);
    panel.play();
    for (int b = 0; b < 4; ++b) runBlock(proc);
    MP_CHECK(proc.keyDownOnKeyboard(702, 72) && !proc.keyDownOnKeyboard(701, 48), "a staff given no manual is not played");
    panel.stop();
    runBlock(proc);
    panel.setKeyboardForStaff(3, 701);

    // Larger notation on a small panel: more pages, turned by the playing.
    panel.setSize(700, 420);
    panel.setZoom(200);
    panel.render();
    const int pages = panel.pageCount();
    MP_CHECK(pages >= 3, "at 200% the trio takes several pages, got " + std::to_string(pages));
    MP_CHECK(panel.zoom() == 200, "and the zoom says so");
    panel.showPage(1);
    panel.setStart(0);
    panel.play();
    int furthest = 1;
    for (int b = 0; b < 48000 * 17 / kBlock && panel.playing(); ++b) {
      runBlock(proc);
      if (b % 40 == 0) {
        panel.render();
        furthest = std::max(furthest, panel.page());
      }
    }
    MP_CHECK(furthest == pages, "the pages turn to the last as it plays, reached " + std::to_string(furthest) +
                                    " of " + std::to_string(pages));
    runBlock(proc);
    panel.render();
    MP_CHECK(!panel.playing(), "and the playing ends with the score");
    MP_CHECK(!proc.keyDownOnKeyboard(701, 43) && !proc.keyDownOnKeyboard(702, 60), "with every key let go");

    // Two pages side by side.
    panel.setSize(1200, 500);
    panel.setTwoPages(true);
    panel.render();
    panel.showPage(1);
    MP_CHECK(panel.geometryOf(1) != nullptr && panel.geometryOf(2) != nullptr, "two pages show side by side");
    page = panel.render();
    savePicture(page, "panel-trio-two-pages.png");
    // At 200% on a short panel the zoom stops where a whole system still fits.
    for (const int shown : {1, 2}) {
      const auto* geo = panel.geometryOf(shown);
      MP_CHECK(geo != nullptr && !geo->systems.empty(), "page " + std::to_string(shown) + " has systems");
      if (geo == nullptr) continue;
      for (const auto& sys : geo->systems)
        MP_CHECK(sys.area.getBottom() <= geo->height && sys.area.getRight() <= geo->width,
                 "every system lies on its page, bottom " + std::to_string(sys.area.getBottom()) + " of " +
                     std::to_string(geo->height));
    }
    MP_CHECK(panel.drawnZoom() < panel.zoom() && panel.drawnZoom() >= 50,
             "and the zoom shown is the one drawn, " + std::to_string(panel.drawnZoom()) + "% of " +
                 std::to_string(panel.zoom()) + "% asked");
    // Both pages at full strength: the paper of the right one is white too.
    int white = 0, seen = 0;
    for (int y = 60; y < page.getHeight() - 10; y += 3)
      for (int x = page.getWidth() / 2 + 20; x < page.getWidth() - 20; x += 3, ++seen)
        if (page.getPixelAt(x, y).getBrightness() > 0.95f) ++white;
    MP_CHECK(white * 2 > seen, "the right page is drawn as brightly as the left, white " + std::to_string(white) +
                                   " of " + std::to_string(seen));
    panel.showPage(panel.page() + 2);
    MP_CHECK(panel.page() == 3, "the next turn shows the next two");
    panel.setTwoPages(false);

    // Inverted: the paper is dark.
    panel.setZoom(100);
    panel.setInverted(true);
    page = panel.render();
    savePicture(page, "panel-trio-inverted.png");
    MP_CHECK(page.getPixelAt(page.getWidth() / 2, page.getHeight() - 20).getBrightness() < 0.2f, "inverted, the paper is dark");
    panel.setInverted(false);
    page = panel.render();
    MP_CHECK(page.getPixelAt(page.getWidth() / 2, page.getHeight() - 20).getBrightness() > 0.9f, "and white again");

    // Compressed MusicXML: the score named by META-INF/container.xml.
    juce::MemoryOutputStream zipped;
    {
      juce::ZipFile::Builder builder;
      const char* container =
          "<?xml version=\"1.0\" encoding=\"UTF-8\"?><container><rootfiles>"
          "<rootfile full-path=\"music/trio.musicxml\" media-type=\"application/vnd.recordare.musicxml+xml\"/>"
          "</rootfiles></container>";
      builder.addEntry(new juce::MemoryInputStream(container, std::strlen(container), true), 9, "META-INF/container.xml",
                       juce::Time::getCurrentTime());
      builder.addEntry(new juce::MemoryInputStream(xml.data(), xml.size(), true), 9, "music/trio.musicxml",
                       juce::Time::getCurrentTime());
      builder.writeToStream(zipped, nullptr);
    }
    MP_CHECK(panel.openData(zipped.getMemoryBlock(), "trio.mxl"), "a compressed score opens");
    MP_CHECK(panel.staves().size() == 3, "with its three staves");

    MP_CHECK(!panel.openData(juce::MemoryBlock("not a score", 11), "junk.xml"), "a file that is not a score is refused");
    MP_CHECK(panel.pageCount() == 0 && !panel.loaded(), "and shows no page");
  }
};
static ScorePanelTest g_scorePanel;

// The cursor stays in sight when the lines break again: after two pages, a
// zoom or a new size, the page shown holds the notes the cursor marks. A
// start inside the last notes of a page, held while the next page's first
// note waits, showed the next page and left the cursor behind.
class ScoreCursorPageTest final : public mp::test::Test {
public:
  ScoreCursorPageTest() : Test("functional.score.cursor-page", Category::Functional) {}

  void run() override {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto fonts = unpackedFonts();
    ScoreOrgan organ;
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, kBlock);
    MP_CHECK(proc.loadOrgan(organ.odf, 0, false).ok, "the organ loads");
    mp::ui::ScorePanel panel(proc, fonts);
    // Sixteen bars of one whole note, a second each: every moment but a
    // bar's first is inside a held note, the last bars of a page included.
    const std::string xml = readScoreFixture("whole-notes.musicxml");
    MP_CHECK(panel.openData(juce::MemoryBlock(xml.data(), xml.size()), "whole-notes.musicxml"), "the score opens");
    // Small pages, so the score runs over several of them.
    panel.setSize(700, 420);
    panel.setZoom(200);
    panel.render();
    MP_CHECK(panel.pageCount() >= 4, "the score over several pages, got " + std::to_string(panel.pageCount()));

    int checked = 0, lost = 0;
    std::string first;
    for (int quarters = 1; quarters < 64; ++quarters) {
      const double seconds = quarters / 4.0;
      panel.setStart(seconds);
      if (panel.cursorNotes().empty()) continue;
      for (const bool two : {false, true}) {
        panel.setTwoPages(two);
        ++checked;
        bool seen = false;
        for (const int shown : {panel.page(), panel.page() + 1}) {
          const auto* geo = panel.geometryOf(shown);
          if (geo == nullptr) continue;
          for (const auto& id : panel.cursorNotes())
            if (geo->notes.count(id) != 0) seen = true;
        }
        if (!seen && ++lost == 1)
          first = std::to_string(seconds) + " s, " + (two ? "two pages" : "one page") + ", page " +
                  std::to_string(panel.page());
      }
    }
    MP_CHECK(checked > 100, "starts checked: " + std::to_string(checked));
    MP_CHECK(lost == 0, "the cursor is on a page shown, lost " + std::to_string(lost) + " times, first at " + first);
    panel.setTwoPages(false);
  }
};
static ScoreCursorPageTest g_scoreCursorPage;

// A score drives the organ exactly as live MIDI does: the same notes at the
// same samples, from the player or from the block's input, give the same
// output to the bit. And with the player idle the output is the organ's
// alone.
class ScoreSameAsLiveTest final : public mp::test::Test {
public:
  ScoreSameAsLiveTest() : Test("functional.score.same-as-live", Category::Functional) {}

  void run() override {
    ScoreOrgan organ;
    auto make = [&](mp::MasterpieceProcessor& p) {
      p.prepareToPlay(48000.0, kBlock);
      MP_CHECK(p.loadOrgan(organ.odf, 0, false).ok, "the organ loads");
      p.setStopEngaged(901, true);
      p.setStopEngaged(902, true);
      runBlock(p);
    };
    mp::MasterpieceProcessor live, scored;
    make(live);
    make(scored);
    const int greatChannel = scored.clickChannelForKeyboard(702);
    const int pedalChannel = scored.clickChannelForKeyboard(701);
    MP_CHECK(greatChannel >= 1 && pedalChannel >= 1, "both keyboards can be reached");

    struct Note {
      int64_t on, off;
      int channel, note;
    };
    const std::vector<Note> notes = {{300, 9000, greatChannel, 60}, {300, 12000, pedalChannel, 48},
                                     {4100, 7000, greatChannel, 64}, {9000, 15000, greatChannel, 62}};
    std::vector<mp::ScorePlayer::Event> events;
    for (const auto& n : notes) {
      events.push_back({n.on / 48000.0, static_cast<uint8_t>(n.channel), static_cast<uint8_t>(n.note), true});
      events.push_back({n.off / 48000.0, static_cast<uint8_t>(n.channel), static_cast<uint8_t>(n.note), false});
    }
    mp::ScorePlayer::orderEvents(events);
    scored.scorePlayer().play(events, 0.0);

    juce::AudioBuffer<float> a(2, kBlock), b(2, kBlock);
    bool same = true;
    double energy = 0;
    for (int blk = 0; blk < 100; ++blk) {
      juce::MidiBuffer in, none;
      const int64_t start = static_cast<int64_t>(blk) * kBlock;
      // The player's arithmetic: a note at t seconds lands on sample t * rate.
      for (const auto& e : events) {
        const auto at = static_cast<int64_t>(e.seconds * 48000.0);
        if (at < start || at >= start + kBlock) continue;
        in.addEvent(e.on ? juce::MidiMessage::noteOn(e.channel, e.note, static_cast<juce::uint8>(100))
                         : juce::MidiMessage::noteOff(e.channel, e.note),
                    static_cast<int>(at - start));
      }
      runBlock(live, a, in);
      runBlock(scored, b, none);
      for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < kBlock; ++i) {
          if (a.getSample(ch, i) != b.getSample(ch, i)) same = false;
          energy += std::abs(a.getSample(ch, i));
        }
    }
    MP_CHECK(energy > 1.0, "the notes sound");
    MP_CHECK(same, "the score's render is bit-identical to the live one");
    MP_CHECK(!scored.scorePlayer().isPlaying(), "and the score has ended");
  }
};
static ScoreSameAsLiveTest g_scoreSameAsLive;

// The marks in the file: read out of the score, left out of what is
// engraved, and written back each before its note. A score with no marks is
// saved as it was read, to the byte.
class ScoreMarksFileTest final : public mp::test::Test {
public:
  ScoreMarksFileTest() : Test("functional.score.marks-file", Category::Functional) {}

  void run() override {
    const std::string plain = readScoreFixture("two-bars.musicxml");
    mp::ScoreMarks marks;
    MP_CHECK(marks.load(plain), "the score reads");
    MP_CHECK(marks.notes().size() == 6, "six notes, got " + std::to_string(marks.notes().size()));
    MP_CHECK(marks.marks.empty(), "and no marks");
    MP_CHECK(marks.save() == plain, "saved with no marks, the file is as it was");
    MP_CHECK(countOf(marks.engraving(), "id=\"mpn") == 6, "each note has an id for the engraver");
    MP_CHECK(marks.engraving().find("id=\"mpn") != std::string::npos && plain.find("mpn") == std::string::npos,
             "which the file does not get");

    // A note on the D, over two lines, and a registration on bar 2's G.
    marks.marks.push_back({mp::ScoreMark::Kind::Note, "mpn2", "Slower\nhere & <softly>", {}});
    marks.marks.push_back({mp::ScoreMark::Kind::Registration, "mpn5", "Full", "org1\t{\"drawn\":[\"s901\"]}"});
    const std::string saved = marks.save();
    MP_CHECK(saved.find("<other-direction print-object=\"no\">masterpiece-note\nSlower\nhere &amp; &lt;softly&gt;") !=
                 std::string::npos,
             "the note is written as a direction other programs keep");
    MP_CHECK(saved.find("masterpiece-note") < saved.find("<step>D</step>") &&
                 saved.find("masterpiece-note") > saved.find("<step>C</step>"),
             "just before its note");
    MP_CHECK(saved.find("masterpiece-registration\nFull\norg1\t") < saved.find("<step>G</step>") &&
                 saved.find("masterpiece-registration") > saved.find("<measure number=\"2\">"),
             "and the registration before bar 2's G");

    mp::ScoreMarks again;
    MP_CHECK(again.load(saved), "the saved score reads");
    MP_CHECK(again.marks.size() == 2, "with its two marks, got " + std::to_string(again.marks.size()));
    if (again.marks.size() == 2) {
      MP_CHECK(again.marks[0].kind == mp::ScoreMark::Kind::Note && again.marks[0].note == "mpn2" &&
                   again.marks[0].text == "Slower\nhere & <softly>",
               "the note, on its note, its text whole: " + again.marks[0].note + " " + again.marks[0].text);
      MP_CHECK(again.marks[1].kind == mp::ScoreMark::Kind::Registration && again.marks[1].note == "mpn5" &&
                   again.marks[1].text == "Full" && again.marks[1].states == "org1\t{\"drawn\":[\"s901\"]}",
               "the registration, its name and its states");
    }
    MP_CHECK(again.engraving().find("masterpiece-") == std::string::npos, "the engraver never sees a mark");
    MP_CHECK(again.notes() == marks.notes(), "the notes keep their ids");
    MP_CHECK(again.save() == saved, "saved again, the same file");
    again.marks.clear();
    MP_CHECK(again.save() == plain, "and with its marks deleted, the file it started as");

    // A chord's notes start together and stand together: a mark on its upper
    // note goes before the chord.
    std::string chord = plain;
    const std::string e4 = "<note><pitch><step>E</step>";
    chord.replace(chord.find(e4), 0,
                  "<note><pitch><step>G</step><octave>4</octave></pitch><duration>1</duration><type>quarter</type></note>\n      ");
    const std::string after = "<note><pitch><step>E</step><octave>4</octave></pitch><duration>1</duration><type>quarter</type></note>";
    chord.replace(chord.find(after) + after.size(), 0,
                  "<note><chord/><pitch><step>B</step><octave>4</octave></pitch><duration>1</duration><type>quarter</type></note>");
    mp::ScoreMarks chords;
    MP_CHECK(chords.load(chord) && chords.notes().size() == 8, "the score with a chord reads");
    chords.marks.push_back({mp::ScoreMark::Kind::Note, "mpn5", "chord", {}});  // the B over E
    const auto withChord = chords.save();
    MP_CHECK(withChord.find("masterpiece-note") < withChord.find("<step>E</step>"), "the mark goes before the chord");
    mp::ScoreMarks chordAgain;
    MP_CHECK(chordAgain.load(withChord) && chordAgain.marks.size() == 1 && chordAgain.marks[0].note == "mpn4",
             "and belongs to the chord's first note when read again");

    mp::ScoreMarks junk;
    MP_CHECK(!junk.load("not xml <") && junk.engraving() == "not xml <", "text that is not XML is passed on as it is");
  }
};
static ScoreMarksFileTest g_scoreMarksFile;

// The marks in the panel: a registration sets the organ as the playing
// reaches it, the buttons step through them, the icons are drawn on their
// notes, and every change is in the file and can be taken back.
class ScorePanelMarksTest final : public mp::test::Test {
public:
  ScorePanelMarksTest() : Test("functional.score.panel-marks", Category::Functional) {}

  void run() override {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto fonts = unpackedFonts();
    ScoreOrgan organ;
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, kBlock);
    MP_CHECK(proc.loadOrgan(organ.odf, 0, false).ok, "the organ loads");

    const auto file = organ.root.getChildFile("two-bars.musicxml");
    const std::string xml = readScoreFixture("two-bars.musicxml");
    file.replaceWithData(xml.data(), xml.size());

    auto draw = [&](bool subbass, bool principal) {
      proc.setStopEngaged(901, subbass);
      proc.setStopEngaged(902, principal);
      runBlock(proc);
    };
    auto drawn = [&] { return std::make_pair(proc.stopEngaged(901), proc.stopEngaged(902)); };
    {
      mp::ui::ScorePanel panel(proc, fonts);
      panel.setSize(1000, 700);
      MP_CHECK(panel.openFile(file), "the score opens from its file");
      panel.render();

      // The Principal for bar 1, the Subbass for bar 2: each registration
      // keeps the organ as it is when it is added.
      // The volume too, in decibels: 0 dB for the first, -6 dB for the second.
      auto* volume = proc.apvts().getParameter("masterGain");
      auto setDb = [&](float db) { volume->setValueNotifyingHost(volume->convertTo0to1(juce::Decibels::decibelsToGain(db))); };
      auto db = [&] { return juce::Decibels::gainToDecibels(proc.apvts().getRawParameterValue("masterGain")->load()); };
      draw(false, true);
      setDb(0.0f);
      const int first = panel.addMark(mp::ScoreMark::Kind::Registration, "mpn1");
      draw(true, false);
      setDb(-6.0f);
      const int second = panel.addMark(mp::ScoreMark::Kind::Registration, "mpn5");
      MP_CHECK(panel.marks()[1].states.find("\"gain\":-6") != std::string::npos,
               "the volume is kept in decibels: " + panel.marks()[1].states);
      panel.recallRegistration(first);
      MP_CHECK(std::abs(db()) < 0.05f, "the first sets 0 dB, got " + std::to_string(db()));
      panel.recallRegistration(second);
      MP_CHECK(std::abs(db() + 6.0f) < 0.05f, "the second -6 dB, got " + std::to_string(db()));
      const int note = panel.addMark(mp::ScoreMark::Kind::Note, "mpn2", "Breathe");
      MP_CHECK(first == 0 && second == 1 && note == 2, "three marks added");
      MP_CHECK(panel.addMark(mp::ScoreMark::Kind::Note, "no-such-note") < 0, "a note the score lacks takes none");
      MP_CHECK(panel.registrationOrder() == std::vector<int>({0, 1}), "the registrations in the order played");

      // Each is drawn on the page, over its note.
      auto page = panel.render();
      savePicture(page, "panel-marks.png");
      for (int i = 0; i < 3; ++i) {
        const auto r = panel.markBounds(i);
        MP_CHECK(!r.isEmpty(), "mark " + std::to_string(i) + " has an icon");
        const auto c = page.getPixelAt(juce::roundToInt(r.getX() + 4), juce::roundToInt(r.getCentreY()));
        MP_CHECK(c.getBrightness() > 0.4f && c.getSaturation() > 0.3f,
                 "drawn in colour, got " + c.toDisplayString(false).toStdString());
        MP_CHECK(std::abs(r.getX() + 2.0f - panel.notePosition(panel.marks()[static_cast<size_t>(i)].note).x) < 30.0f,
                 "above its note");
      }
      MP_CHECK(panel.markBounds(0).getX() < panel.markBounds(2).getX(), "the D's note to the right of the C's");

      // Played from the start: the Principal, then at bar 2 the Subbass.
      draw(false, false);
      panel.setStart(0);
      panel.play();
      for (int b = 0; b < 4; ++b) runBlock(proc);
      panel.render();
      runBlock(proc);
      MP_CHECK(drawn() == std::make_pair(false, true), "bar 1 plays on the Principal");
      MP_CHECK(panel.currentRegistration() == 0, "the first registration is the current one");
      for (int b = 0; b < 48000 * 21 / 10 / kBlock; ++b) {
        runBlock(proc);
        if (b % 8 == 0) panel.render();
      }
      panel.render();
      runBlock(proc);
      MP_CHECK(drawn() == std::make_pair(true, false), "bar 2 on the Subbass");
      MP_CHECK(panel.currentRegistration() == 1, "the second is current");
      panel.stop();

      // The buttons: back to the first, and its note is where Play starts.
      MP_CHECK(panel.stepRegistration(-1), "a step back");
      runBlock(proc);
      MP_CHECK(drawn() == std::make_pair(false, true) && panel.currentRegistration() == 0, "sets the first again");
      MP_CHECK(!panel.stepRegistration(-1), "and there is none before it");
      MP_CHECK(panel.startSeconds() < 0.01, "playing would start at its note");
      MP_CHECK(panel.stepRegistration(1), "a step forward");
      runBlock(proc);
      MP_CHECK(drawn() == std::make_pair(true, false), "sets the second again");

      // Stored again, it holds the organ as it is now.
      draw(true, true);
      panel.storeRegistration(0);
      draw(false, false);
      panel.recallRegistration(0);
      runBlock(proc);
      MP_CHECK(drawn() == std::make_pair(true, true), "a registration stored again recalls the new state");
      // Forgotten for this organ, it changes nothing here.
      panel.forgetRegistration(0);
      draw(false, false);
      panel.recallRegistration(0);
      runBlock(proc);
      MP_CHECK(drawn() == std::make_pair(false, false), "and forgotten for this organ, it leaves the organ alone");
      MP_CHECK(panel.undo(), "the forgetting is taken back");
      panel.recallRegistration(0);
      runBlock(proc);
      MP_CHECK(drawn() == std::make_pair(true, true), "and the state is there again");

      // Edited, moved, deleted, and each taken back.
      panel.setMarkText(2, "Breathe\nthen go on");
      MP_CHECK(panel.marks()[2].text == "Breathe\nthen go on", "a note's text changes");
      panel.moveMark(2, "mpn3");
      MP_CHECK(panel.marks()[2].note == "mpn3", "a mark moves to another note");
      panel.deleteMark(2);
      MP_CHECK(panel.marks().size() == 2, "a mark is deleted");
      MP_CHECK(panel.undo() && panel.marks().size() == 3 && panel.marks()[2].note == "mpn3", "the delete is taken back");
      MP_CHECK(panel.undo() && panel.marks()[2].note == "mpn2", "the move");
      MP_CHECK(panel.undo() && panel.marks()[2].text == "Breathe", "and the edit");
      panel.setMarkText(2, "Breathe\nthen go on");
      MP_CHECK(panel.saveMarks(), "the marks are written into the file");
    }

    // The file: what was set is there, the score's own content untouched,
    // and the first version kept beside it.
    juce::String text = file.loadFileAsString();
    MP_CHECK(text.contains("masterpiece-note\nBreathe\nthen go on"), "the note is in the file");
    MP_CHECK(text.contains("masterpiece-registration\nRegistration 1\n") &&
                 text.contains("masterpiece-registration\nRegistration 2\n"),
             "and both registrations");
    MP_CHECK(file.getSiblingFile("two-bars.musicxml.bak").loadFileAsString() == juce::String::fromUTF8(xml.c_str()),
             "the score as it was is kept beside it");
    {
      mp::ui::ScorePanel panel(proc, fonts);
      panel.setSize(1000, 700);
      MP_CHECK(panel.openFile(file), "the marked score opens");
      MP_CHECK(panel.marks().size() == 3, "with its three marks, got " + std::to_string(panel.marks().size()));
      draw(false, false);
      MP_CHECK(panel.stepRegistration(1), "the first registration is stepped to");
      runBlock(proc);
      MP_CHECK(drawn() == std::make_pair(true, true), "and sets the organ as it was stored");
      // A page turned forward sets the page's first registration; a page
      // turned back leaves the organ as it is (one page here: no turn at all).
      panel.showPage(1);
      MP_CHECK(panel.currentRegistration() == 0, "turning to the page shown changes nothing");
    }
    // Saved when it closes, with no step needed.
    {
      mp::ui::ScorePanel panel(proc, fonts);
      panel.setSize(1000, 700);
      panel.openFile(file);
      for (int k = 0; k < static_cast<int>(panel.marks().size()); ++k)
        if (panel.marks()[static_cast<size_t>(k)].kind == mp::ScoreMark::Kind::Note) panel.deleteMark(k);
    }
    MP_CHECK(!file.loadFileAsString().contains("masterpiece-note"), "a delete is in the file once the panel closes");

    // A compressed score keeps its marks in the same zip.
    const auto mxl = organ.root.getChildFile("two-bars.mxl");
    {
      juce::ZipFile::Builder zb;
      const juce::String container =
          "<?xml version=\"1.0\"?><container><rootfiles><rootfile full-path=\"score.musicxml\"/></rootfiles></container>";
      zb.addEntry(new juce::MemoryInputStream(container.toRawUTF8(), container.getNumBytesAsUTF8(), true), 9,
                  "META-INF/container.xml", juce::Time::getCurrentTime());
      zb.addEntry(new juce::MemoryInputStream(xml.data(), xml.size(), true), 9, "score.musicxml",
                  juce::Time::getCurrentTime());
      auto os = mxl.createOutputStream();
      zb.writeToStream(*os, nullptr);
    }
    {
      mp::ui::ScorePanel panel(proc, fonts);
      panel.setSize(1000, 700);
      MP_CHECK(panel.openFile(mxl), "the compressed score opens");
      panel.addMark(mp::ScoreMark::Kind::Note, "mpn4", "In the zip");
      MP_CHECK(panel.saveMarks(), "and its marks are written");
    }
    {
      mp::ui::ScorePanel panel(proc, fonts);
      panel.setSize(1000, 700);
      MP_CHECK(panel.openFile(mxl) && panel.marks().size() == 1 && panel.marks()[0].text == "In the zip",
               "the compressed score opens again with its note");
    }
  }
};
static ScorePanelMarksTest g_scorePanelMarks;

#endif  // MP_TEST_HAS_SCORE
