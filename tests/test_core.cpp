// Masterpiece tests — functional (correctness) + perf (throughput, CPU-time
// measured like GrandOrgue's perftests). Add tests by defining a class and
// instantiating it as a static — self-registers into mp::test::registry().
// HW-only premise (ADR-007): all fixtures are our own synthetic
// *.Organ_Hauptwerk_xml / *.CustomOrgan_Hauptwerk_xml under tests/fixtures/.
#include "MpTest.h"

#include "../src/mp_core/CodmCompiler.h"
#include "../src/mp_core/KeyboardLayout.h"
#include "../src/mp_core/OdfLoader.h"
#include "../src/mp_core/Temperament.h"
#include "../src/mp_audio/AudioGraph.h"
#include "../src/mp_audio/MixerConfig.h"
#include "../src/mp_audio/VoicingSet.h"
#include "../src/mp_audio/Favourites.h"
#include "../src/mp_control/Control.h"
#include "../src/mp_control/Registration.h"
#include "../src/mp_control/MidiMap.h"
#include "../src/mp_control/LcdPanel.h"
#include "../src/mp_control/Combinations.h"

#include <set>
#include "../src/mp_control/StageSwitches.h"
#include "../src/mp_control/Stepper.h"
#include "../src/mp_control/WindSolver.h"
#include "../src/mp_control/SwitchNetwork.h"
#ifdef MP_TEST_HAS_SAMPLER
#include "../src/mp_sampler/StreamingEngine.h"
#include "../src/mp_sampler/DiskProbe.h"
#include "../src/mp_sampler/VoiceEngine.h"
#endif
#ifdef MP_TEST_HAS_DSP
#include "../src/mp_dsp/Dsp.h"
#endif
#ifdef MP_TEST_HAS_AUDIO
#include "../src/mp_audio/AudioRecorder.h"
#include "../src/mp_audio/SampleLibrary.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/OrganDialog.h"
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <thread>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>

using mp::test::Category;

#ifndef MP_TEST_FIXTURES_DIR
#define MP_TEST_FIXTURES_DIR "fixtures"
#endif

static std::string readFixture(const char* name) {
  std::ifstream f(std::string(MP_TEST_FIXTURES_DIR) + "/" + name,
                  std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static bool loadFixture(const char* name, mp::OrganModel& m,
                         mp::OdfDiagnostics& d) {
  const std::string xml = readFixture(name);
  if (xml.empty())
    throw mp::test::Failure(std::string("fixture unreadable: ") + name);
  mp::OdfLoader l;
  mp::OdfLoader::Options o;
  return l.loadFromXmlString(xml, name, o, m, d);
}

// ---------------------------------------------------------------- functional

class DetectTypeTest final : public mp::test::Test {
public:
  DetectTypeTest() : Test("functional.odf.detect-type", Category::Functional) {}
  void run() override {
    MP_CHECK(mp::OdfLoader::detectType(
                 "<Hauptwerk><Organ_Hauptwerk", "test.Organ_Hauptwerk_xml") ==
                 mp::OdfType::Full,
             "full ODF not detected");
    MP_CHECK(mp::OdfLoader::detectType("<CustomOrgan_Hauptwerk",
                                       "x.CustomOrgan_Hauptwerk_xml") ==
                 mp::OdfType::Codm,
             "CODM not detected");
    MP_CHECK(mp::OdfLoader::detectType("<foo", "x.xml") ==
                 mp::OdfType::Unknown,
             "garbage should be Unknown");
  }
};

class LoaderRejectsUnknownTest final : public mp::test::Test {
public:
  LoaderRejectsUnknownTest()
    : Test("functional.odf.reject-unknown", Category::Functional) {}
  void run() override {
    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    MP_CHECK(!l.loadFromXmlString("<foo/>", "x.xml", o, m, d),
             "unknown ODF must be rejected");
    MP_CHECK(!d.errors.empty(), "errors must explain the rejection");
  }
};

class LoaderToleranceTest final : public mp::test::Test {
public:
  LoaderToleranceTest()
    : Test("functional.odf.unknown-table-tolerance", Category::Functional) {}
  void run() override {
    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    MP_CHECK(l.loadFromXmlString(
                 "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                 "<ObjectList ObjectType=\"_General\"><_General>"
                 "<Identification_UniqueOrganID>1</Identification_UniqueOrganID>"
                 "</_General></ObjectList>"
                 "<ObjectList ObjectType=\"UnknownTable\"><UnknownTable/></ObjectList>"
                 "</Hauptwerk>",
                 "a.Organ_Hauptwerk_xml", o, m, d),
             "known-type ODF with unknown table must load (ADR-002)");
    MP_CHECK(!d.warnings.empty(),
             "unknown table must produce a warning, not silence");
  }
};

// An EMPTY unknown table is a placeholder, not a feature the set uses.
// Hauptwerk writes several object lists with nothing in them -- every one of
// the 22 sets on hand carries six such -- and warning about each sent an
// implementation plan after features no set here uses. It is still recorded,
// so nothing is lost; it just is not announced. A table with rows still warns.
class LoaderEmptyTableTest final : public mp::test::Test {
public:
  LoaderEmptyTableTest()
    : Test("functional.odf.empty-unknown-table-quiet", Category::Functional) {}
  void run() override {
    auto load = [](const char* tableXml, mp::OrganModel& m, mp::OdfDiagnostics& d) {
      mp::OdfLoader l;
      mp::OdfLoader::Options o;
      return l.loadFromXmlString(
          std::string("<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                      "<ObjectList ObjectType=\"_General\"><_General>"
                      "<Identification_UniqueOrganID>1</Identification_UniqueOrganID>"
                      "</_General></ObjectList>") +
              tableXml + "</Hauptwerk>",
          "a.Organ_Hauptwerk_xml", o, m, d);
    };
    auto mentions = [](const mp::OdfDiagnostics& d, const std::string& what) {
      for (const auto& w : d.warnings)
        if (w.find(what) != std::string::npos) return true;
      return false;
    };

    mp::OrganModel m1; mp::OdfDiagnostics d1;
    MP_CHECK(load("<ObjectList ObjectType=\"ReversiblePiston\"></ObjectList>", m1, d1),
             "a set with an empty unknown table loads");
    MP_CHECK(!mentions(d1, "ReversiblePiston"),
             "an empty table is not announced as an unsupported feature");
    MP_CHECK(m1.unknownTables.size() == 1,
             "but it is still recorded, so nothing is lost");

    mp::OrganModel m2; mp::OdfDiagnostics d2;
    MP_CHECK(load("<ObjectList ObjectType=\"ReversiblePiston\">"
                  "<ReversiblePiston/><ReversiblePiston/></ObjectList>", m2, d2),
             "a set with a populated unknown table loads");
    MP_CHECK(mentions(d2, "ReversiblePiston") && mentions(d2, "2 row(s)"),
             "a table with rows warns, and says how many it ignored");
  }
};

// An organ can declare no StopRank at all and reach every pipe through its
// switch wiring: the key is a switch, it is wired through the stop's switch to
// a pallet switch, and each pipe names its pallet. Alessandria, Erfurt and
// Swieta Lipka are built this way and were silent. The loader has to keep the
// pallet, and the network has to open it only while the key AND the stop are
// down, and close it when either lets go.
class PalletSwitchTest final : public mp::test::Test {
public:
  PalletSwitchTest()
    : Test("functional.odf.pallet-switch", Category::Functional) {}
  void run() override {
    const std::string odf =
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"_General\"><_General>"
        "<Identification_UniqueOrganID>1</Identification_UniqueOrganID>"
        "</_General></ObjectList>"
        "<ObjectList ObjectType=\"Switch\">"
        "<Switch><SwitchID>9001</SwitchID><Name>Key 36</Name><Latching>N</Latching></Switch>"
        "<Switch><SwitchID>20002</SwitchID><Name>Stop</Name><Latching>Y</Latching></Switch>"
        "<Switch><SwitchID>327999</SwitchID><Name>Pallet</Name><Latching>N</Latching></Switch>"
        "</ObjectList>"
        "<ObjectList ObjectType=\"KeyboardKey\"><KeyboardKey><KeyboardID>1</KeyboardID>"
        "<SwitchID>9001</SwitchID><NormalMIDINoteNumber>36</NormalMIDINoteNumber>"
        "</KeyboardKey></ObjectList>"
        "<ObjectList ObjectType=\"SwitchLinkage\"><SwitchLinkage>"
        "<SourceSwitchID>9001</SourceSwitchID><DestSwitchID>327999</DestSwitchID>"
        "<ConditionSwitchID>20002</ConditionSwitchID>"
        "<SourceSwitchLinkIfEngaged>Y</SourceSwitchLinkIfEngaged>"
        "<ConditionSwitchLinkIfEngaged>Y</ConditionSwitchLinkIfEngaged>"
        "<EngageLinkActionCode>1</EngageLinkActionCode>"
        "<DisengageLinkActionCode>2</DisengageLinkActionCode>"
        "</SwitchLinkage></ObjectList>"
        "<ObjectList ObjectType=\"Rank\"><Rank><RankID>1</RankID><Name>Principal</Name></Rank></ObjectList>"
        "<ObjectList ObjectType=\"Pipe_SoundEngine01\"><Pipe_SoundEngine01>"
        "<PipeID>101</PipeID><RankID>1</RankID>"
        "<ControllingPalletSwitchID>327999</ControllingPalletSwitchID>"
        "<NormalMIDINoteNumber>36</NormalMIDINoteNumber>"
        "</Pipe_SoundEngine01></ObjectList></Hauptwerk>";
    mp::OdfLoader l;
    mp::OdfLoader::Options o;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(l.loadFromXmlString(odf, "a.Organ_Hauptwerk_xml", o, m, d),
             "a pallet-wired organ loads");
    const auto rank = m.ranks.find(1);
    MP_CHECK(rank != m.ranks.end() && rank->second.pipes.size() == 1 &&
                 rank->second.pipes[0].palletSwitchId == 327999,
             "the pipe keeps the switch that opens its pallet");
    MP_CHECK(m.keyboardKeys.count(9001) == 1, "the key is a switch");

    mp::SwitchNetwork net;
    net.reset(m);
    net.set(9001, true);
    MP_CHECK(!net.engaged(327999), "a key with the stop in opens no pallet");
    net.set(20002, true);
    MP_CHECK(net.engaged(327999),
             "drawing the stop under a held key opens the pallet at once");
    net.set(9001, false);
    MP_CHECK(!net.engaged(327999), "letting go of the key closes it");
    net.set(9001, true);
    MP_CHECK(net.engaged(327999), "key and stop together open it");
    net.set(20002, false);
    MP_CHECK(!net.engaged(327999), "pushing the stop in under the key closes it");
  }
};

#ifdef MP_TEST_HAS_AUDIO
#include "../src/mp_ui/BmpImage.h"

// Console artwork in BMP. JUCE reads PNG, JPEG and GIF; the older Hauptwerk
// sets paint their consoles in BMP, and those came out black (issue #24).
// The depths and layouts checked here are the ones such a set uses.
class BmpImageTest final : public mp::test::Test {
public:
  BmpImageTest() : Test("functional.ui.bmp-artwork", Category::Functional) {}

  void run() override {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "mp_bmp_test_51c7";
    if (ec) return;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) return;
    struct Cleanup { fs::path p; ~Cleanup(){ std::error_code e; std::filesystem::remove_all(p,e);} } cleanup{dir};

    // Red, green, blue and white across a four-pixel row.
    const std::vector<std::array<int, 3>> want = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};

    check(dir / "b24.bmp", write24(dir / "b24.bmp"), want, 24);
    check(dir / "b32.bmp", write32(dir / "b32.bmp", 255), want, 32);
    // Alpha bytes left at zero: the image must still be visible.
    check(dir / "b32z.bmp", write32(dir / "b32z.bmp", 0), want, 32);
    check(dir / "b8.bmp", write8(dir / "b8.bmp"), want, 8);
    check(dir / "btd.bmp", writeTopDown(dir / "btd.bmp"), want, -24);

    // Not a BMP at all: an invalid image, not a crash and not a guess.
    const auto junk = dir / "junk.bmp";
    { std::ofstream f(junk, std::ios::binary); f << "not a bitmap at all"; }
    MP_CHECK(!mp::loadBmpImage(juce::File(junk.string())).isValid(),
             "a file that is not a BMP gives an invalid image");
  }

private:
  static void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x & 0xff)); v.push_back((uint8_t)((x >> 8) & 0xff));
    v.push_back((uint8_t)((x >> 16) & 0xff)); v.push_back((uint8_t)((x >> 24) & 0xff));
  }
  static void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x & 0xff)); v.push_back((uint8_t)((x >> 8) & 0xff));
  }
  static void writeFile(const std::filesystem::path& p, const std::vector<uint8_t>& v) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(v.data()), (std::streamsize) v.size());
  }
  // One row of four pixels, so padding is exercised only where it matters.
  static std::vector<uint8_t> header(int w, int h, int bpp, size_t dataSize, int palette = 0) {
    std::vector<uint8_t> v;
    const uint32_t off = 14 + 40 + (uint32_t)(palette * 4);
    v.push_back('B'); v.push_back('M');
    put32(v, (uint32_t)(off + dataSize)); put16(v, 0); put16(v, 0); put32(v, off);
    put32(v, 40); put32(v, (uint32_t) w); put32(v, (uint32_t) h);
    put16(v, 1); put16(v, (uint16_t) bpp); put32(v, 0); put32(v, (uint32_t) dataSize);
    put32(v, 2835); put32(v, 2835); put32(v, (uint32_t) palette); put32(v, 0);
    return v;
  }
  static bool write24(const std::filesystem::path& p) {
    std::vector<uint8_t> rows{0,0,255, 0,255,0, 255,0,0, 255,255,255};
    auto v = header(4, 1, 24, rows.size());
    v.insert(v.end(), rows.begin(), rows.end());
    writeFile(p, v); return true;
  }
  static bool write32(const std::filesystem::path& p, int alpha) {
    std::vector<uint8_t> rows{0,0,255,(uint8_t)alpha, 0,255,0,(uint8_t)alpha,
                              255,0,0,(uint8_t)alpha, 255,255,255,(uint8_t)alpha};
    auto v = header(4, 1, 32, rows.size());
    v.insert(v.end(), rows.begin(), rows.end());
    writeFile(p, v); return true;
  }
  static bool write8(const std::filesystem::path& p) {
    auto v = header(4, 1, 8, 4, 4);
    const uint8_t pal[16] = {0,0,255,0, 0,255,0,0, 255,0,0,0, 255,255,255,0};
    v.insert(v.end(), pal, pal + 16);
    const uint8_t rows[4] = {0, 1, 2, 3};
    v.insert(v.end(), rows, rows + 4);
    writeFile(p, v); return true;
  }
  static bool writeTopDown(const std::filesystem::path& p) {
    std::vector<uint8_t> rows{0,0,255, 0,255,0, 255,0,0, 255,255,255};
    auto v = header(4, -1, 24, rows.size());
    v.insert(v.end(), rows.begin(), rows.end());
    writeFile(p, v); return true;
  }
  void check(const std::filesystem::path& p, bool written,
             const std::vector<std::array<int, 3>>& want, int what) {
    if (!written) return;
    const juce::Image img = mp::loadBmpImage(juce::File(p.string()));
    const std::string tag = std::to_string(what) + "-bit";
    MP_CHECK(img.isValid() && img.getWidth() == 4,
             tag + ": the bitmap loads at its stated size");
    if (!img.isValid()) return;
    for (int x = 0; x < 4; ++x) {
      const juce::Colour c = img.getPixelAt(x, 0);
      MP_CHECK(c.getRed() == want[(size_t) x][0] && c.getGreen() == want[(size_t) x][1] &&
                   c.getBlue() == want[(size_t) x][2] && c.getAlpha() == 255,
               tag + ": pixel " + std::to_string(x) + " keeps its colour, opaque");
    }
  }
};

#endif // MP_TEST_HAS_AUDIO

class EncryptedDetectionTest final : public mp::test::Test {
public:
  EncryptedDetectionTest()
    : Test("functional.odf.encrypted-detection", Category::Functional) {}
  void run() override {
    mp::OdfLoader l;
    mp::OdfLoader::Options o;
    // lowercase extension (the common case)
    {
      mp::OrganModel m;
      mp::OdfDiagnostics d;
      MP_CHECK(l.loadFromXmlString(
                   "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                   "<ObjectList ObjectType=\"_General\"><_General>"
                   "<Identification_UniqueOrganID>1</Identification_UniqueOrganID>"
                   "</_General></ObjectList>"
                   "<ObjectList ObjectType=\"Sample\"><Sample>"
                   "<SampleID>1</SampleID><SampleFilename>x.hbw</SampleFilename>"
                   "</Sample></ObjectList></Hauptwerk>",
                   "a.Organ_Hauptwerk_xml", o, m, d),
               "encrypted sample must load the model with a report (ADR-003)");
      MP_CHECK(m.hasEncryptedSamples, "hasEncryptedSamples flag must be set");
      MP_CHECK(!d.encryptedSamples.empty(), "encrypted file must be listed");
    }
    // uppercase extension must be detected too (case-insensitive scan)
    {
      mp::OrganModel m;
      mp::OdfDiagnostics d;
      MP_CHECK(l.loadFromXmlString(
                   "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                   "<ObjectList ObjectType=\"_General\"><_General>"
                   "<Identification_UniqueOrganID>1</Identification_UniqueOrganID>"
                   "</_General></ObjectList>"
                   "<ObjectList ObjectType=\"Sample\"><Sample>"
                   "<SampleID>1</SampleID><SampleFilename>X.HBX</SampleFilename>"
                   "</Sample></ObjectList></Hauptwerk>",
                   "a.Organ_Hauptwerk_xml", o, m, d),
               "uppercase HBX must be detected");
      MP_CHECK(m.hasEncryptedSamples, "uppercase extension must set flag");
    }
  }
};

class FixtureCorpusTest final : public mp::test::Test {
public:
  FixtureCorpusTest()
    : Test("functional.fixtures.corpus", Category::Functional) {}
  void run() override {
    {
      mp::OrganModel m;
      mp::OdfDiagnostics d;
      MP_CHECK(loadFixture("minimal.Organ_Hauptwerk_xml", m, d),
               "minimal full ODF fixture must load");
      MP_CHECK(m.odfType == mp::OdfType::Full, "minimal fixture = Full type");
    }
    {
      mp::OrganModel m;
      mp::OdfDiagnostics d;
      MP_CHECK(loadFixture("minimal.CustomOrgan_Hauptwerk_xml", m, d),
               "minimal CODM fixture must load");
      MP_CHECK(m.odfType == mp::OdfType::Codm, "CODM fixture = Codm type");
    }
    {
      mp::OrganModel m;
      mp::OdfDiagnostics d;
      MP_CHECK(loadFixture("edge.unknown-tables.Organ_Hauptwerk_xml", m, d),
               "unknown tables must load with warnings (ADR-002)");
      MP_CHECK(m.odfType == mp::OdfType::Full, "edge fixture = Full type");
    }
    {
      mp::OrganModel m;
      mp::OdfDiagnostics d;
      MP_CHECK(loadFixture("edge.encrypted.Organ_Hauptwerk_xml", m, d),
               "encrypted fixture must load with report (ADR-003)");
      MP_CHECK(m.hasEncryptedSamples, "encrypted fixture sets flag");
    }
    {
      mp::OrganModel m;
      mp::OdfDiagnostics d;
      MP_CHECK(loadFixture("edge.dangling-ids.Organ_Hauptwerk_xml", m, d),
               "dangling IDs must load + report until link phase (M1.3)");
      MP_CHECK(m.odfType == mp::OdfType::Full, "dangling fixture = Full type");
    }
  }
};

class CodmCodesTest final : public mp::test::Test {
public:
  CodmCodesTest() : Test("functional.codm.codes", Category::Functional) {}
  void run() override {
    MP_CHECK(mp::codm::captureDivisionForStopCode(2001) == 0,
             "20xx stop code -> division 0 (Pedal)");
    MP_CHECK(mp::codm::captureDivisionForStopCode(2300) == 3,
             "23xx stop code -> division 3");
    MP_CHECK(mp::codm::captureDivisionForStopCode(9999) == -1,
             "unmapped code must return -1 for triage");
    MP_CHECK(mp::codm::captureDivisionForCouplerCode(10000) == -2,
             "custom coupler -> full-ODF path marker");
  }
};

class CodmDefaultsTest final : public mp::test::Test {
public:
  CodmDefaultsTest() : Test("functional.codm.defaults", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    for (int i = 0; i < 8; ++i) {
      mp::Division div;
      div.divisionId = i;
      div.manualNumber = i;
      div.name = "Div" + std::to_string(i);
      m.divisions.emplace(i, std::move(div));
    }
    mp::OdfDiagnostics d;
    mp::codm::applyCodmDefaults(m, d);
    // Setter + General Cancel + 20 generals + 20 divisionals per division.
    const size_t expected =
        2 + static_cast<size_t>(mp::codm::kNumGenerals) +
        8 * static_cast<size_t>(mp::codm::kNumDivisionalsPerDivision);
    MP_CHECK(m.combinations.size() == expected,
             "CODM defaults build setter + cancel + generals + divisionals");
    MP_CHECK(!d.warnings.empty(),
             "more than 7 standard divisions must warn");

    // Re-running must be idempotent: author-declared combinations always win.
    const size_t before = m.combinations.size();
    mp::codm::applyCodmDefaults(m, d);
    MP_CHECK(m.combinations.size() == before,
             "applyCodmDefaults is idempotent");

    // Jamb cap: one stop past the per-division limit must be reported.
    mp::OrganModel jam;
    mp::Division only;
    only.divisionId = 1000;
    only.name = "Great";
    jam.divisions.emplace(only.divisionId, only);
    for (int i = 0; i < mp::codm::kMaxJambPerDivision + 1; ++i) {
      mp::Stop st;
      st.stopId = 3000 + i;
      st.divisionId = 1000;
      jam.stops.emplace(st.stopId, std::move(st));
    }
    mp::OdfDiagnostics jd;
    mp::codm::applyCodmDefaults(jam, jd);
    MP_CHECK(!jd.jambOverflows.empty(), "jamb cap overflow is reported");
  }
};

// The temperament tables are derived from fifth-chain definitions
// (build-scripts/gen-temperaments.py), so this test checks the musical
// invariants those definitions imply rather than re-listing the numbers:
// a wrong table breaks an interval, not just a digit.
class TemperamentLibraryTest final : public mp::test::Test {
public:
  TemperamentLibraryTest()
    : Test("functional.temperament.library", Category::Functional) {}

  // Width in cents of an interval of `semitones` starting on pitch class
  // `root`. Intervals wrap the octave (F#-C# runs past C), so the upper note
  // is taken mod 12 and the nominal width is added back.
  static double interval(const mp::Temperament& t, int root, int semitones) {
    const size_t lo = static_cast<size_t>(((root % 12) + 12) % 12);
    const size_t hi = static_cast<size_t>(((root + semitones) % 12 + 12) % 12);
    return 100.0 * semitones + t.centsOffset12[hi] - t.centsOffset12[lo];
  }
  static double majorThird(const mp::Temperament& t, int root) {
    return interval(t, root, 4);
  }
  static double fifth(const mp::Temperament& t, int root) {
    return interval(t, root, 7);
  }

  void run() override {
    const auto& lib = mp::temperamentLibrary();
    MP_CHECK(lib.size() >= 8, "the built-in library carries the historical set");
    for (const auto& t : lib) {
      MP_CHECK(t.centsOffset12.size() == 12,
               "every temperament has 12 offsets");
      MP_CHECK(std::fabs(t.centsOffset12[0]) < 1e-9,
               "every temperament is anchored on C");
      for (double c : t.centsOffset12)
        MP_CHECK(std::fabs(c) < 60.0, "no offset exceeds half a semitone");
    }

    // Lookup is case-insensitive and rejects what it does not know.
    MP_CHECK(mp::findTemperament("equal") != nullptr, "lookup is case-insensitive");
    MP_CHECK(mp::findTemperament("VALLOTTI") != nullptr, "lookup ignores case");
    MP_CHECK(mp::findTemperament("Nonesuch 1723") == nullptr,
             "an unknown temperament name returns nullptr");

    const auto* equal = mp::findTemperament("Equal");
    MP_CHECK(equal != nullptr, "Equal is present");
    for (double c : equal->centsOffset12)
      MP_CHECK(std::fabs(c) < 1e-9, "Equal is 12-TET exactly");

    // Quarter-comma meantone is defined by its pure major thirds: C-E must be
    // the just 5:4 (386.31 cents), not the 400 cents of equal temperament.
    const auto* meantone = mp::findTemperament("Meantone 1/4 comma");
    MP_CHECK(meantone != nullptr, "quarter-comma meantone is present");
    MP_CHECK(std::fabs(majorThird(*meantone, 0) - 386.314) < 0.01,
             "quarter-comma meantone has a pure major third on C");
    MP_CHECK(std::fabs(majorThird(*meantone, 5) - 386.314) < 0.01,
             "quarter-comma meantone has a pure major third on F");
    MP_CHECK(std::fabs(majorThird(*meantone, 9) - 386.314) < 0.01,
             "quarter-comma meantone has a pure major third on A");

    // Pythagorean is defined by its pure fifths: C-G is the just 3:2.
    const auto* pyth = mp::findTemperament("Pythagorean");
    MP_CHECK(pyth != nullptr, "Pythagorean is present");
    int pureFifths = 0;
    for (int root = 0; root < 12; ++root)
      if (std::fabs(fifth(*pyth, root) - 701.955) < 0.01) ++pureFifths;
    MP_CHECK(pureFifths == 11,
             "Pythagorean has 11 pure fifths and one wolf");
    MP_CHECK(std::fabs(fifth(*pyth, 8) - 701.955) > 20.0,
             "the Pythagorean wolf sits on G#-Eb");

    // Werckmeister III and Vallotti are well temperaments: every key is
    // playable, so no major third may be worse than the Pythagorean ditone.
    for (const char* name : {"Werckmeister III", "Vallotti", "Young II",
                             "Kirnberger III"}) {
      const auto* wt = mp::findTemperament(name);
      MP_CHECK(wt != nullptr, "well temperament present in the library");
      for (int root = 0; root < 12; ++root) {
        MP_CHECK(majorThird(*wt, root) > 380.0 && majorThird(*wt, root) < 412.0,
                 "a well temperament keeps every major third usable");
        MP_CHECK(fifth(*wt, root) > 690.0 && fifth(*wt, root) < 706.0,
                 "a well temperament has no wolf fifth");
      }
    }

    // Vallotti tempers the six natural fifths and leaves the rest pure, so
    // C-G is narrow while its remote counterpart F#-C# is not.
    const auto* vallotti = mp::findTemperament("Vallotti");
    MP_CHECK(vallotti != nullptr, "Vallotti is present");
    MP_CHECK(std::fabs(fifth(*vallotti, 0) - (701.955 - 23.460 / 6.0)) < 0.01,
             "Vallotti narrows C-G by a sixth of a Pythagorean comma");
    MP_CHECK(std::fabs(fifth(*vallotti, 6) - 701.955) < 0.01,
             "Vallotti leaves the remote F#-C# fifth pure");
    int narrowed = 0;
    for (int root = 0; root < 12; ++root)
      if (fifth(*vallotti, root) < 700.0) ++narrowed;
    MP_CHECK(narrowed == 6, "Vallotti tempers exactly six fifths");

    // A library temperament must flow through the pitch solver unchanged.
    const double e = mp::pipeTargetHz(64, 8, 440.0, 0.0, *meantone, 0);
    const double eEqual = mp::pipeTargetHz(64, 8, 440.0, 0.0, *equal, 0);
    MP_CHECK(std::fabs(mp::centsBetween(e, eEqual) -
                       meantone->centsOffset12[4]) < 1e-6,
             "the solver applies the temperament offset of the sounding note");
  }
};

// M2.3/M2.4: expression hardware — swell boxes, tremulants, continuous
// controls and their linkages, plus noise ranks. The fixture deliberately
// carries one instance of each fault the roadmap's validator queries name, so
// this test checks both that the good rows parse and that the bad rows are
// caught rather than silently accepted.
// M2.4 runtime: continuous-control positions and the linkages between them.
// This is what a swell shoe actually does between MIDI and the enclosure DSP,
// so it is checked against the expression fixture rather than a toy model.
// M2.2: the ODF Temperament table. An organ that declares a tuning we cannot
// resolve must SAY so — falling back to equal temperament silently would
// retune the instrument behind the player's back, and it would sound fine,
// which is exactly why it needs catching at load.
// ------------------------------------------------- M2: the voice engine
//
// These tests render actual audio through the engine with a synthesised
// sample, so they check what comes out of the mixer rather than merely that
// the code runs. A silent engine is the failure mode that matters here.
namespace voicetest {

// A pipe sample: `hz` at `sr`, with a sustain loop over its second half.
inline mp::SampleBuffer makeTone(double hz, double sr, int frames,
                                 int channels = 1, bool loop = true) {
  mp::SampleBuffer b;
  b.numChannels = channels;
  b.sampleRate = sr;
  b.numFrames = frames;
  b.frames.resize(static_cast<size_t>(frames) * static_cast<size_t>(channels));
  for (int i = 0; i < frames; ++i)
    for (int c = 0; c < channels; ++c)
      b.frames[static_cast<size_t>(i) * static_cast<size_t>(channels) +
               static_cast<size_t>(c)] =
          static_cast<float>(std::sin(2.0 * 3.141592653589793 * hz * i / sr));
  if (loop) {
    b.loopStart = frames / 2;
    b.loopEnd = frames;
  }
  return b;
}

inline double rms(const std::vector<float>& v) {
  if (v.empty()) return 0.0;
  double acc = 0.0;
  for (float x : v) acc += static_cast<double>(x) * x;
  return std::sqrt(acc / static_cast<double>(v.size()));
}

// A pipe with one layer, one attack (sample 1) and one release (sample 2).
struct Fixture {
  mp::Pipe pipe;
  mp::SampleBuffer attack;
  mp::SampleBuffer release;

  Fixture() {
    attack = makeTone(440.0, 48000.0, 4800);
    release = makeTone(220.0, 48000.0, 2400, 1, false);

    mp::PipeLayer layer;
    layer.layerId = 1;
    mp::AttackSample a;
    a.id = 11;
    a.sample.sampleId = 1;
    layer.attacks.push_back(a);
    mp::ReleaseSample r;
    r.id = 21;
    r.sample.sampleId = 2;
    layer.releases.push_back(r);

    pipe.pipeId = 100;
    pipe.midiNote = 69;
    pipe.layers.push_back(std::move(layer));
  }

  mp::SampleProvider provider() {
    return [this](mp::Id id) -> const mp::SampleBuffer* {
      if (id == 1) return &attack;
      if (id == 2) return &release;
      return nullptr;
    };
  }
};

} // namespace voicetest

// A stop drawn, or pushed in, while the key is still down.
//
// On a real organ the slider admits wind to a rank the key is already asking
// for: the pipe speaks at once, and stops at once when the stop goes in, with
// the key untouched. Reported by a player as two faults -- a stop drawn mid
// note stayed silent until the note was struck again, and a stop pushed in
// mid note kept sounding.
//
// The engine's half of that is here: a rank added under a note already
// sounding joins that note and is released with it, and one pipe can be let
// go on its own while the rest of the note plays on.
class VoiceStopWhileHeldTest final : public mp::test::Test {
public:
  VoiceStopWhileHeldTest()
    : Test("functional.voice.stop-drawn-while-held", Category::Functional) {}

  // Render until the engine falls quiet, or give up: a voice that never ends
  // must fail the test rather than hang it.
  static int renderUntilIdle(mp::VoiceEngine& eng, int maxBlocks = 200) {
    std::vector<float> buf(512, 0.0f);
    float* out[1] = {buf.data()};
    for (int i = 0; i < maxBlocks; ++i) {
      if (eng.activeVoiceCount() == 0) return i;
      std::fill(buf.begin(), buf.end(), 0.0f);
      eng.render(out, 1, 512);
    }
    return -1;
  }

  void run() override {
    voicetest::Fixture fx;
    // A second rank at the same key: the stop about to be drawn.
    mp::Pipe second = fx.pipe;
    second.pipeId = 200;

    mp::VoiceEngine eng;
    eng.prepare(48000.0, 64, 1);
    eng.setSampleProvider(fx.provider());

    std::vector<float> buf(512, 0.0f);
    float* out[1] = {buf.data()};

    mp::VoiceStart drawn;
    drawn.pipe = &fx.pipe;
    drawn.layer = &fx.pipe.layers[0];
    drawn.attackIndex = 0;
    drawn.velocity = 100;
    drawn.ratio = 1.0;
    drawn.gain = 1.0f;

    // A key goes down with one stop drawn.
    const uint64_t note = 7;
    MP_CHECK(eng.startVoice(drawn, note) >= 0, "the drawn stop sounds");
    eng.render(out, 1, 512);
    const double oneStop = voicetest::rms(buf);
    MP_CHECK(oneStop > 0.1, "one stop is audible");
    MP_CHECK(eng.activeVoiceCount() == 1, "one rank is speaking");

    // A second stop is drawn WITHOUT the key moving: same note id, so the
    // note-off still to come will release it with the rest of the note.
    mp::VoiceStart added = drawn;
    added.pipe = &second;
    added.layer = &second.layers[0];
    MP_CHECK(eng.startVoice(added, note) >= 0,
             "a stop drawn under a held key starts speaking at once");
    MP_CHECK(eng.activeVoiceCount() == 2, "both ranks are now speaking");

    std::fill(buf.begin(), buf.end(), 0.0f);
    eng.render(out, 1, 512);
    MP_CHECK(voicetest::rms(buf) > oneStop,
             "the organ is louder for the stop just drawn");

    // That stop is pushed in again, still without the key moving. Only its
    // pipe lets go; the other rank plays on, and the key is still down.
    eng.noteOffPipe(note, 200, mp::NoteRelease{});
    for (int i = 0; i < 40; ++i) {   // past the release sample's own length
      std::fill(buf.begin(), buf.end(), 0.0f);
      eng.render(out, 1, 512);
    }
    MP_CHECK(eng.activeVoiceCount() == 1,
             "the stop pushed in has stopped; the other rank has not");
    std::fill(buf.begin(), buf.end(), 0.0f);
    eng.render(out, 1, 512);
    MP_CHECK(voicetest::rms(buf) > 0.1, "the held note is still sounding");

    // Releasing a pipe the note is not sounding changes nothing.
    eng.noteOffPipe(note, 999, mp::NoteRelease{});
    MP_CHECK(eng.activeVoiceCount() == 1, "an unrelated pipe id is ignored");

    // And the key finally comes up: what is left releases and ends.
    eng.noteOff(note, mp::NoteRelease{});
    MP_CHECK(renderUntilIdle(eng) >= 0, "the note ends when the key is released");

    // One note's stop change must not reach another note holding the same
    // rank -- two keys down, one stop pushed in, the other key unaffected.
    mp::VoiceEngine two;
    two.prepare(48000.0, 64, 1);
    two.setSampleProvider(fx.provider());
    two.startVoice(added, 11);
    two.startVoice(added, 22);
    MP_CHECK(two.activeVoiceCount() == 2, "two keys hold the same rank");
    two.noteOffPipe(11, 200, mp::NoteRelease{});
    for (int i = 0; i < 40; ++i) {
      std::fill(buf.begin(), buf.end(), 0.0f);
      two.render(out, 1, 512);
    }
    MP_CHECK(two.activeVoiceCount() == 1,
             "only the named note let go; the other key still sounds");
  }
};

class VoiceEngineRenderTest final : public mp::test::Test {
public:
  VoiceEngineRenderTest()
    : Test("functional.voice.render", Category::Functional) {}
  void run() override {
    voicetest::Fixture fx;
    mp::VoiceEngine eng;
    eng.prepare(48000.0, 64, 1);
    eng.setSampleProvider(fx.provider());

    std::vector<float> buf(512, 0.0f);
    float* out[1] = {buf.data()};

    // Silence before anything is played: an engine that hums at rest is broken.
    eng.render(out, 1, 512);
    MP_CHECK(voicetest::rms(buf) == 0.0, "an idle engine renders exact silence");
    MP_CHECK(eng.activeVoiceCount() == 0, "no voices active at rest");

    mp::VoiceStart start;
    start.pipe = &fx.pipe;
    start.layer = &fx.pipe.layers[0];
    start.attackIndex = 0;
    start.velocity = 100;
    start.ratio = 1.0;
    start.gain = 1.0f;

    const int slot = eng.startVoice(start, 1);
    MP_CHECK(slot >= 0, "a voice starts when its sample is resident");
    MP_CHECK(eng.activeVoiceCount() == 1, "exactly one voice is sounding");

    std::fill(buf.begin(), buf.end(), 0.0f);
    eng.render(out, 1, 512);
    const double sounding = voicetest::rms(buf);
    MP_CHECK(sounding > 0.1, "a sounding voice actually produces audio");

    // Gain is honoured, and the engine mixes additively.
    mp::VoiceEngine quiet;
    quiet.prepare(48000.0, 64, 1);
    quiet.setSampleProvider(fx.provider());
    start.gain = 0.5f;
    quiet.startVoice(start, 1);
    std::vector<float> qbuf(512, 0.0f);
    float* qout[1] = {qbuf.data()};
    quiet.render(qout, 1, 512);
    MP_CHECK(std::fabs(voicetest::rms(qbuf) / sounding - 0.5) < 0.05,
             "voice gain scales the rendered level");

    // A start whose sample is not resident must stay silent, not fault.
    mp::VoiceEngine none;
    none.prepare(48000.0, 8, 1);
    none.setSampleProvider([](mp::Id) -> const mp::SampleBuffer* { return nullptr; });
    start.gain = 1.0f;
    MP_CHECK(none.startVoice(start, 1) < 0,
             "a voice whose audio is missing does not start");
    MP_CHECK(none.stats().samplesMissing == 1, "the missing sample is counted");
    std::fill(buf.begin(), buf.end(), 0.0f);
    none.render(out, 1, 512);
    MP_CHECK(voicetest::rms(buf) == 0.0, "a missing sample renders silence");
  }
};

class VoiceEngineLifecycleTest final : public mp::test::Test {
public:
  VoiceEngineLifecycleTest()
    : Test("functional.voice.lifecycle", Category::Functional) {}
  void run() override {
    voicetest::Fixture fx;
    mp::VoiceEngine eng;
    eng.prepare(48000.0, 16, 1);
    eng.setSampleProvider(fx.provider());

    mp::VoiceStart start;
    start.pipe = &fx.pipe;
    start.layer = &fx.pipe.layers[0];
    start.velocity = 90;
    const int slot = eng.startVoice(start, 7);
    MP_CHECK(slot >= 0, "voice started");

    std::vector<float> buf(4096, 0.0f);
    float* out[1] = {buf.data()};

    // Hold well past the end of the attack: the loop must keep it speaking.
    // Without looping the sample would run out after 4800 frames.
    for (int i = 0; i < 20; ++i) {
      std::fill(buf.begin(), buf.end(), 0.0f);
      eng.render(out, 1, 4096);
    }
    MP_CHECK(eng.activeVoiceCount() == 1,
             "a held note keeps sounding via its sustain loop");
    MP_CHECK(voicetest::rms(buf) > 0.1, "the looped note is still audible");
    MP_CHECK(eng.voice(slot).phase == mp::VoicePhase::Loop,
             "the voice reached its loop phase");

    // Key off: the voice must switch to the release sample, then decay away.
    mp::NoteRelease rel;
    rel.velocity = 64;
    eng.noteOff(7, rel);
    MP_CHECK(eng.voice(slot).phase == mp::VoicePhase::Release,
             "note-off puts the voice into release");
    MP_CHECK(eng.voice(slot).sampleId == 2,
             "the release sample replaces the attack");
    MP_CHECK(eng.voice(slot).cursor == 0.0,
             "the release plays from its head");

    bool decayed = false;
    for (int i = 0; i < 40 && !decayed; ++i) {
      std::fill(buf.begin(), buf.end(), 0.0f);
      eng.render(out, 1, 4096);
      decayed = eng.activeVoiceCount() == 0;
    }
    MP_CHECK(decayed, "a released voice eventually frees its slot");

    // A layer may override the file's own loop: the same sample can be shared
    // by layers that loop it differently, so the loop belongs to the voice.
    {
      voicetest::Fixture ov;
      // The synthesised attack loops over its second half (2400..4800).
      mp::VoiceEngine e;
      e.prepare(48000.0, 4, 1);
      e.setSampleProvider(ov.provider());
      mp::VoiceStart s3;
      s3.pipe = &ov.pipe;
      s3.layer = &ov.pipe.layers[0];
      s3.loopStartOverride = 100;
      s3.loopEndOverride = 300;
      const int i3 = e.startVoice(s3, 5);
      MP_CHECK(i3 >= 0, "voice with a loop override started");
      MP_CHECK(e.voice(i3).loopStart == 100 && e.voice(i3).loopEnd == 300,
               "the declared loop overrides the file's own");

      // An override that does not fit the audio must be ignored, not clamped:
      // a clamped loop invents a pitch.
      mp::VoiceStart s4 = s3;
      s4.loopStartOverride = 10;
      s4.loopEndOverride = 999999;
      const int i4 = e.startVoice(s4, 6);
      MP_CHECK(i4 >= 0, "voice with an impossible override still starts");
      MP_CHECK(e.voice(i4).loopStart == ov.attack.loopStart,
               "an out-of-range override falls back to the file's loop");
    }

    // A one-shot must ignore the file's loop entirely. "-1 means use the
    // file's loop" cannot express "do not loop", which is exactly what a
    // noise, a percussive rank and a release all need.
    {
      voicetest::Fixture os;
      mp::VoiceEngine e;
      e.prepare(48000.0, 4, 1);
      e.setSampleProvider(os.provider());
      mp::VoiceStart s6;
      s6.pipe = &os.pipe;
      s6.layer = &os.pipe.layers[0];
      s6.oneShot = true;
      const int i6 = e.startVoice(s6, 9);
      MP_CHECK(i6 >= 0, "one-shot voice started");
      MP_CHECK(!e.voice(i6).loops(),
               "a one-shot ignores the sample's own loop");

      // A one-shot must also override an explicit loop request: the flag is
      // the stronger statement.
      mp::VoiceStart s7 = s6;
      s7.loopStartOverride = 100;
      s7.loopEndOverride = 300;
      const int i7 = e.startVoice(s7, 10);
      MP_CHECK(i7 >= 0 && !e.voice(i7).loops(),
               "one-shot beats an explicit loop override");

      // And it must actually stop: run past the sample length and the slot
      // has to come back.
      std::vector<float> ob(4096, 0.0f);
      float* op[1] = {ob.data()};
      bool freed = false;
      for (int i = 0; i < 8 && !freed; ++i) {
        e.render(op, 1, 4096);
        freed = e.activeVoiceCount() == 0;
      }
      MP_CHECK(freed, "a one-shot ends and frees its voice");
    }

    // A release must never inherit the attack's loop, or it would sustain
    // forever instead of decaying.
    {
      voicetest::Fixture rf;
      mp::VoiceEngine e;
      e.prepare(48000.0, 4, 1);
      e.setSampleProvider(rf.provider());
      mp::VoiceStart s5;
      s5.pipe = &rf.pipe;
      s5.layer = &rf.pipe.layers[0];
      const int i5 = e.startVoice(s5, 8);
      MP_CHECK(i5 >= 0 && e.voice(i5).loops(), "attack is looping");
      mp::NoteRelease r5;
      e.noteOff(8, r5);
      MP_CHECK(!e.voice(i5).loops(), "a release does not loop");
    }

    // A rank with no release sample must fade, not click off.
    voicetest::Fixture bare;
    bare.pipe.layers[0].releases.clear();
    mp::VoiceEngine e2;
    e2.prepare(48000.0, 8, 1);
    e2.setSampleProvider(bare.provider());
    mp::VoiceStart s2;
    s2.pipe = &bare.pipe;
    s2.layer = &bare.pipe.layers[0];
    const int slot2 = e2.startVoice(s2, 3);
    MP_CHECK(slot2 >= 0, "voice started on a rank without releases");
    e2.noteOff(3, rel);
    MP_CHECK(e2.voice(slot2).phase == mp::VoicePhase::Release,
             "a rank without releases still enters release");
    MP_CHECK(e2.activeVoiceCount() == 1,
             "it fades rather than vanishing on the same sample");
  }
};

class VoiceReleaseTailTest final : public mp::test::Test {
 public:
  VoiceReleaseTailTest()
      : Test("functional.voice.release-tail", Category::Functional) {}
  void run() override {
    // A one-second release tail: the room, not the pipe. The old code faded
    // every release to silence in 120 ms, so a tail this long never survived
    // note-off on any real sample set.
    mp::SampleBuffer attack = voicetest::makeTone(440.0, 48000.0, 4800);
    mp::SampleBuffer tail = voicetest::makeTone(440.0, 48000.0, 48000, 1, false);
    mp::Pipe pipe;
    pipe.pipeId = 100;
    pipe.midiNote = 69;
    mp::PipeLayer layer;
    layer.layerId = 1;
    mp::AttackSample a;
    a.id = 11;
    a.sample.sampleId = 1;
    layer.attacks.push_back(a);
    mp::ReleaseSample r;
    r.id = 21;
    r.sample.sampleId = 2;
    layer.releases.push_back(r);
    pipe.layers.push_back(std::move(layer));
    auto provider = [&](mp::Id id) -> const mp::SampleBuffer* {
      if (id == 1) return &attack;
      if (id == 2) return &tail;
      return nullptr;
    };

    mp::VoiceEngine eng;
    eng.prepare(48000.0, 4, 1);
    eng.setSampleProvider(provider);
    mp::VoiceStart start;
    start.pipe = &pipe;
    start.layer = &pipe.layers[0];
    start.velocity = 90;
    start.ratio = 1.0;
    start.gain = 1.0f;
    const int slot = eng.startVoice(start, 7);
    MP_CHECK(slot >= 0, "voice started");

    std::vector<float> buf(4096, 0.0f);
    float* out[1] = {buf.data()};
    for (int i = 0; i < 4; ++i) eng.render(out, 1, 4096);

    mp::NoteRelease rel;
    rel.velocity = 64;
    eng.noteOff(7, rel);
    MP_CHECK(eng.voice(slot).phase == mp::VoicePhase::Release,
             "note-off puts the voice into release");
    MP_CHECK(eng.voice(slot).sampleId == 2,
             "the release sample replaces the attack");

    // 0.3 s after note-off: the old 120 ms fade would have silenced this.
    for (int i = 0; i < 3; ++i) {
      std::fill(buf.begin(), buf.end(), 0.0f);
      eng.render(out, 1, 4096);
    }
    MP_CHECK(voicetest::rms(buf) > 0.1,
             "a real release tail still speaks past the old 120 ms fade");

    // The tail must still end: play to the end of the one-second file and the
    // slot has to come back.
    bool freed = false;
    for (int i = 0; i < 40 && !freed; ++i) {
      eng.render(out, 1, 4096);
      freed = eng.activeVoiceCount() == 0;
    }
    MP_CHECK(freed, "a finished tail frees its voice");
  }
};

class VoiceReleaseCrossfadeTest final : public mp::test::Test {
 public:
  VoiceReleaseCrossfadeTest()
      : Test("functional.voice.release-crossfade", Category::Functional) {}
  void run() override {
    // Opposed DC levels: attack holds +0.5, the tail is recorded at -0.5. A
    // hard swap steps a full 1.0 in one frame (the click); the overlap
    // crossfade must walk it linearly over the authored 28 ms instead, and
    // the tail must then speak at its full recorded level.
    mp::SampleBuffer attack = voicetest::makeTone(440.0, 48000.0, 4800);
    for (auto& s : attack.frames) s = 0.5f;
    mp::SampleBuffer tail = voicetest::makeTone(440.0, 48000.0, 48000, 1, false);
    for (auto& s : tail.frames) s = -0.5f;
    mp::Pipe pipe;
    pipe.pipeId = 100;
    pipe.midiNote = 69;
    mp::PipeLayer layer;
    layer.layerId = 1;
    mp::AttackSample a;
    a.id = 11;
    a.sample.sampleId = 1;
    layer.attacks.push_back(a);
    mp::ReleaseSample r;
    r.id = 21;
    r.sample.sampleId = 2;
    r.releaseCrossfadeMs = 28.0; // what Nancy authors on its releases
    layer.releases.push_back(r);
    pipe.layers.push_back(std::move(layer));
    auto provider = [&](mp::Id id) -> const mp::SampleBuffer* {
      if (id == 1) return &attack;
      if (id == 2) return &tail;
      return nullptr;
    };

    mp::VoiceEngine eng;
    eng.prepare(48000.0, 4, 1);
    eng.setSampleProvider(provider);
    mp::VoiceStart start;
    start.pipe = &pipe;
    start.layer = &pipe.layers[0];
    start.velocity = 90;
    start.ratio = 1.0;
    start.gain = 1.0f;
    MP_CHECK(eng.startVoice(start, 7) >= 0, "voice started");

    std::vector<float> buf(4096, 0.0f);
    float* out[1] = {buf.data()};
    for (int i = 0; i < 4; ++i) eng.render(out, 1, 4096);

    mp::NoteRelease rel;
    rel.velocity = 64;
    eng.noteOff(7, rel);

    std::fill(buf.begin(), buf.end(), 0.0f);
    eng.render(out, 1, 4096);
    double worst = 0.0;
    for (size_t i = 1; i < 1408; ++i)
      worst = std::max(
          worst, static_cast<double>(std::fabs(buf[i] - buf[i - 1])));
    MP_CHECK(worst < 0.05,
             "the 28 ms crossfade walks the 1.0 level step instead of jumping "
             "it");
    double mean = 0.0;
    for (size_t i = 2048; i < buf.size(); ++i) mean += buf[i];
    mean /= static_cast<double>(buf.size() - 2048);
    MP_CHECK(std::fabs(mean + 0.5) < 0.05,
             "past the crossfade the tail speaks at its recorded level");
  }
};

// A key struck again while it is still down.
//
// The processor keeps one note id per key and hands it to the voice engine.
// Striking the same key twice without a release used to overwrite that id,
// which orphaned the first note's voices: nothing held their handle any more,
// so no note-off could ever reach them, and an organ pipe has no decay to hide
// it. A piece full of repeated notes silted up as it played.
//
// Tested at the voice engine, because that is the JUCE-free half and the
// invariant belongs there: releasing a note id must stop every voice started
// under it, and a second note id must not strand the first.
class VoiceRetriggerTest final : public mp::test::Test {
public:
  VoiceRetriggerTest()
      : Test("functional.voice.retrigger", Category::Functional) {}

  void run() override {
    voicetest::Fixture fx;
    mp::VoiceEngine eng;
    eng.prepare(48000.0, 16, 4);
    eng.setSampleProvider(fx.provider());

    mp::VoiceStart start;
    start.pipe = &fx.pipe;
    start.layer = &fx.pipe.layers[0];
    start.velocity = 90;

    MP_CHECK(eng.startVoice(start, 100) >= 0, "first strike sounds");
    MP_CHECK(eng.startVoice(start, 101) >= 0, "second strike sounds");
    MP_CHECK(eng.activeVoiceCount() == 2, "both are running");

    std::vector<float> buf(4096, 0.0f);
    float* out[1] = {buf.data()};
    eng.render(out, 1, 4096);

    // Release only the SECOND id, which is what the old code could still
    // reach. The first must still be running -- that is the orphan.
    eng.noteOff(101, mp::NoteRelease{});
    for (int i = 0; i < 40; ++i) {
      std::fill(buf.begin(), buf.end(), 0.0f);
      eng.render(out, 1, 4096);
    }
    MP_CHECK(eng.activeVoiceCount() >= 1,
             "releasing one id leaves the other sounding -- an orphan, if "
             "nothing holds its id");

    // Releasing the first id must stop it. If this fails the engine has lost
    // track of the note, and no amount of care in the caller can recover it.
    eng.noteOff(100, mp::NoteRelease{});
    for (int i = 0; i < 200; ++i) {
      std::fill(buf.begin(), buf.end(), 0.0f);
      eng.render(out, 1, 4096);
    }
    MP_CHECK(eng.activeVoiceCount() == 0,
             "every voice stops once its own id is released");
  }
};

class VoiceEngineStealingTest final : public mp::test::Test {
public:
  VoiceEngineStealingTest()
    : Test("functional.voice.stealing", Category::Functional) {}
  void run() override {
    voicetest::Fixture fx;
    mp::VoiceEngine eng;
    eng.prepare(48000.0, 4, 1); // deliberately tiny pool
    eng.setSampleProvider(fx.provider());

    mp::VoiceStart start;
    start.pipe = &fx.pipe;
    start.layer = &fx.pipe.layers[0];

    std::vector<float> buf(64, 0.0f);
    float* out[1] = {buf.data()};

    // Fill the pool, advancing a block between each so ages differ.
    for (int i = 0; i < 4; ++i) {
      MP_CHECK(eng.startVoice(start, static_cast<uint64_t>(i + 1)) >= 0,
               "the pool fills without stealing");
      eng.render(out, 1, 64);
    }
    MP_CHECK(eng.activeVoiceCount() == 4, "pool is full");
    MP_CHECK(eng.stats().stolenVoices == 0, "nothing was stolen while free");

    // Release the oldest, then demand another voice: a decaying voice is the
    // cheapest thing to cut, so that is what must go.
    mp::NoteRelease rel;
    eng.noteOff(1, rel);
    const int reused = eng.startVoice(start, 99);
    MP_CHECK(reused >= 0, "a new note still sounds when the pool is full");
    MP_CHECK(eng.stats().stolenVoices == 1, "exactly one voice was stolen");
    MP_CHECK(eng.voice(reused).noteId == 99,
             "the stolen slot now carries the new note");

    // Every remaining voice must still be one of the notes we started; the
    // engine must never leave a slot half-initialised.
    for (int i = 0; i < eng.poolSize(); ++i) {
      const auto& v = eng.voice(i);
      if (!v.active()) continue;
      MP_CHECK(v.buffer != nullptr, "an active voice always has a backing store");
      MP_CHECK(v.noteId != 0, "an active voice always belongs to a note");
    }

    // A pool of one, all voices brand new this block: the engine must refuse
    // rather than steal the note the player just pressed.
    mp::VoiceEngine tiny;
    tiny.prepare(48000.0, 1, 1);
    tiny.setSampleProvider(fx.provider());
    MP_CHECK(tiny.startVoice(start, 1) >= 0, "first note fits");
    MP_CHECK(tiny.startVoice(start, 2) < 0,
             "a voice started this block is never stolen");
    MP_CHECK(tiny.stats().startsDropped == 1, "the dropped start is counted");
  }
};

// Expression must be per rank, not per organ: the whole point of a swell box
// is that it covers SOME pipework. A voice carries the bus of the enclosure
// that encloses its pipe, and the engine can render one bus at a time.
// Multi-threaded rendering must be an optimisation, not a behaviour change.
// Note what that can and cannot mean: parallel partial sums group float
// additions differently from a sequential pass, so the threaded path is NOT
// bit-identical to the inline one and asserting that would be wrong. The two
// real guarantees are that it is numerically equivalent (differences at the
// noise floor, not audible), and that it is DETERMINISTIC — the same voices
// give the same samples every run, whatever order the workers finish in.
// The renderer has two interpolation paths: a checked one for the first and
// last few frames of a buffer, and an unchecked interior one that skips the
// per-tap bounds tests (worth ~1.7x single-threaded, since the inner loop is
// check-bound rather than compute-bound). They must agree, or a voice would
// glitch exactly at the seam between them — a defect that would be inaudible
// in a benchmark and obvious in a chord.
// The resample ratio for a pipe is target pitch over the pitch the FILE holds,
// not over the organ's reference A. Getting that wrong is catastrophic and
// silent in every unit test that looks at one note: each pipe of a real rank
// is recorded from its own pipe, so every ratio should sit near 1.0. Using the
// reference pitch instead gives 0.59 at middle C and 1.0 only at A — the rank
// collapses toward a single pitch. This test pins the whole rank.
// A loop point where the two ends do not match phase clicks on every wrap.
// LoopCrossfadeLengthInSrcSampleMs is Hauptwerk's answer, and this checks that
// applying it actually removes the discontinuity rather than merely running.
class LoopCrossfadeTest final : public mp::test::Test {
public:
  LoopCrossfadeTest()
    : Test("functional.voice.loop-crossfade", Category::Functional) {}

  // Largest sample-to-sample jump in a rendered block: a click shows up here
  // as a spike far above what the waveform itself ever does.
  static double maxStep(const std::vector<float>& v) {
    double worst = 0.0;
    for (size_t i = 1; i < v.size(); ++i)
      worst = std::max(worst, std::fabs(static_cast<double>(v[i] - v[i - 1])));
    return worst;
  }

  // A buffer whose loop deliberately does NOT join cleanly: the loop end sits
  // at a peak and the loop start at a trough, so a hard wrap steps hard.
  static mp::SampleBuffer discontinuous() {
    constexpr int kFrames = 4000;
    mp::SampleBuffer b;
    b.numChannels = 1;
    b.sampleRate = 48000.0;
    b.numFrames = kFrames;
    b.frames.resize(kFrames);
    for (int i = 0; i < kFrames; ++i)
      b.frames[static_cast<size_t>(i)] =
          static_cast<float>(std::sin(2.0 * 3.141592653589793 * i / 400.0));
    // 400-frame period; start at a zero-crossing going up, end a quarter
    // period later, at the peak. Wrapping there is a full-amplitude jump.
    b.loopStart = 800;   // sin = 0, rising
    b.loopEnd = 1900;    // sin = +1, peak
    return b;
  }

  void run() override {
    voicetest::Fixture fx;
    fx.attack = discontinuous();

    auto renderWith = [&](int crossfadeFrames) {
      mp::VoiceEngine eng;
      eng.prepare(48000.0, 4, 1, 4096);
      eng.setSampleProvider(fx.provider());
      mp::VoiceStart st;
      st.pipe = &fx.pipe;
      st.layer = &fx.pipe.layers[0];
      st.loopCrossfadeFrames = crossfadeFrames;
      MP_CHECK(eng.startVoice(st, 1) >= 0, "voice started");
      // Long enough to wrap several times.
      std::vector<float> buf(4096, 0.0f);
      float* out[1] = {buf.data()};
      std::vector<float> all;
      for (int b = 0; b < 3; ++b) {
        std::fill(buf.begin(), buf.end(), 0.0f);
        eng.render(out, 1, 4096);
        all.insert(all.end(), buf.begin(), buf.end());
      }
      return all;
    };

    const auto hard = renderWith(0);
    const auto faded = renderWith(256);

    // The sine itself steps by at most ~2*pi/400 per frame; a hard wrap at a
    // peak steps by ~1.0. So the hard render must show a big jump...
    MP_CHECK(maxStep(hard) > 0.3, "a mismatched loop clicks without crossfade");
    // ...and the crossfaded one must not.
    MP_CHECK(maxStep(faded) < 0.1,
             "the loop crossfade removes the discontinuity");
    MP_CHECK(maxStep(faded) < maxStep(hard) * 0.5,
             "crossfading is a large improvement, not a rounding difference");

    // The blend must not dip or bulge the level: an equal-gain fade between
    // two correlated streams should hold amplitude across the wrap.
    double peak = 0.0;
    for (float x : faded) peak = std::max(peak, std::fabs((double)x));
    MP_CHECK(peak > 0.8 && peak < 1.3,
             "the crossfade holds level rather than dipping or bulging");
    for (float x : faded) MP_CHECK(std::isfinite(x), "crossfade stays finite");

    // A crossfade longer than the material after the loop end must degrade
    // gracefully: blend what exists, then stop, never read past the buffer.
    const auto huge = renderWith(100000);
    for (float x : huge)
      MP_CHECK(std::isfinite(x) && std::fabs(x) < 4.0,
               "an oversized crossfade never reads past the sample");

    // A one-shot must ignore the crossfade setting entirely.
    mp::VoiceEngine os;
    os.prepare(48000.0, 4, 1, 512);
    os.setSampleProvider(fx.provider());
    mp::VoiceStart s2;
    s2.pipe = &fx.pipe;
    s2.layer = &fx.pipe.layers[0];
    s2.oneShot = true;
    s2.loopCrossfadeFrames = 256;
    const int idx = os.startVoice(s2, 1);
    MP_CHECK(idx >= 0 && !os.voice(idx).loops(),
             "a one-shot with a crossfade set still does not loop");
  }
};

// Detuning: an organ drifting out of tune, pipe by pipe. A set declares a
// control per division and zone and a sensitivity per pipe, so the same
// request moves every pipe by a slightly different amount — which is the
// point, and is why it cannot be a single global offset.
class DetuningTest final : public mp::test::Test {
public:
  DetuningTest() : Test("functional.voice.detuning", Category::Functional) {}

  void run() override {
    const double a440 = 440.0;
    // A 0..127 control: the middle of its travel is where "in tune" sits.
    const double mid = 63.5;

    // At rest the organ must be exactly in tune. This is the assertion that
    // matters most: a real set declares every sensitivity POSITIVE and then
    // points half its layers at a "DetPos" control and half at a "DetNeg"
    // one, so reading the offset from zero rather than from centre leaves
    // every pipe sharp together — a whole organ about ten cents high while
    // its own settings page reports no detuning at all.
    MP_CHECK(mp::detunedTargetHz(a440, 63, mid, 0.795) < a440,
             "below centre goes flat, however large the sensitivity");
    MP_CHECK(mp::detunedTargetHz(a440, 64, mid, 0.795) > a440,
             "and above centre goes sharp");
    MP_CHECK(std::abs(mp::detunedTargetHz(a440, 63, mid, 0.795) - a440) < 0.5,
             "and a control resting at centre is within half a hertz of true");
    MP_CHECK(mp::detunedTargetHz(a440, 64, mid, 0.0) == a440,
             "a layer with no sensitivity is untouched wherever the control is");

    // offsetHz = (value - centre) * sensitivity, symmetric about the middle.
    // A 0..127 control has a half-integer centre, so the reachable values
    // either side of it are 49.5 units out, not 50.
    MP_CHECK(std::abs(mp::detunedTargetHz(a440, 113, mid, 0.02) - 440.99) < 1e-9,
             "above centre the pitch rises by the distance times sensitivity");
    MP_CHECK(std::abs(mp::detunedTargetHz(a440, 14, mid, 0.02) - 439.01) < 1e-9,
             "and the same distance below lowers it by the same amount");

    // The Pos/Neg pair is what makes a chorus: one control up and its partner
    // down spreads pipes either side of true pitch rather than moving them
    // all the same way.
    const double sharp = mp::detunedTargetHz(a440, 127, mid, 0.04);
    const double flat = mp::detunedTargetHz(a440, 0, mid, 0.04);
    MP_CHECK(sharp > a440 && flat < a440,
             "a Pos/Neg pair at opposite ends straddles true pitch");
    MP_CHECK(std::abs((sharp - a440) - (a440 - flat)) < 1e-6,
             "and straddles it evenly");

    // The audible size of it: single-figure cents, which is drift, not a
    // transposition.
    const double cents = 1200.0 * std::log2(sharp / a440);
    MP_CHECK(cents > 1.0 && cents < 25.0,
             "full detuning is a handful of cents, the size of real drift");

    // Per-pipe sensitivity is the whole mechanism: the same control must move
    // two pipes by different amounts, or a rank detunes as a block and sounds
    // transposed rather than untuned.
    MP_CHECK(mp::detunedTargetHz(a440, 127, mid, 0.0035) !=
                 mp::detunedTargetHz(a440, 127, mid, 0.0219),
             "one control moves two pipes differently, per their own rates");

    // A pathological sensitivity must not invert or octave-drop a rank.
    MP_CHECK(mp::detunedTargetHz(a440, 0, mid, 90.0) >= a440 * 0.5,
             "an absurd sensitivity is clamped rather than obeyed");
    MP_CHECK(mp::detunedTargetHz(a440, 0, mid, 90.0) > 0.0,
             "and can never reach zero or go negative");

    // It has to survive the ratio: a detuned target against an undetuned
    // recording is exactly what resampling has to express.
    const double ratio = mp::playbackRatio(sharp, a440);
    MP_CHECK(ratio > 1.0 && ratio < 1.02,
             "and reaches the resample ratio as a slight sharpening");
  }
};

class RankPitchRatioTest final : public mp::test::Test {
public:
  RankPitchRatioTest()
    : Test("functional.voice.rank-pitch", Category::Functional) {}

  // Concert pitch of a MIDI note.
  static double noteHz(int midi) {
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
  }

  void run() override {
    const mp::Temperament equal{"Equal", std::vector<double>(12, 0.0)};

    // A 61-note 8' rank, each pipe recorded at its own pitch, as a real
    // sample set is. Every ratio must be ~1.0.
    for (int note = 36; note <= 96; ++note) {
      const double target = mp::pipeTargetHz(note, 8, 440.0, 0.0, equal, 0);
      const double recorded = noteHz(note); // sampled from that very pipe
      const double ratio = mp::playbackRatio(target, recorded);
      MP_CHECK(std::fabs(ratio - 1.0) < 1e-9,
               "a pipe recorded at its own pitch plays at unity");
    }

    // The old bug, stated explicitly so it cannot come back: target over the
    // organ's reference pitch is NOT the resample ratio, and differs wildly
    // from 1.0 away from A.
    const double wrong = mp::temperedPlaybackRatio(60, 8, 440.0, 0.0, equal, 0);
    MP_CHECK(std::fabs(wrong - 1.0) > 0.3,
             "target-over-reference is not a resample ratio (this is the trap)");

    // A transposed rank: a 4' rank (64ft harmonic 16) sounds an octave above
    // its key, so a pipe recorded at its written pitch plays at ratio 2.
    for (int note = 48; note <= 72; ++note) {
      const double target = mp::pipeTargetHz(note, 16, 440.0, 0.0, equal, 0);
      const double ratio = mp::playbackRatio(target, noteHz(note));
      MP_CHECK(std::fabs(ratio - 2.0) < 1e-9,
               "a 4' rank sounds an octave above the key it is played from");
    }

    // Temperament trims the ratio slightly, and only slightly: a well
    // temperament must never move a pipe by more than a few cents.
    const auto* vallotti = mp::findTemperament("Vallotti");
    MP_CHECK(vallotti != nullptr, "Vallotti available");
    for (int note = 60; note < 72; ++note) {
      const double target = mp::pipeTargetHz(note, 8, 440.0, 0.0, *vallotti, 0);
      const double ratio = mp::playbackRatio(target, noteHz(note));
      const double cents = 1200.0 * std::log2(ratio);
      MP_CHECK(std::fabs(cents) < 15.0,
               "a well temperament retunes a pipe by cents, not semitones");
    }

    // A sample tagged with a MIDI note rather than an exact pitch must give
    // the same answer, since that tag is written at concert pitch.
    const double taggedRatio =
        mp::playbackRatio(mp::pipeTargetHz(60, 8, 440.0, 0.0, equal, 0),
                          noteHz(60));
    MP_CHECK(std::fabs(taggedRatio - 1.0) < 1e-9,
             "a MIDI-note tag resolves to the same pitch as an exact one");

    // An organ tuned sharp of concert pitch shifts the whole rank together.
    const double sharpTarget = mp::pipeTargetHz(60, 8, 465.0, 0.0, equal, 0);
    const double sharpRatio = mp::playbackRatio(sharpTarget, noteHz(60));
    MP_CHECK(std::fabs(sharpRatio - 465.0 / 440.0) < 1e-9,
             "a sharp-tuned organ raises every pipe by the same ratio");
  }
};

// The tremulant, at the voice. The LFO itself has its own test; this is about
// whether a voice actually wobbles — the LFOs were being built at load and
// then never asked for a sample, so every tremulant in the program was inert.
//
// A tremulant swings the level AND the pitch, by an amount the organ states
// per pipe, and it has to be smooth: the modulation is delivered as a ramp
// across each block rather than one value per block, because at six hertz and
// a 256-frame block one value a block is thirty steps a cycle and steps.
class TremulantVoiceTest final : public mp::test::Test {
public:
  TremulantVoiceTest()
    : Test("functional.voice.tremulant", Category::Functional) {}

  static std::vector<float> render(const mp::VoiceEngine::TremMod* mods,
                                   int count, float ampDepth,
                                   double pitchDepth, int frames) {
    voicetest::Fixture fx;
    mp::VoiceEngine eng;
    eng.prepare(48000.0, 4, 1, frames);
    eng.setSampleProvider(fx.provider());
    eng.setTremMods(mods, count);
    mp::VoiceStart st;
    st.pipe = &fx.pipe;
    st.layer = &fx.pipe.layers[0];
    st.ratio = 1.0;
    st.gain = 1.0f;
    if (mods != nullptr) {
      st.tremIndex = 0;
      st.tremAmpDepth = ampDepth;
      st.tremPitchDepth = pitchDepth;
    }
    eng.startVoice(st, 1);
    std::vector<float> out(static_cast<size_t>(frames), 0.0f);
    float* ptr[1] = {out.data()};
    eng.render(ptr, 1, frames);
    return out;
  }

  static double rms(const std::vector<float>& v) {
    double acc = 0.0;
    for (float x : v) acc += static_cast<double>(x) * x;
    return std::sqrt(acc / static_cast<double>(v.size()));
  }

  void run() override {
    constexpr int kFrames = 480;

    // --- no tremulant ------------------------------------------------------
    const auto dry = render(nullptr, 0, 0.0f, 0.0, kFrames);
    MP_CHECK(rms(dry) > 0.1, "the voice sounds at all");

    // --- a tremulant at full swing, held ----------------------------------
    // A steady +1 is the top of the swing: a 30% depth must make it audibly
    // louder, not merely different.
    // The top of the swing, held. Both fields are the same signed value: the
    // voice is what turns it into a gain and a pitch.
    mp::VoiceEngine::TremMod top;
    top.ampStart = 1.0f;
    top.pitchStart = 1.0;
    const auto loud = render(&top, 1, 0.3f, 0.0, kFrames);
    MP_CHECK(rms(loud) > rms(dry) * 1.25,
             "at the top of its swing a tremulant makes the pipe louder");

    mp::VoiceEngine::TremMod bottom;
    bottom.ampStart = -1.0f;
    bottom.pitchStart = -1.0;
    const auto quiet = render(&bottom, 1, 0.3f, 0.0, kFrames);
    MP_CHECK(rms(quiet) < rms(dry) * 0.8,
             "and at the bottom, quieter");

    // --- a tremulant with no depth does nothing ---------------------------
    const auto flat = render(&top, 1, 0.0f, 0.0, kFrames);
    MP_CHECK(std::fabs(rms(flat) - rms(dry)) < 1e-6,
             "a pipe the tremulant does not reach is untouched, which is how "
             "one chest can wobble while another does not");

    // --- and an unset index is the same as no tremulant --------------------
    voicetest::Fixture fx;
    mp::VoiceEngine eng;
    eng.prepare(48000.0, 4, 1, kFrames);
    eng.setSampleProvider(fx.provider());
    eng.setTremMods(&top, 1);
    mp::VoiceStart st;
    st.pipe = &fx.pipe;
    st.layer = &fx.pipe.layers[0];
    st.ratio = 1.0;
    st.gain = 1.0f;
    st.tremIndex = -1; // this pipe has no tremulant
    eng.startVoice(st, 1);
    std::vector<float> untouched(kFrames, 0.0f);
    float* p2[1] = {untouched.data()};
    eng.render(p2, 1, kFrames);
    MP_CHECK(std::fabs(rms(untouched) - rms(dry)) < 1e-6,
             "and a voice with no tremulant index ignores the table entirely");

    // --- the ramp is smooth ------------------------------------------------
    // Swing right across the block. Delivered as one value a block this would
    // be a step; delivered as a ramp the output has no discontinuity in it.
    mp::VoiceEngine::TremMod sweep;
    sweep.ampStart = -1.0f;
    sweep.ampStep = 2.0f / static_cast<float>(kFrames - 1);
    sweep.pitchStart = -1.0;
    sweep.pitchStep = 2.0 / static_cast<double>(kFrames - 1);
    const auto ramped = render(&sweep, 1, 0.5f, 0.0, kFrames);
    // Compare against the same voice with no tremulant: the ratio of the two
    // must climb monotonically from about 0.5 to about 1.5, with no jump.
    double worstJump = 0.0, prev = -1.0;
    for (size_t i = 0; i < ramped.size(); ++i) {
      if (std::fabs(dry[i]) < 0.05) continue; // ratios are noise near a zero
      const double r = static_cast<double>(ramped[i]) / dry[i];
      if (prev >= 0.0) worstJump = std::max(worstJump, std::fabs(r - prev));
      prev = r;
    }
    MP_CHECK(worstJump < 0.02,
             "the modulation moves smoothly across the block rather than "
             "stepping, which is the whole reason it is a ramp");

    // --- pitch -------------------------------------------------------------
    // A pitch swing moves the cursor, so the same number of frames covers a
    // different amount of the sample. Read at the top and the bottom of the
    // swing and the two must differ.
    const auto sharp = render(&top, 1, 0.0f, 1.0, kFrames);    // +1 semitone
    const auto flatP = render(&bottom, 1, 0.0f, 1.0, kFrames); // -1 semitone
    double diff = 0.0;
    for (size_t i = 0; i < sharp.size(); ++i)
      diff = std::max(diff, static_cast<double>(std::fabs(sharp[i] - flatP[i])));
    MP_CHECK(diff > 0.05,
             "a tremulant moves the pitch as well as the level - a wobble that "
             "is only amplitude sounds like a volume pedal, not like wind");
  }
};

// Streaming. Only the head of a release is held; the rest arrives from a
// background thread while the voice is playing the part that is resident.
//
// The thing worth proving is not that it plays something — it is that it plays
// EXACTLY what the preloaded path plays. Per ADR-004 there is one voice path,
// and streaming differs only in where the frames came from, so a streamed
// render that is anything other than identical means the seam is audible.
class StreamingTest final : public mp::test::Test {
public:
  StreamingTest() : Test("functional.samples.streaming", Category::Functional) {}

  // A "disk": the whole sample, served on request. Counts its reads, so the
  // test can tell a streamer that works from one that never ran.
  struct FakeDisk {
    std::vector<float> all;
    int channels = 1;
    std::atomic<int> reads{0};
  };

  static mp::SampleBuffer headOf(const mp::SampleBuffer& whole, int64_t head,
                                 std::shared_ptr<FakeDisk> disk) {
    mp::SampleBuffer out;
    out.numChannels = whole.numChannels;
    out.sampleRate = whole.sampleRate;
    out.numFrames = std::min(head, whole.numFrames);
    out.frames.assign(whole.frames.begin(),
                      whole.frames.begin() + static_cast<size_t>(out.numFrames) *
                                                 static_cast<size_t>(out.numChannels));
    auto tail = std::make_shared<mp::SampleTail>();
    tail->totalFrames = whole.numFrames;
    tail->read = [disk](int64_t start, int n, float* dest, int ch) -> int64_t {
      disk->reads.fetch_add(1, std::memory_order_relaxed);
      int64_t written = 0;
      for (int f = 0; f < n; ++f) {
        const int64_t src = start + f;
        for (int c = 0; c < ch; ++c) {
          const size_t idx = static_cast<size_t>(src) *
                                 static_cast<size_t>(disk->channels) +
                             static_cast<size_t>(std::min(c, disk->channels - 1));
          dest[static_cast<size_t>(f) * static_cast<size_t>(ch) +
               static_cast<size_t>(c)] =
              idx < disk->all.size() ? disk->all[idx] : 0.0f;
        }
        ++written;
      }
      return written;
    };
    out.tail = std::move(tail);
    return out;
  }

  // Render one voice of `buf` for `frames` output samples.
  static std::vector<float> render(const mp::SampleBuffer& buf, int frames,
                                   double settleMs) {
    voicetest::Fixture fx;
    fx.attack = buf;
    mp::VoiceEngine eng;
    eng.prepare(48000.0, 4, 1, frames);
    eng.setSampleProvider(fx.provider());
    mp::VoiceStart st;
    st.pipe = &fx.pipe;
    st.layer = &fx.pipe.layers[0];
    st.ratio = 1.0;
    st.gain = 1.0f;
    st.oneShot = true; // a release: straight through, no loop
    eng.startVoice(st, 1);
    // Give the streamer a moment to run ahead, exactly as it has in the
    // seconds between a note-on and the voice reaching the end of the head.
    if (settleMs > 0.0)
      std::this_thread::sleep_for(
          std::chrono::milliseconds(static_cast<int>(settleMs)));
    std::vector<float> out(static_cast<size_t>(frames), 0.0f);
    float* ptr[1] = {out.data()};
    eng.render(ptr, 1, frames);
    return out;
  }

  void run() override {
    constexpr int kFrames = 8000;
    // A one-shot: no loop, so the voice walks straight off the end of the head
    // and into the streamed part.
    auto whole = voicetest::makeTone(220.0, 48000.0, kFrames, 1, false);

    auto disk = std::make_shared<FakeDisk>();
    disk->all = whole.frames;
    disk->channels = 1;

    // A head far shorter than the sample: most of what is played is streamed.
    const auto streamed = headOf(whole, 1000, disk);
    MP_CHECK(streamed.streams() && streamed.numFrames == 1000,
             "only the head is resident");
    MP_CHECK(streamed.totalFrames() == kFrames,
             "but the buffer knows how long the whole sample is");
    MP_CHECK(!whole.streams(), "and a fully loaded sample streams nothing");

    const auto plain = render(whole, kFrames - 8, 0.0);
    const auto viaDisk = render(streamed, kFrames - 8, 120.0);

    MP_CHECK(disk->reads.load() > 0, "the streamer actually read something");

    // The whole point: identical, not merely similar.
    double worst = 0.0;
    for (size_t i = 0; i < plain.size(); ++i)
      worst = std::max(worst, static_cast<double>(std::fabs(plain[i] - viaDisk[i])));
    MP_CHECK(worst < 1e-6,
             "a streamed render is the same render - one voice path, and the "
             "frames only came from somewhere else");

    // And the resident part alone is not enough to produce it, or the test
    // above would pass without the streamer doing anything at all.
    auto truncated = whole;
    truncated.numFrames = 1000;
    truncated.frames.resize(1000);
    const auto headOnly = render(truncated, kFrames - 8, 0.0);
    double energyAfterHead = 0.0;
    for (size_t i = 2000; i < headOnly.size(); ++i)
      energyAfterHead += static_cast<double>(headOnly[i]) * headOnly[i];
    MP_CHECK(energyAfterHead < 1e-9,
             "the head on its own goes silent past its end, so the match above "
             "could only have come from the streamed frames");
  }
};

// Holding samples as 16-bit halves what an organ costs to keep resident, and
// it is the difference between a large set fitting in a machine and not. The
// engine must render the compact form through the same voice path, at the same
// pitch and level, differing only by the quantisation itself.
class CompactStorageTest final : public mp::test::Test {
public:
  CompactStorageTest()
    : Test("functional.samples.compact-storage", Category::Functional) {}

  // The same audio, quantised the way SampleLibrary quantises it: scaled by
  // the file's own peak first, so a quiet sample still uses all sixteen bits.
  static mp::SampleBuffer toInt16(const mp::SampleBuffer& src) {
    mp::SampleBuffer out;
    out.numChannels = src.numChannels;
    out.sampleRate = src.sampleRate;
    out.numFrames = src.numFrames;
    out.loopStart = src.loopStart;
    out.loopEnd = src.loopEnd;
    float peak = 0.0f;
    for (float v : src.frames) peak = std::max(peak, std::fabs(v));
    out.pcmScale = peak > 0.0f ? peak / 32767.0f : 1.0f;
    const float toCounts = peak > 0.0f ? 32767.0f / peak : 0.0f;
    out.pcm16.reserve(src.frames.size());
    for (float v : src.frames)
      out.pcm16.push_back(static_cast<int16_t>(
          std::clamp(std::round(v * toCounts), -32767.0f, 32767.0f)));
    return out;
  }

  static double renderOne(const mp::SampleBuffer& buf, double ratio,
                          std::vector<float>& out) {
    constexpr double kSr = 48000.0;
    voicetest::Fixture fx;
    fx.attack = buf;
    mp::VoiceEngine eng;
    eng.prepare(kSr, 4, 1, static_cast<int>(out.size()));
    eng.setSampleProvider(fx.provider());
    mp::VoiceStart st;
    st.pipe = &fx.pipe;
    st.layer = &fx.pipe.layers[0];
    st.ratio = ratio;
    st.gain = 1.0f;
    const bool started = eng.startVoice(st, 1) >= 0;
    float* ptr[1] = {out.data()};
    eng.render(ptr, 1, static_cast<int>(out.size()));
    return started ? 1.0 : 0.0;
  }

  void run() override {
    constexpr double kSr = 48000.0;
    constexpr int kFrames = 2000;
    const auto tone = voicetest::makeTone(440.0, kSr, kFrames, 1, false);
    const auto compact = toInt16(tone);

    MP_CHECK(!tone.compact() && compact.compact(),
             "the two buffers report which format they hold");
    MP_CHECK(compact.residentBytes() * 2 == tone.residentBytes(),
             "sixteen bits is exactly half of thirty-two");

    // The accessor has to return the true value, not the raw count: anything
    // outside the render path reads through it.
    double worstAccessor = 0.0;
    for (int64_t f = 0; f < kFrames; ++f)
      worstAccessor = std::max(
          worstAccessor,
          std::fabs(static_cast<double>(tone.sample(f, 0) - compact.sample(f, 0))));
    MP_CHECK(worstAccessor < 4e-5,
             "reading a compact buffer gives back the sample, not the count");

    // Out of range must still be silence rather than a scaled garbage read.
    MP_CHECK(compact.sample(-1, 0) == 0.0f && compact.sample(kFrames, 0) == 0.0f,
             "a compact buffer is silent outside its own length");

    // The render is what actually matters. Both formats go through the same
    // voice path; only the quantisation may differ, and a 16-bit quantisation
    // of a full-scale sine sits around -96 dBFS.
    for (double ratio : {1.0, 0.5, 1.5}) {
      std::vector<float> a(kFrames, 0.0f), b(kFrames, 0.0f);
      renderOne(tone, ratio, a);
      renderOne(compact, ratio, b);

      double worst = 0.0, energyA = 0.0, energyB = 0.0;
      for (int i = 0; i < kFrames; ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(a[i] - b[i])));
        energyA += static_cast<double>(a[i]) * a[i];
        energyB += static_cast<double>(b[i]) * b[i];
      }
      MP_CHECK(worst < 1e-3,
               "the compact render tracks the float render sample for sample");
      MP_CHECK(energyA > 1.0 && std::fabs(energyA - energyB) / energyA < 1e-3,
               "and carries the same level, so the peak scaling is undone "
               "exactly where it was applied");
    }

    // A quiet sample is the case peak scaling exists for: truncating it
    // straight to int16 would throw away the bits it most needs.
    auto quiet = voicetest::makeTone(440.0, kSr, kFrames, 1, false);
    for (auto& v : quiet.frames) v *= 0.001f; // -60 dBFS
    const auto quietCompact = toInt16(quiet);
    double worstQuiet = 0.0, peakQuiet = 0.0;
    for (int64_t f = 0; f < kFrames; ++f) {
      worstQuiet = std::max(worstQuiet, std::fabs(static_cast<double>(
                                            quiet.sample(f, 0) -
                                            quietCompact.sample(f, 0))));
      peakQuiet = std::max(peakQuiet, std::fabs(static_cast<double>(quiet.sample(f, 0))));
    }
    MP_CHECK(peakQuiet > 0.0 && worstQuiet / peakQuiet < 1e-4,
             "a quiet sample keeps its full resolution, because it is scaled "
             "by its own peak and not by full scale");

    // Silence must not divide by a zero peak.
    mp::SampleBuffer silent;
    silent.numChannels = 1;
    silent.sampleRate = kSr;
    silent.numFrames = 8;
    silent.frames.assign(8, 0.0f);
    const auto silentCompact = toInt16(silent);
    for (int64_t f = 0; f < 8; ++f)
      MP_CHECK(silentCompact.sample(f, 0) == 0.0f,
               "a silent sample quantises to silence, not to a NaN");
  }
};

class VoiceEngineInterpolationTest final : public mp::test::Test {
public:
  VoiceEngineInterpolationTest()
    : Test("functional.voice.interpolation", Category::Functional) {}
  void run() override {
    constexpr double kSr = 48000.0;
    constexpr double kHz = 440.0;
    constexpr int kFrames = 2000;

    // A pure tone with no loop: the voice runs from frame 0 straight off the
    // end, crossing BOTH seams (leading edge, then trailing edge).
    voicetest::Fixture fx;
    fx.attack = voicetest::makeTone(kHz, kSr, kFrames, 1, false);

    mp::VoiceEngine eng;
    eng.prepare(kSr, 4, 1, kFrames);
    eng.setSampleProvider(fx.provider());
    mp::VoiceStart st;
    st.pipe = &fx.pipe;
    st.layer = &fx.pipe.layers[0];
    st.ratio = 1.0;  // exact frame alignment: output must equal the source
    st.gain = 1.0f;
    MP_CHECK(eng.startVoice(st, 1) >= 0, "voice started");

    std::vector<float> out(kFrames, 0.0f);
    float* ptr[1] = {out.data()};
    eng.render(ptr, 1, kFrames);

    // At ratio 1.0 with integral cursors, Hermite reduces to the sample
    // itself, so the render must reproduce the source frame for frame —
    // through both seams. An off-by-one in the interior path shows up here
    // immediately as a one-frame shift.
    int mismatches = 0;
    for (int i = 0; i < kFrames - 1; ++i)
      if (std::fabs(out[i] - fx.attack.frames[static_cast<size_t>(i)]) > 1e-5f)
        ++mismatches;
    MP_CHECK(mismatches == 0,
             "interior and edge interpolation reproduce the source exactly");

    // Fractional cursor: the interpolated signal must stay a clean sine, with
    // no step at the seam where the fast path takes over (frame 1) or hands
    // back (frame numFrames-3).
    voicetest::Fixture fx2;
    fx2.attack = voicetest::makeTone(kHz, kSr, kFrames, 1, false);
    mp::VoiceEngine e2;
    e2.prepare(kSr, 4, 1, kFrames);
    e2.setSampleProvider(fx2.provider());
    mp::VoiceStart s2 = st;
    s2.pipe = &fx2.pipe;
    s2.layer = &fx2.pipe.layers[0];
    s2.ratio = 0.5; // lands halfway between frames, worst case for the taps
    MP_CHECK(e2.startVoice(s2, 1) >= 0, "fractional voice started");

    std::vector<float> frac(kFrames, 0.0f);
    float* fptr[1] = {frac.data()};
    e2.render(fptr, 1, kFrames);

    // Compare against the analytic sine the source was built from. Hermite on
    // an oversampled sine is accurate to well under a percent; a seam bug
    // would blow past this on exactly one or two frames.
    double worst = 0.0;
    for (int i = 2; i < kFrames - 2; ++i) {
      const double t = 0.5 * i; // source position for ratio 0.5
      const double want =
          std::sin(2.0 * 3.141592653589793 * kHz * t / kSr);
      worst = std::max(worst, std::fabs(want - static_cast<double>(frac[i])));
    }
    MP_CHECK(worst < 0.02,
             "fractional interpolation tracks the source across both seams");

    // A one-frame buffer exercises the edge path exclusively: every tap is out
    // of range, and it must not read past the allocation.
    mp::SampleBuffer tiny;
    tiny.numChannels = 1;
    tiny.sampleRate = kSr;
    tiny.numFrames = 1;
    tiny.frames = {0.5f};
    voicetest::Fixture fx3;
    fx3.attack = tiny;
    mp::VoiceEngine e3;
    e3.prepare(kSr, 4, 1, 64);
    e3.setSampleProvider(fx3.provider());
    mp::VoiceStart s3 = st;
    s3.pipe = &fx3.pipe;
    s3.layer = &fx3.pipe.layers[0];
    e3.startVoice(s3, 1);
    std::vector<float> t3(64, 0.0f);
    float* tp[1] = {t3.data()};
    e3.render(tp, 1, 64);
    for (float x : t3)
      MP_CHECK(std::isfinite(x), "a one-frame sample never reads out of bounds");

    // Stereo source into a mono render, and mono source into stereo: the
    // channel clamp lives in both paths and must behave the same either way.
    voicetest::Fixture fx4;
    fx4.attack = voicetest::makeTone(kHz, kSr, 500, 2, false);
    mp::VoiceEngine e4;
    e4.prepare(kSr, 4, 2, 256);
    e4.setSampleProvider(fx4.provider());
    mp::VoiceStart s4 = st;
    s4.pipe = &fx4.pipe;
    s4.layer = &fx4.pipe.layers[0];
    e4.startVoice(s4, 1);
    std::vector<float> L(256, 0.0f), R(256, 0.0f);
    float* st4[2] = {L.data(), R.data()};
    e4.render(st4, 2, 256);
    for (int i = 0; i < 256; ++i)
      MP_CHECK(std::fabs(L[i] - R[i]) < 1e-6f,
               "both channels of a stereo source render identically here");
  }
};

class VoiceEngineThreadingTest final : public mp::test::Test {
public:
  VoiceEngineThreadingTest()
    : Test("functional.voice.threading", Category::Functional) {}

  static void fill(mp::VoiceEngine& e, voicetest::Fixture& fx, int n) {
    for (int i = 0; i < n; ++i) {
      mp::VoiceStart st;
      st.pipe = &fx.pipe;
      st.layer = &fx.pipe.layers[0];
      st.ratio = 1.0 + 0.0007 * (i % 53); // detuned, like a real chord
      st.gain = 0.05f;
      st.busIndex = i % 3;
      e.startVoice(st, static_cast<uint64_t>(i + 1));
    }
  }

  void run() override {
    constexpr int kVoices = 256;
    constexpr int kFrames = 256;

    voicetest::Fixture fxA, fxB;
    mp::VoiceEngine single, multi;
    single.prepare(48000.0, kVoices, 2, kFrames);
    multi.prepare(48000.0, kVoices, 2, kFrames);
    single.setSampleProvider(fxA.provider());
    multi.setSampleProvider(fxB.provider());

    // Four threads, and a threshold low enough that it actually splits.
    multi.setRenderThreads(4, 8);
    MP_CHECK(multi.renderThreads() == 4, "the pool reports its thread count");
    MP_CHECK(single.renderThreads() == 1, "the default is inline rendering");

    fill(single, fxA, kVoices);
    fill(multi, fxB, kVoices);
    MP_CHECK(single.activeVoiceCount() == kVoices, "single-threaded pool full");
    MP_CHECK(multi.activeVoiceCount() == kVoices, "multi-threaded pool full");

    std::vector<float> sl(kFrames, 0.0f), sr(kFrames, 0.0f);
    std::vector<float> ml(kFrames, 0.0f), mr(kFrames, 0.0f);
    float* sOut[2] = {sl.data(), sr.data()};
    float* mOut[2] = {ml.data(), mr.data()};

    // Several blocks: cursors advance, so a threading bug shows up as drift
    // rather than only as a first-block difference.
    for (int block = 0; block < 8; ++block) {
      std::fill(sl.begin(), sl.end(), 0.0f);
      std::fill(sr.begin(), sr.end(), 0.0f);
      std::fill(ml.begin(), ml.end(), 0.0f);
      std::fill(mr.begin(), mr.end(), 0.0f);
      single.render(sOut, 2, kFrames);
      multi.render(mOut, 2, kFrames);

      // Numerically equivalent: any difference must be rounding, ~1e-6 on
      // signals of order 1, not a missing or doubled voice.
      for (int i = 0; i < kFrames; ++i) {
        MP_CHECK(std::fabs(sl[i] - ml[i]) < 1e-4f,
                 "threaded rendering matches inline within rounding (left)");
        MP_CHECK(std::fabs(sr[i] - mr[i]) < 1e-4f,
                 "threaded rendering matches inline within rounding (right)");
      }
    }

    // Per-bus rendering must agree too, since that is the path the processor
    // actually takes when an organ has enclosures.
    single.beginBlock();
    multi.beginBlock();
    for (int bus = 0; bus < 3; ++bus) {
      std::fill(sl.begin(), sl.end(), 0.0f);
      std::fill(ml.begin(), ml.end(), 0.0f);
      single.render(sOut, 2, kFrames, bus);
      multi.render(mOut, 2, kFrames, bus);
      for (int i = 0; i < kFrames; ++i)
        MP_CHECK(std::fabs(sl[i] - ml[i]) < 1e-4f,
                 "threaded per-bus rendering matches inline within rounding");
    }

    // Determinism: two identically-configured threaded engines must agree
    // EXACTLY. This is the property a fixed partition and fixed summation
    // order actually buy, and it is what makes a render reproducible.
    {
      voicetest::Fixture f1, f2;
      mp::VoiceEngine e1, e2;
      e1.prepare(48000.0, kVoices, 2, kFrames);
      e2.prepare(48000.0, kVoices, 2, kFrames);
      e1.setSampleProvider(f1.provider());
      e2.setSampleProvider(f2.provider());
      e1.setRenderThreads(4, 8);
      e2.setRenderThreads(4, 8);
      fill(e1, f1, kVoices);
      fill(e2, f2, kVoices);

      std::vector<float> a1(kFrames, 0.0f), a2(kFrames, 0.0f);
      std::vector<float> b1(kFrames, 0.0f), b2(kFrames, 0.0f);
      float* o1[2] = {a1.data(), b1.data()};
      float* o2[2] = {a2.data(), b2.data()};
      for (int block = 0; block < 4; ++block) {
        std::fill(a1.begin(), a1.end(), 0.0f);
        std::fill(b1.begin(), b1.end(), 0.0f);
        std::fill(a2.begin(), a2.end(), 0.0f);
        std::fill(b2.begin(), b2.end(), 0.0f);
        e1.render(o1, 2, kFrames);
        e2.render(o2, 2, kFrames);
        for (int i = 0; i < kFrames; ++i)
          MP_CHECK(a1[i] == a2[i],
                   "threaded rendering is deterministic run to run");
      }
    }

    // A block larger than the pool was prepared for must fall back to inline
    // rendering rather than overrun the worker scratch buffers.
    std::vector<float> big(kFrames * 4, 0.0f), big2(kFrames * 4, 0.0f);
    float* bigOut[2] = {big.data(), big2.data()};
    multi.render(bigOut, 2, kFrames * 4);
    for (float x : big) MP_CHECK(std::isfinite(x), "oversized block is safe");

    // Rebuilding the pool while voices sound must not corrupt anything.
    multi.setRenderThreads(2, 8);
    MP_CHECK(multi.renderThreads() == 2, "the pool can be resized");
    multi.render(mOut, 2, kFrames);
    for (float x : ml) MP_CHECK(std::isfinite(x), "audio survives a pool resize");

    // And turning threading off entirely.
    multi.setRenderThreads(1);
    MP_CHECK(multi.renderThreads() == 1, "threading can be disabled");
    multi.render(mOut, 2, kFrames);
    for (float x : ml) MP_CHECK(std::isfinite(x), "audio survives going inline");
  }
};

class VoiceEngineBusTest final : public mp::test::Test {
public:
  VoiceEngineBusTest()
    : Test("functional.voice.buses", Category::Functional) {}

  static double rms(const std::vector<float>& v) {
    double acc = 0.0;
    for (float x : v) acc += static_cast<double>(x) * x;
    return std::sqrt(acc / static_cast<double>(v.size()));
  }

  void run() override {
    voicetest::Fixture fx;
    mp::VoiceEngine eng;
    eng.prepare(48000.0, 16, 1);
    eng.setSampleProvider(fx.provider());

    mp::VoiceStart enclosed;
    enclosed.pipe = &fx.pipe;
    enclosed.layer = &fx.pipe.layers[0];
    enclosed.busIndex = 0;

    mp::VoiceStart open = enclosed;
    open.busIndex = 1;

    MP_CHECK(eng.startVoice(enclosed, 1) >= 0, "enclosed voice started");
    MP_CHECK(eng.startVoice(open, 2) >= 0, "unenclosed voice started");

    std::vector<float> buf(512, 0.0f);
    float* out[1] = {buf.data()};

    // Rendering one bus must produce only that bus's voices.
    eng.beginBlock();
    std::fill(buf.begin(), buf.end(), 0.0f);
    eng.render(out, 1, 512, 0);
    const double busZero = rms(buf);
    MP_CHECK(busZero > 0.0, "bus 0 renders its own voice");

    std::fill(buf.begin(), buf.end(), 0.0f);
    eng.render(out, 1, 512, 1);
    MP_CHECK(rms(buf) > 0.0, "bus 1 renders its own voice");

    // A bus nothing is routed to must be silent, not merely quiet.
    std::fill(buf.begin(), buf.end(), 0.0f);
    eng.render(out, 1, 512, 7);
    MP_CHECK(rms(buf) == 0.0, "an empty bus renders exact silence");

    // Rendering every bus must equal rendering with no filter at all.
    mp::VoiceEngine a, b;
    a.prepare(48000.0, 16, 1);
    b.prepare(48000.0, 16, 1);
    a.setSampleProvider(fx.provider());
    b.setSampleProvider(fx.provider());
    a.startVoice(enclosed, 1); a.startVoice(open, 2);
    b.startVoice(enclosed, 1); b.startVoice(open, 2);

    std::vector<float> all(512, 0.0f), summed(512, 0.0f), scratch(512, 0.0f);
    float* allOut[1] = {all.data()};
    float* scratchOut[1] = {scratch.data()};
    a.render(allOut, 1, 512);

    b.beginBlock();
    for (int bus = 0; bus < 2; ++bus) {
      std::fill(scratch.begin(), scratch.end(), 0.0f);
      b.render(scratchOut, 1, 512, bus);
      for (size_t i = 0; i < summed.size(); ++i) summed[i] += scratch[i];
    }
    for (size_t i = 0; i < all.size(); ++i)
      MP_CHECK(std::fabs(all[i] - summed[i]) < 1e-5f,
               "summing every bus equals rendering them together");

    // Voice ages must not drift when the caller renders bus by bus: that is
    // what beginBlock() is for, and getting it wrong would corrupt stealing.
    mp::VoiceEngine c;
    c.prepare(48000.0, 4, 1);
    c.setSampleProvider(fx.provider());
    c.startVoice(enclosed, 1);
    const uint64_t age = c.voice(0).startedAtBlock;
    c.beginBlock();
    for (int bus = 0; bus < 4; ++bus) c.render(scratchOut, 1, 64, bus);
    c.startVoice(open, 2);
    MP_CHECK(c.voice(1).startedAtBlock == age + 1,
             "one audio block advances the age by exactly one, not by bus count");
  }
};

class VoiceEnginePitchTest final : public mp::test::Test {
public:
  VoiceEnginePitchTest()
    : Test("functional.voice.pitch", Category::Functional) {}

  // Count zero crossings to estimate the rendered frequency.
  static double estimateHz(const std::vector<float>& v, double sr) {
    int crossings = 0;
    for (size_t i = 1; i < v.size(); ++i)
      if ((v[i - 1] < 0.0f) != (v[i] < 0.0f)) ++crossings;
    return (crossings * sr) / (2.0 * static_cast<double>(v.size()));
  }

  void run() override {
    voicetest::Fixture fx;

    // Always measure the same DURATION, not the same frame count: a
    // zero-crossing estimate resolves to about +/- 1/(2*seconds) Hz, so a
    // fixed buffer length would silently loosen the test at higher sample
    // rates (at 96 kHz, 2048 frames is only 21 ms -> +/- 23 Hz).
    constexpr double kMeasureSeconds = 0.1; // -> about +/- 5 Hz resolution
    auto renderAt = [&](double ratio, double engineRate) {
      mp::VoiceEngine eng;
      eng.prepare(engineRate, 8, 1);
      eng.setSampleProvider(fx.provider());
      mp::VoiceStart s;
      s.pipe = &fx.pipe;
      s.layer = &fx.pipe.layers[0];
      s.ratio = ratio;
      eng.startVoice(s, 1);
      const int frames = static_cast<int>(engineRate * kMeasureSeconds);
      std::vector<float> buf(static_cast<size_t>(frames), 0.0f);
      float* out[1] = {buf.data()};
      eng.render(out, 1, frames);
      return buf;
    };

    const double base = estimateHz(renderAt(1.0, 48000.0), 48000.0);
    MP_CHECK(std::fabs(base - 440.0) < 8.0,
             "ratio 1.0 reproduces the recorded pitch");

    // An octave up must double the sounding frequency.
    const double up = estimateHz(renderAt(2.0, 48000.0), 48000.0);
    MP_CHECK(std::fabs(up / base - 2.0) < 0.05,
             "ratio 2.0 sounds an octave higher");

    // Sample-rate conversion: a 48 kHz sample played by a 96 kHz engine must
    // still sound at 440 Hz, because the ratio folds in the rate difference.
    const double resampled = estimateHz(renderAt(1.0, 96000.0), 96000.0);
    MP_CHECK(std::fabs(resampled - 440.0) < 8.0,
             "a sample is pitch-correct when engine and file rates differ");

    // ...and the same holds downward, for a 44.1 kHz host.
    const double down = estimateHz(renderAt(1.0, 44100.0), 44100.0);
    MP_CHECK(std::fabs(down - 440.0) < 8.0,
             "pitch is correct at 44.1 kHz too");

    // A nonsense ratio must not hang or emit garbage.
    mp::VoiceEngine eng;
    eng.prepare(48000.0, 4, 1);
    eng.setSampleProvider(fx.provider());
    mp::VoiceStart s;
    s.pipe = &fx.pipe;
    s.layer = &fx.pipe.layers[0];
    s.ratio = 0.0; // must be rejected and replaced with unity
    eng.startVoice(s, 1);
    std::vector<float> buf(512, 0.0f);
    float* out[1] = {buf.data()};
    eng.render(out, 1, 512);
    for (float x : buf)
      MP_CHECK(std::isfinite(x), "a zero playback ratio never emits non-finite audio");
  }
};

class TuningTableTest final : public mp::test::Test {
public:
  TuningTableTest()
    : Test("functional.tuning.temperament-table", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(loadFixture("m22.tuning.Organ_Hauptwerk_xml", m, d),
             "tuning fixture must load");
    MP_CHECK(d.errors.empty(), "an unresolvable tuning is a warning, not an error");
    MP_CHECK(m.temperaments.size() == 3, "all three temperaments parsed");
    MP_CHECK(m.defaultTemperamentId == 1, "first temperament becomes the default");

    // Named, and in the built-in library: resolved from the derived tables.
    const auto& named = m.temperaments.at(1);
    MP_CHECK(named.resolved, "a library temperament resolves by name");
    MP_CHECK(named.centsOffset12.size() == 12, "resolved tuning has 12 offsets");
    const auto* lib = mp::findTemperament("Vallotti");
    MP_CHECK(lib != nullptr, "Vallotti is in the library");
    for (size_t i = 0; i < 12; ++i)
      MP_CHECK(std::fabs(named.centsOffset12[i] - lib->centsOffset12[i]) < 1e-9,
               "the ODF name resolves to the library table exactly");

    // Explicit offsets in the file win: a house tuning is not a library one.
    const auto& explicitTuning = m.temperaments.at(2);
    MP_CHECK(explicitTuning.resolved, "explicit offsets count as resolved");
    MP_CHECK(std::fabs(explicitTuning.centsOffset12[0]) < 1e-9,
             "explicit tuning is anchored on C");
    MP_CHECK(std::fabs(explicitTuning.centsOffset12[11] + 10.0) < 1e-9,
             "explicit per-note offsets are read from the file");

    // Unresolvable: reported, and left empty so the solver falls back safely.
    const auto& unknown = m.temperaments.at(3);
    MP_CHECK(!unknown.resolved, "an unknown tuning name stays unresolved");
    MP_CHECK(unknown.centsOffset12.empty(),
             "an unresolved tuning carries no offsets");
    MP_CHECK(d.unknownTemperaments.size() == 1,
             "the unknown tuning is reported once");
    MP_CHECK(d.unknownTemperaments[0] == "Nonesuch Irregular 1712",
             "the unresolvable name is reported verbatim for triage");
    bool warned = false;
    for (const auto& w : d.warnings)
      if (w.find("equal temperament") != std::string::npos) warned = true;
    MP_CHECK(warned, "the player is told the organ will play in equal temperament");

    // pipe-pitch-out-of-range catches a mis-declared footage.
    MP_CHECK(d.pipesPitchOutOfRange.size() == 1,
             "exactly the impossible pipe is flagged");
    MP_CHECK(d.pipesPitchOutOfRange[0].pipeId == 12,
             "the pipe with an absurd harmonic number is the one flagged");
    MP_CHECK(d.pipesPitchOutOfRange[0].harmonicNum == 4096 &&
                 d.pipesPitchOutOfRange[0].hz > mp::kMaxPipeHz,
             "the fault carries what produced it, not just an id");

    // Footage is read per pipe, because a rank can be transposed.
    const auto& rank = m.ranks.at(10);
    MP_CHECK(rank.pipes.size() == 3, "all pipes parsed");
    bool saw8 = false, saw16 = false;
    for (const auto& pipe : rank.pipes) {
      if (pipe.pipeId == 10) {
        saw8 = pipe.basePitch64ftHarmonicNum == 8;
        // An 8' A4 must sound at the organ's base pitch under any tuning that
        // leaves A alone.
        const double hz = mp::pipeTargetHz(pipe.midiNote,
                                           pipe.basePitch64ftHarmonicNum,
                                           m.basePitchHz, 0.0,
                                           mp::Temperament{"Equal", std::vector<double>(12, 0.0)}, 0);
        MP_CHECK(std::fabs(hz - 440.0) < 1e-9,
                 "an 8' A4 sounds at the organ base pitch");
      }
      if (pipe.pipeId == 11) saw16 = pipe.basePitch64ftHarmonicNum == 4;
    }
    MP_CHECK(saw8, "8' footage parsed per pipe");
    MP_CHECK(saw16, "16' footage parsed per pipe");
  }
};

// MIDI mapping. A player's console is not the organ's: their drawstop sends
// note 36 on channel 3, their shoe sends CC 11, and none of that is in the ODF.
// The mapping therefore lives outside the model, is built by learning rather
// than typing, and has to survive a save and reload.
// A drawn manual is assembled key by key from per-shape artwork, and getting
// that wrong is not subtle: keys drift out of the case, or a notch appears
// where no sharp exists. The values here are Lemmer's own KeyImageSet 1
// ("CustKS_OldManual", a Flentrop of 1977), read out of its ODF.
class KeyboardLayoutTest final : public mp::test::Test {
public:
  KeyboardLayoutTest() : Test("functional.console.keyboard-layout", Category::Functional) {}

  // Distinct ids per shape so a test can name which one came back.
  static mp::KeyImageSet lemmer() {
    mp::KeyImageSet k;
    k.keyImageSetId = 1;
    k.shapeCF = 7;
    k.shapeD = 8;
    k.shapeEB = 9;
    k.shapeG = 10;
    k.shapeA = 11;
    k.shapeWholeNatural = 12;
    k.shapeFirstKeyDA = 13;
    k.shapeFirstKeyG = 14;
    k.shapeLastKeyDG = 15;
    k.shapeLastKeyA = 16;
    k.shapeSharp = 17;
    k.spacingNaturalToNatural = 16;
    k.spacingCFToSharp = 10;
    k.spacingDAToSharp = 10;
    k.spacingGToSharp = 10;
    k.spacingSharpToDG = 7;
    k.spacingSharpToEB = 7;
    k.spacingSharpToA = 7;
    return k;
  }

  void run() override {
    const auto ks = lemmer();

    // --- shapes, mid-compass -------------------------------------------
    // C and F share a cut, and so do E and B: what decides the shape is which
    // side a sharp sits on, not the letter.
    MP_CHECK(mp::keyShapeFor(ks, 0, false, false) == 7, "a C is a CF");
    MP_CHECK(mp::keyShapeFor(ks, 5, false, false) == 7, "an F is a CF too");
    MP_CHECK(mp::keyShapeFor(ks, 4, false, false) == 9, "an E is an EB");
    MP_CHECK(mp::keyShapeFor(ks, 11, false, false) == 9, "a B is an EB too");
    MP_CHECK(mp::keyShapeFor(ks, 2, false, false) == 8, "a D has its own cut");
    MP_CHECK(mp::keyShapeFor(ks, 7, false, false) == 10, "so does a G");
    MP_CHECK(mp::keyShapeFor(ks, 9, false, false) == 11, "and an A");
    for (int pc : {1, 3, 6, 8, 10})
      MP_CHECK(mp::keyShapeFor(ks, pc, false, false) == 17,
               "every sharp is the same shape");

    // --- the ends of the compass ----------------------------------------
    // The special cuts exist because the neighbouring sharp is missing there.
    MP_CHECK(mp::keyShapeFor(ks, 2, true, false) == 13, "a bottom D is cut square on the left");
    MP_CHECK(mp::keyShapeFor(ks, 9, true, false) == 13, "and so is a bottom A");
    MP_CHECK(mp::keyShapeFor(ks, 7, true, false) == 14, "a bottom G has its own cut");
    MP_CHECK(mp::keyShapeFor(ks, 4, true, false) == 12,
             "a bottom E is a whole natural: there is no D sharp below it");
    MP_CHECK(mp::keyShapeFor(ks, 0, true, false) == 7,
             "a bottom C needs nothing special; a C is never cut on its left");

    MP_CHECK(mp::keyShapeFor(ks, 2, false, true) == 15, "a top D is cut square on the right");
    MP_CHECK(mp::keyShapeFor(ks, 7, false, true) == 15, "and a top G takes the same cut");
    MP_CHECK(mp::keyShapeFor(ks, 9, false, true) == 16, "a top A has its own");
    MP_CHECK(mp::keyShapeFor(ks, 0, false, true) == 12,
             "a top C is a whole natural: there is no C sharp above it");
    MP_CHECK(mp::keyShapeFor(ks, 11, false, true) == 9,
             "a top B is an ordinary EB; nothing sat above it anyway");
    MP_CHECK(mp::keyShapeFor(ks, 1, true, true) == 17,
             "an end-of-compass sharp is still just a sharp");

    // A set that defines only the plain natural still draws a keyboard rather
    // than nothing: sets in the wild are routinely this sparse.
    mp::KeyImageSet sparse;
    sparse.shapeWholeNatural = 99;
    sparse.shapeSharp = 98;
    for (int pc = 0; pc < 12; ++pc)
      MP_CHECK(mp::keyShapeFor(sparse, pc, false, false) ==
                   (mp::isSharpPitchClass(pc) ? 98 : 99),
               "a sparse set falls back to the whole natural");

    // --- advances --------------------------------------------------------
    // Each of the seven advances has to reach the right neighbour, and mixing
    // two of them up is invisible key by key but ruins the octave. Pinning
    // each one by name is what catches that.
    MP_CHECK(mp::keyAdvance(ks, 0) == ks.spacingCFToSharp, "C reaches its sharp");
    MP_CHECK(mp::keyAdvance(ks, 5) == ks.spacingCFToSharp, "and so does F");
    MP_CHECK(mp::keyAdvance(ks, 1) == ks.spacingSharpToDG, "C sharp reaches D");
    MP_CHECK(mp::keyAdvance(ks, 6) == ks.spacingSharpToDG, "F sharp reaches G");
    MP_CHECK(mp::keyAdvance(ks, 2) == ks.spacingDAToSharp, "D reaches its sharp");
    MP_CHECK(mp::keyAdvance(ks, 9) == ks.spacingDAToSharp, "and so does A");
    MP_CHECK(mp::keyAdvance(ks, 3) == ks.spacingSharpToEB, "D sharp reaches E");
    MP_CHECK(mp::keyAdvance(ks, 10) == ks.spacingSharpToEB, "A sharp reaches B");
    MP_CHECK(mp::keyAdvance(ks, 7) == ks.spacingGToSharp, "G reaches G sharp");
    MP_CHECK(mp::keyAdvance(ks, 8) == ks.spacingSharpToA, "G sharp reaches A");
    MP_CHECK(mp::keyAdvance(ks, 4) == ks.spacingNaturalToNatural,
             "E reaches F with no sharp in between");
    MP_CHECK(mp::keyAdvance(ks, 11) == ks.spacingNaturalToNatural,
             "and B reaches C the same way");

    // Note what is NOT asserted here: that an octave closes on seven natural
    // widths. It does not. Lemmer's own numbers make C to D 17 pixels while E
    // to F is 16, so the octave comes out at 117 rather than 112, and the
    // pedal set is further out still. Real sets are authored key by key with
    // the sharps overlapping their neighbours, and the reference conversion in
    // specs/odfedit walks them sequentially exactly as this does. A tidier
    // rule would draw a tidier keyboard than the organ actually has.

    // Lemmer's manual: 56 keys from MIDI 36, drawn from x=488 on artwork that
    // is 1536 wide. Walking the whole compass has to land inside the case.
    int x = 0;
    int naturals = 0, sharps = 0;
    for (int i = 0; i < 56; ++i) {
      const int pc = mp::pitchClassOf(36 + i);
      (mp::isSharpPitchClass(pc) ? sharps : naturals) += 1;
      if (i < 55) x += mp::keyAdvance(ks, pc);
    }
    MP_CHECK(naturals == 33 && sharps == 23,
             "a 56-note compass from C is 33 naturals and 23 sharps");
    MP_CHECK(x == 535, "the last key starts 535 pixels along");
    MP_CHECK(488 + x + 16 <= 1536, "so the manual fits inside the console");
  }
};

// The wind. An organ is a pneumatic machine: every sounding pipe draws air out
// of the chest it stands on, and if enough of them do it at once the pressure
// dips and the whole instrument goes momentarily flatter and softer. One flute
// does nothing. That difference is the entire point, and it is what this
// checks — on a synthetic organ, because the sag a real one shows depends on
// how many stops the set happens to ship.
class WindSolverTest final : public mp::test::Test {
public:
  WindSolverTest() : Test("functional.control.wind", Category::Functional) {}

  // A blower at five inches, feeding one small chest through a supply pipe,
  // with a leak to the open air. The shape every organ has.
  static mp::OrganModel organ() {
    mp::OrganModel m;
    mp::WindCompartment air;
    air.compartmentId = 1;
    air.name = "Open air";
    air.infiniteVolume = true;
    air.defaultPressureInches = 0.0;
    m.wind[1] = air;

    mp::WindCompartment blower;
    blower.compartmentId = 2;
    blower.name = "Blower";
    blower.infiniteVolume = true;
    blower.defaultPressureInches = 5.0;
    m.wind[2] = blower;

    mp::WindCompartment chest;
    chest.compartmentId = 3;
    chest.name = "Chest";
    chest.infiniteVolume = false;
    chest.volumeM3 = 0.3;
    m.wind[3] = chest;

    mp::WindCompartmentLink supply;
    supply.firstCompartmentId = 2;
    supply.secondCompartmentId = 3;
    supply.massFlowKgPerSec = 0.05;
    supply.refPressureInches = 3.0;
    m.windLinks.push_back(supply);

    mp::WindCompartmentLink leak;
    leak.firstCompartmentId = 3;
    leak.secondCompartmentId = 1;
    leak.massFlowKgPerSec = 0.002;
    leak.refPressureInches = 3.0;
    m.windLinks.push_back(leak);
    return m;
  }

  static mp::Pipe pipeOn(mp::Id chest, double kgPerSec) {
    mp::Pipe p;
    p.pipeId = 1;
    p.windSourceCompartmentId = chest;
    p.windMassFlowKgPerSec = kgPerSec;
    return p;
  }

  // Hold a load for `seconds` and report how far the chest sags.
  static double holdFor(mp::WindSolver& wind, double kgPerSec, double seconds) {
    const mp::EngineSwitch full;
    const std::unordered_set<mp::Id> nothing;
    const std::vector<mp::Id> chests{3};
    const std::vector<float> demand{static_cast<float>(kgPerSec)};
    double worst = 0.0;
    for (int i = 0; i < static_cast<int>(seconds * 200.0); ++i) {
      wind.clearDemand();
      wind.addDemandDirect(chests, demand);
      wind.advance(0.005, full, nothing);
      worst = std::min(worst, wind.sagFor(3));
    }
    return worst;
  }

  void run() override {
    const auto m = organ();
    mp::WindSolver wind;
    wind.reset(m);

    MP_CHECK(wind.active() && wind.modelledCompartments() == 1,
             "one chest is modelled; the blower and the open air are boundaries");
    // The chest settles between its supply and its leak, close to the blower.
    const double working = wind.nominalFor(3);
    MP_CHECK(working > 4.0 && working < 5.01,
             "the chest settles just under the blower's five inches, held down "
             "by its own leak");

    // --- nothing playing ---------------------------------------------------
    MP_CHECK(std::fabs(holdFor(wind, 0.0, 2.0)) < 1e-4,
             "an organ with nobody playing does not sag - the settled state "
             "has to BE settled, or every later number is measuring drift");

    // --- one flute ---------------------------------------------------------
    const double oneStop = holdFor(wind, 0.0005, 2.0);
    MP_CHECK(oneStop > -0.005,
             "a single pipe does not move the wind, which is why an organ is "
             "playable at all");

    // --- the full organ ----------------------------------------------------
    const double tutti = holdFor(wind, 0.04, 2.0);
    std::printf("        one pipe %.3f%%, tutti %.2f%% (working %.2f inches)\n",
                oneStop * 100.0, tutti * 100.0, working);
    MP_CHECK(tutti < -0.02,
             "a full tutti pulls the pressure down measurably");
    MP_CHECK(tutti < oneStop * 5.0,
             "and far further than one pipe does - the difference between the "
             "two is the whole model");

    // --- what the pipes do about it ---------------------------------------
    const auto mod = wind.modFor(3);
    MP_CHECK(mod.ampMul < 1.0 && mod.pitchRatio < 1.0,
             "a sagging chest makes its pipes quieter AND flatter");
    const double cents = 1200.0 * std::log2(mod.pitchRatio);
    MP_CHECK(cents > -60.0,
             "by cents, not semitones: an organ under load goes flat, it does "
             "not change key");

    // --- recovery ----------------------------------------------------------
    holdFor(wind, 0.0, 5.0);
    MP_CHECK(std::fabs(wind.sagFor(3)) < 1e-3,
             "and the wind comes back when the keys come up");

    // --- a soak, because a wind model that goes non-finite silences the
    // organ permanently rather than for one block --------------------------
    const mp::EngineSwitch full2;
    const std::unordered_set<mp::Id> nothing;
    const std::vector<mp::Id> chests{3};
    for (int i = 0; i < 20000; ++i) {
      // Slam the load on and off every block, which is the worst a player can
      // do to it and much worse than anything musical.
      const std::vector<float> demand{(i % 2) ? 0.2f : 0.0f};
      wind.clearDemand();
      wind.addDemandDirect(chests, demand);
      wind.advance(0.005, full2, nothing);
      const double p = wind.pressureFor(3);
      MP_CHECK(std::isfinite(p) && p >= 0.0 && p < 100.0,
               "the pressure stays finite and physical under abuse");
    }

    // --- and it obeys the bypass ------------------------------------------
    mp::EngineSwitch off;
    off.enableWindModel = false;
    wind.advance(0.005, off, nothing);
    const auto idle = wind.modFor(3);
    MP_CHECK(idle.ampMul == 1.0 && idle.pitchRatio == 1.0,
             "with the model switched off nothing is modulated at all");
  }
};

// The registration sequencer: one thumb piston that walks the generals in
// order. Unlike everything else in M3 this is NOT wired in the organ file —
// Hauptwerk provides it and the player maps it — so what it walks, and in what
// order, is a decision rather than a reading.
class StepperTest final : public mp::test::Test {
public:
  StepperTest() : Test("functional.control.stepper", Category::Functional) {}

  static void addCombo(mp::OrganModel& m, mp::Id id, int type,
                       const char* name, bool withElements = true) {
    mp::Combination c;
    c.combinationId = id;
    c.type = type;
    c.name = name;
    if (withElements) {
      mp::CombinationElement el;
      el.combinationId = id;
      el.controlledSwitchId = 201;
      c.elements.push_back(el);
    }
    if (c.isCancel()) {
      c.canEngage = false;
      c.canDisengage = true;
    }
    m.combinations[id] = std::move(c);
  }

  void run() override {
    mp::OrganModel m;
    // Deliberately out of id order, to prove the order comes from the piston
    // number and not from whatever the map iterates first.
    addCombo(m, 9003, 103, "General 03");
    addCombo(m, 9001, 101, "General 01");
    addCombo(m, 9002, 102, "General 02");
    addCombo(m, 9100, 100, "General cancel");   // a cancel: never stepped onto
    addCombo(m, 9200, 201, "Pedal divisional"); // a divisional: not a general
    addCombo(m, 9004, 104, "General 04", /*withElements*/ false); // empty

    mp::Stepper st;
    st.reset(m);

    MP_CHECK(st.frameCount() == 3,
             "the sequencer walks the generals only - not the cancel, not a "
             "divisional, and not a general with nothing in it");
    MP_CHECK(st.frame() == 0, "and starts before the first frame");
    MP_CHECK(st.current() == 0, "with nothing selected");

    MP_CHECK(st.next() == 9001, "stepping forward reaches General 01");
    MP_CHECK(st.frame() == 1, "which organists count as frame one");
    MP_CHECK(st.next() == 9002 && st.next() == 9003,
             "and then 02 and 03, in piston order rather than id order");

    // Not wrapping is the point. A sequencer that rolls round to the first
    // frame will do it in performance, at the loudest possible moment.
    MP_CHECK(st.next() == 0, "stepping past the last frame does nothing");
    MP_CHECK(st.frame() == 3, "and stays where it was");

    MP_CHECK(st.prev() == 9002 && st.prev() == 9001, "stepping back retraces");
    MP_CHECK(st.prev() == 0 && st.frame() == 1,
             "and stops at the first frame rather than falling off the front");

    MP_CHECK(st.gotoFrame(3) == 9003, "a console that sends a number can jump");
    MP_CHECK(st.gotoFrame(0) == 0 && st.gotoFrame(4) == 0,
             "to a frame that exists, and no other");
    MP_CHECK(st.frame() == 3, "a refused jump leaves it where it was");

    st.rewind();
    MP_CHECK(st.frame() == 0 && st.current() == 0, "and it can be rewound");

    // An organ with no generals at all must say so rather than pretending.
    mp::OrganModel bare;
    mp::Stepper none;
    none.reset(bare);
    MP_CHECK(none.empty() && none.next() == 0 && none.prev() == 0,
             "an organ with no generals has no sequencer, and stepping it is "
             "harmless");
  }
};

// The crescendo, and everything else a shoe position moves.
//
// A crescendo is not a special mechanism: it is one continuous control with a
// ContinuousControlStageSwitch row per step, each naming the switch that fires
// that step's registration. The same rows start a blower and trigger enclosure
// noises. Each row is a THRESHOLD CROSSING, not a state, which is what lets an
// organ give a switch hysteresis.
// Two controls combined into a third. This is how a set's own settings page
// reaches its pipework: a level slider and an audio-group level are multiplied
// into the control that every pipe layer names as its amplitude scaler. Get
// this wrong and the sliders move numbers that touch no sound.
class DoubleLinkageTest final : public mp::test::Test {
public:
  DoubleLinkageTest()
    : Test("functional.control.double-linkage", Category::Functional) {}

  static mp::ContinuousControl control(mp::Id id, int dflt) {
    mp::ContinuousControl c;
    c.controlId = id;
    c.defaultValue = dflt;
    c.minValue = 0;
    c.maxValue = 127;
    return c;
  }

  static mp::ContinuousControlDoubleLinkage link(mp::Id dest, mp::Id a,
                                                 mp::Id b, int op,
                                                 double destCoef) {
    mp::ContinuousControlDoubleLinkage d;
    d.destControlId = dest;
    d.firstControlId = a;
    d.secondControlId = b;
    d.operationCode = op;
    d.destCoefficient = destCoef;
    return d;
  }

  void run() override {
    mp::OrganModel m;
    for (mp::Id id : {mp::Id(1), mp::Id(2), mp::Id(10), mp::Id(11), mp::Id(12)})
      m.continuousControls[id] = control(id, 0);

    // The shape a real set uses: two 0..127 levels multiplied and renormalised
    // by 1/127 so the product lands back in range.
    m.controlDoubleLinkages.push_back(link(10, 1, 2, 3, 1.0 / 127.0));
    m.controlDoubleLinkages.push_back(link(11, 1, 2, 1, 1.0)); // add
    m.controlDoubleLinkages.push_back(link(12, 1, 2, 2, 1.0)); // subtract

    mp::ContinuousControlBank bank;
    bank.reset(m);

    // Both full: the product must come back as full, not as 127*127.
    bank.setValue(1, 127);
    bank.setValue(2, 127);
    bank.propagate();
    MP_CHECK(bank.value(10) == 127,
             "two full levels multiply to full, renormalised");
    MP_CHECK(bank.value(11) == 127, "and an add saturates at the top");
    MP_CHECK(bank.value(12) == 0, "and a subtract of equals is zero");

    // Halve one of them. 127 * 64 / 127 = 64: a level slider at half should
    // halve the result, which is the whole point of the mechanism.
    bank.setValue(2, 64);
    bank.propagate();
    MP_CHECK(bank.value(10) == 64, "halving one source halves the product");

    // Either source at zero silences it, however loud the other is.
    bank.setValue(2, 0);
    bank.propagate();
    MP_CHECK(bank.value(10) == 0, "a source at zero takes the product to zero");

    // A chain: a single linkage feeding a double one. Real sets do this, and
    // both kinds have to settle in the same pass loop or the answer is stale.
    mp::OrganModel chained;
    for (mp::Id id : {mp::Id(1), mp::Id(2), mp::Id(3), mp::Id(10)})
      chained.continuousControls[id] = control(id, 0);
    mp::ContinuousControlLinkage single;
    single.sourceControlId = 1;
    single.destControlId = 3;   // 3 follows 1
    chained.controlLinkages.push_back(single);
    chained.controlDoubleLinkages.push_back(link(10, 3, 2, 3, 1.0 / 127.0));

    mp::ContinuousControlBank c2;
    c2.reset(chained);
    c2.setValue(1, 64);
    c2.setValue(2, 127);
    c2.propagate(1);  // 1 is pinned: the player moved it
    MP_CHECK(c2.value(3) == 64, "a single linkage carries the value along");
    MP_CHECK(c2.value(10) == 64,
             "and the double linkage downstream of it settles in the same pass");

    // An unknown operation code must not invent an answer. The loader rejects
    // those, so the bank should never see one — but if it does, the
    // destination keeps whatever it had.
    mp::OrganModel odd;
    odd.continuousControls[1] = control(1, 0);
    odd.continuousControls[2] = control(2, 0);
    odd.continuousControls[10] = control(10, 99);
    odd.controlDoubleLinkages.push_back(link(10, 1, 2, 42, 1.0));
    mp::ContinuousControlBank c3;
    c3.reset(odd);
    c3.setValue(1, 127);
    c3.setValue(2, 127);
    c3.propagate();
    MP_CHECK(c3.value(10) == 99,
             "an unknown operation leaves its destination alone");
  }
};

// A tremulant crossfade feeds one control from two linkages on the same
// switch, one live while it is engaged and one while it is not. Reading both
// as "while engaged" set them fighting: with the tremulant on, each pass
// overwrote the other's value, the solver never settled, and Erfurt spent a
// second on every audio block.
class ConditionSenseTest final : public mp::test::Test {
public:
  ConditionSenseTest()
    : Test("functional.control.condition-sense", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    for (mp::Id id : {mp::Id(23), mp::Id(24), mp::Id(41)})
      m.continuousControls[id] = DoubleLinkageTest::control(id, 0);
    mp::ContinuousControlLinkage on, off;
    on.sourceControlId = 23; on.destControlId = 41;
    on.conditionSwitchId = 702; on.conditionWhenEngaged = true;
    off.sourceControlId = 24; off.destControlId = 41;
    off.conditionSwitchId = 702; off.conditionWhenEngaged = false;
    m.controlLinkages = {off, on};

    mp::ContinuousControlBank bank;
    bank.reset(m);
    bank.setValue(23, 100);
    bank.setValue(24, 20);
    std::unordered_set<mp::Id> sw;
    bank.propagate(0, &sw);
    MP_CHECK(bank.value(41) == 20, "tremulant off: the disengaged-sense link drives");
    sw.insert(702);
    bank.propagate(0, &sw);
    MP_CHECK(bank.value(41) == 100, "tremulant on: only the engaged-sense link drives");
  }
};


class StageSwitchTest final : public mp::test::Test {
public:
  StageSwitchTest()
    : Test("functional.control.stage-switches", Category::Functional) {}

  static mp::ContinuousControlStageSwitch step(mp::Id control, int value,
                                               mp::Id sw) {
    mp::ContinuousControlStageSwitch r;
    r.controlId = control;
    r.value = value;
    r.controlledSwitchId = sw;
    // A crescendo step claims every direction: it means "this step is the one
    // now", whichever way the shoe was moving.
    r.engageWhenIncreasing = r.engageWhenDecreasing = true;
    r.disengageWhenIncreasing = r.disengageWhenDecreasing = true;
    return r;
  }

  static mp::Id lastEngaged(const std::vector<mp::StageSwitchBank::Change>& v) {
    mp::Id id = 0;
    for (const auto& c : v)
      if (c.engage) id = c.switchId;
    return id;
  }

  void run() override {
    // A four-step crescendo at 0, 32, 64 and 96.
    mp::OrganModel m;
    m.controlStageSwitches = {step(51, 0, 901), step(51, 32, 902),
                              step(51, 64, 903), step(51, 96, 904)};

    mp::StageSwitchBank bank;
    bank.reset(m);
    MP_CHECK(bank.drives(51), "the shoe drives switches");
    MP_CHECK(!bank.drives(52), "and nothing else does");
    MP_CHECK(bank.stepCount(51) == 4, "four steps");

    // --- sweeping up -------------------------------------------------------
    std::vector<mp::StageSwitchBank::Change> out;
    bank.moveControl(51, 0, 127, out);
    MP_CHECK(lastEngaged(out) == 904,
             "sweeping the shoe to full leaves the topmost step engaged - the "
             "steps below it fire on the way past, and the last one wins");

    // Every step below the top must have been left behind rather than latched
    // on, or the shoe could never fire it again.
    int engagedAtEnd = 0;
    std::unordered_map<mp::Id, bool> state;
    for (const auto& c : out) state[c.switchId] = c.engage;
    for (const auto& [id, on] : state) {
      (void)id;
      if (on) ++engagedAtEnd;
    }
    MP_CHECK(engagedAtEnd == 1, "exactly one step is left standing");

    // --- and back down -----------------------------------------------------
    out.clear();
    bank.moveControl(51, 127, 0, out);
    MP_CHECK(lastEngaged(out) == 901,
             "sweeping back to nothing leaves the bottom step engaged, because "
             "the crossings fire in the order the shoe meets them");

    // --- a small move crosses nothing --------------------------------------
    out.clear();
    bank.moveControl(51, 10, 20, out);
    MP_CHECK(out.empty(), "a move between two steps changes nothing");
    out.clear();
    bank.moveControl(51, 40, 40, out);
    MP_CHECK(out.empty(), "and a move that is not a move does nothing at all");

    // --- landing exactly on a threshold ------------------------------------
    out.clear();
    bank.moveControl(51, 0, 32, out);
    MP_CHECK(lastEngaged(out) == 902,
             "arriving exactly on a step counts as reaching it");
    out.clear();
    bank.moveControl(51, 32, 33, out);
    MP_CHECK(out.empty(), "and leaving it does not fire it a second time");

    // --- hysteresis: two rows, one switch ----------------------------------
    // How an organ starts a blower: on when the value rises through 120, off
    // when it falls back through 126. A single-threshold rule cannot do this,
    // which is why the rows are crossings rather than states.
    mp::OrganModel blowerModel;
    mp::ContinuousControlStageSwitch on;
    on.controlId = 9001;
    on.value = 120;
    on.controlledSwitchId = 99;
    on.engageWhenIncreasing = true;
    mp::ContinuousControlStageSwitch off;
    off.controlId = 9001;
    off.value = 126;
    off.controlledSwitchId = 99;
    off.disengageWhenDecreasing = true;
    blowerModel.controlStageSwitches = {on, off};

    mp::StageSwitchBank blower;
    blower.reset(blowerModel);

    out.clear();
    blower.moveControl(9001, 0, 127, out);
    MP_CHECK(out.size() == 1 && out.front().switchId == 99 && out.front().engage,
             "rising past 120 starts the blower, and passing 126 does not stop "
             "it again on the way up");

    out.clear();
    blower.moveControl(9001, 127, 121, out);
    MP_CHECK(out.size() == 1 && !out.front().engage,
             "and falling back through 126 stops it");

    out.clear();
    blower.moveControl(9001, 121, 0, out);
    MP_CHECK(out.empty(),
             "falling further past 120 does nothing: that row only ever starts "
             "it, which is the whole point of two rows");
  }
};

// Pistons. A combination is a list of switches and the state each should be
// in; pressing its piston sets them, pressing it with the setter held stores
// what is drawn. The part that is easy to get wrong is not the recall — it is
// finding which switch fires which combination, because an organ with ordinary
// generals names no switch anywhere and wires them by assignment code.
class CombinationTest final : public mp::test::Test {
public:
  CombinationTest()
    : Test("functional.combinations.pistons", Category::Functional) {}

  // Three stops, a general piston and a general cancel, wired the way Lemmer
  // wires them: by matching assignment code, with nothing named explicitly.
  static mp::OrganModel organ() {
    mp::OrganModel m;
    for (mp::Id id : {mp::Id{201}, mp::Id{202}, mp::Id{203}}) {
      mp::Switch sw;
      sw.switchId = id;
      m.switches[id] = sw;
    }
    // The piston switches. Momentary, as a piston is.
    mp::Switch general1;
    general1.switchId = 101;
    general1.asgnCode = 101; // "Comb. Gen. 01"
    general1.latching = false;
    m.switches[101] = general1;

    mp::Switch cancel;
    cancel.switchId = 100;
    cancel.asgnCode = 100; // "Comb. Gen. Cancel"
    cancel.latching = false;
    m.switches[100] = cancel;

    auto makeCombo = [&](mp::Id id, int type, const char* name) {
      mp::Combination c;
      c.combinationId = id;
      c.type = type;
      c.name = name;
      for (mp::Id sw : {mp::Id{201}, mp::Id{202}, mp::Id{203}}) {
        mp::CombinationElement el;
        el.combinationId = id;
        el.controlledSwitchId = sw;
        el.capturedSwitchId = sw;
        c.elements.push_back(el);
      }
      if (c.isCancel()) {
        c.canEngage = false;
        c.canDisengage = true;
      }
      m.combinations[id] = std::move(c);
    };
    makeCombo(3073, 101, "General 01");
    makeCombo(1537, 100, "General cancel");
    return m;
  }

  static bool moves(const std::vector<mp::CombinationSystem::Change>& v,
                    mp::Id sw, bool engage) {
    for (const auto& c : v)
      if (c.switchId == sw && c.engage == engage) return true;
    return false;
  }

  void run() override {
    auto m = organ();
    mp::CombinationSystem combos;
    combos.reset(m);

    // --- finding the piston ---------------------------------------------
    MP_CHECK(combos.combinationForSwitch(101) == 3073,
             "a general piston finds its combination through the assignment "
             "code, with nothing naming it - which is how real organs wire it");
    MP_CHECK(combos.combinationForSwitch(100) == 1537, "and so does the cancel");
    MP_CHECK(combos.combinationForSwitch(201) == 0, "a drawstop is not a piston");

    // --- an empty piston --------------------------------------------------
    std::vector<mp::CombinationSystem::Change> changes;
    combos.recall(3073, changes);
    MP_CHECK(changes.size() == 3, "an unset general still moves what it controls");
    MP_CHECK(moves(changes, 201, false) && moves(changes, 202, false),
             "and moves it all out, because it holds nothing yet");

    // --- capture -----------------------------------------------------------
    std::unordered_set<mp::Id> drawn{201, 203};
    const auto live = [&drawn](mp::Id id) { return drawn.count(id) != 0; };
    MP_CHECK(combos.capture(3073, live), "capture stores what is drawn");
    MP_CHECK(combos.programmedCount() == 1, "one piston now holds something");

    changes.clear();
    combos.recall(3073, changes);
    MP_CHECK(moves(changes, 201, true) && moves(changes, 203, true) &&
                 moves(changes, 202, false),
             "and recalling it puts exactly those stops back");

    // --- cancel ------------------------------------------------------------
    changes.clear();
    combos.recall(1537, changes);
    MP_CHECK(changes.size() == 3, "a cancel touches everything it controls");
    for (const auto& c : changes)
      MP_CHECK(!c.engage, "and only ever takes stops out, never puts them in");

    // Capturing a cancel must not turn it into an ordinary general.
    MP_CHECK(combos.capture(1537, live), "a cancel captures like any other");
    changes.clear();
    combos.recall(1537, changes);
    for (const auto& c : changes)
      MP_CHECK(!c.engage,
               "and still only cancels, because that is its type and not its "
               "contents");

    // --- saving and coming back -------------------------------------------
    const std::string text = combos.toText();
    mp::CombinationSystem reloaded;
    reloaded.reset(m);
    MP_CHECK(reloaded.programmedCount() == 0, "a fresh organ holds nothing");
    MP_CHECK(reloaded.fromText(text), "the saved file reads back cleanly");
    changes.clear();
    reloaded.recall(3073, changes);
    MP_CHECK(moves(changes, 201, true) && moves(changes, 203, true) &&
                 moves(changes, 202, false),
             "and the registration survives the round trip");

    // A file saved against a different organ must not invent switches.
    mp::CombinationSystem other;
    other.reset(m);
    MP_CHECK(!other.fromText("3073 999 1\n"),
             "a line naming a switch this organ does not have is reported");
    MP_CHECK(other.programmedCount() == 0, "and nothing is invented from it");

    // --- setter mode -------------------------------------------------------
    MP_CHECK(!combos.captureMode(), "the setter starts out");
    combos.setCaptureMode(true);
    MP_CHECK(combos.captureMode(), "and can be held");
  }
};

// The switch network: what a drawstop actually does.
//
// On a real console the switch a player clicks is almost never the switch
// anything reads. Lemmer's coupler is three switches: the drawn one (10102),
// the logical coupler (1006) and the node the key flow consults (10101). Miss
// the wiring and the drawstop moves on screen and couples nothing, which is
// exactly what happened before this existed.
class SwitchNetworkTest final : public mp::test::Test {
public:
  SwitchNetworkTest()
    : Test("functional.control.switch-network", Category::Functional) {}

  static mp::SwitchLinkage wire(mp::Id from, mp::Id to, mp::Id condition = 0) {
    mp::SwitchLinkage l;
    l.sourceSwitchId = from;
    l.destSwitchId = to;
    l.conditionSwitchId = condition;
    return l;
  }

  static void addSwitch(mp::OrganModel& m, mp::Id id, bool defaultEngaged = false) {
    mp::Switch sw;
    sw.switchId = id;
    sw.defaultEngaged = defaultEngaged;
    m.switches[id] = sw;
  }

  void run() override {
    // --- an organ with no wiring at all --------------------------------
    {
      mp::OrganModel m;
      addSwitch(m, 1);
      mp::SwitchNetwork net;
      net.reset(m);
      MP_CHECK(net.linkageCount() == 0, "nothing is wired");
      MP_CHECK(!net.engaged(1), "and the switch is out");
      net.set(1, true);
      MP_CHECK(net.engaged(1), "setting it engages it, and nothing else");
      MP_CHECK(net.lastChanges().size() == 1, "one switch moved");
    }

    // --- the shape Lemmer actually uses --------------------------------
    // The drawn drawstop and the logical switch drive EACH OTHER, so that
    // moving either moves both; the logical one then drives the node the key
    // flow reads. That mutual pair is a loop, and it must settle.
    {
      mp::OrganModel m;
      for (mp::Id id : {mp::Id{10102}, mp::Id{1006}, mp::Id{10101}}) addSwitch(m, id);
      m.switchLinkages.push_back(wire(10102, 1006));
      m.switchLinkages.push_back(wire(1006, 10102));
      m.switchLinkages.push_back(wire(1006, 10101));

      mp::SwitchNetwork net;
      net.reset(m);
      MP_CHECK(!net.engaged(10101), "the coupling node starts out");

      net.set(10102, true); // the player clicks the drawstop
      MP_CHECK(net.engaged(1006) && net.engaged(10101),
               "clicking the drawn drawstop reaches the node two wires away");
      MP_CHECK(!net.lastChangeRanAway(),
               "and the mutual pair settles rather than ringing");
      MP_CHECK(net.lastChanges().size() == 3,
               "all three switches moved, so all three fire their noises");

      net.set(10102, false);
      MP_CHECK(!net.engaged(1006) && !net.engaged(10101),
               "and letting it out takes the whole chain with it");

      // The other half of the pair drives it just as well, which is the point
      // of wiring it both ways.
      net.set(1006, true);
      MP_CHECK(net.engaged(10102) && net.engaged(10101),
               "and moving the logical switch moves the drawn one");
    }

    // --- several wires into one node -----------------------------------
    // Nancy feeds its coupler nodes from more than one place. Whichever says
    // "on" wins; the ones that say nothing must not turn it back off.
    {
      mp::OrganModel m;
      for (mp::Id id : {mp::Id{1}, mp::Id{2}, mp::Id{3}, mp::Id{9}}) addSwitch(m, id);
      m.switchLinkages.push_back(wire(1, 9));
      m.switchLinkages.push_back(wire(2, 9, /*condition*/ 3));

      mp::SwitchNetwork net;
      net.reset(m);
      net.set(1, true);
      MP_CHECK(net.engaged(9),
               "one wire asserting engage is enough, even though the other is "
               "quiet - this is what a last-writer-wins rule got wrong");

      net.set(1, false);
      MP_CHECK(!net.engaged(9), "and with nothing asserting, the node is out");

      // The conditional wire needs both its source and its condition.
      net.set(2, true);
      MP_CHECK(!net.engaged(9), "a conditional wire with its condition out does nothing");
      net.set(3, true);
      MP_CHECK(net.engaged(9), "and fires once the condition is in");
    }

    // --- an inverting wire ----------------------------------------------
    // Written as a link that fires from the source's OFF state. This is how a
    // unison off is wired, and getting the sense backwards silences a manual.
    {
      mp::OrganModel m;
      addSwitch(m, 1);
      addSwitch(m, 9);
      auto inv = wire(1, 9);
      inv.sourceWhenEngaged = false;
      m.switchLinkages.push_back(inv);

      mp::SwitchNetwork net;
      net.reset(m);
      MP_CHECK(net.engaged(9), "an inverting wire holds its node on while the source is out");
      net.set(1, true);
      MP_CHECK(!net.engaged(9), "and releases it when the source is drawn");
    }

    // --- defaults --------------------------------------------------------
    // An organ that ships with its blower running comes up that way, and the
    // default propagates through the wiring like any other state.
    {
      mp::OrganModel m;
      addSwitch(m, 1, /*defaultEngaged*/ true);
      addSwitch(m, 9);
      m.switchLinkages.push_back(wire(1, 9));

      mp::SwitchNetwork net;
      net.reset(m);
      MP_CHECK(net.engaged(1) && net.engaged(9),
               "a default-engaged switch is on at load, and so is what it drives");
      MP_CHECK(net.lastChanges().empty(),
               "and the load is a baseline, not a pile of switch noises");
    }

    // --- a wiring loop must not hang -------------------------------------
    {
      mp::OrganModel m;
      for (mp::Id id : {mp::Id{1}, mp::Id{2}, mp::Id{3}}) addSwitch(m, id);
      m.switchLinkages.push_back(wire(1, 2));
      m.switchLinkages.push_back(wire(2, 3));
      m.switchLinkages.push_back(wire(3, 1));

      mp::SwitchNetwork net;
      net.reset(m);
      MP_CHECK(!net.engaged(1), "a ring with nothing driving it stays out");
      net.set(1, true);
      MP_CHECK(net.engaged(2) && net.engaged(3),
               "and once one is set the whole ring comes on, once");
      MP_CHECK(!net.lastChangeRanAway(), "settling rather than ringing");
    }

    // --- a cancel has to be able to push a knob back out ------------------
    // The reason switches latch rather than being derived from their inputs.
    // A general cancel turns the logical stop switch off; the drawn knob is
    // wired both ways, so it follows. Derive each switch from its inputs
    // instead and the still-drawn knob turns the stop straight back on.
    {
      mp::OrganModel m;
      addSwitch(m, 10103); // the drawn knob
      addSwitch(m, 2001);  // the logical stop
      m.switchLinkages.push_back(wire(10103, 2001));
      m.switchLinkages.push_back(wire(2001, 10103));

      mp::SwitchNetwork net;
      net.reset(m);
      net.set(10103, true);
      MP_CHECK(net.engaged(2001), "clicking the knob draws the stop");

      net.set(2001, false); // what a general cancel does
      MP_CHECK(!net.engaged(2001) && !net.engaged(10103),
               "and cancelling the stop pushes the knob back out with it");
    }
  }
};

// Coupling. A Hauptwerk organ has no coupler objects: it has key-flow edges
// from a keyboard to another keyboard or to a division, each optionally gated
// on a switch. "Great to Pedal 8" is one such edge. So the whole of coupling is
// a graph walk, and the failures that matter are a manual reaching nothing (a
// silent organ) or reaching everything (an organ permanently coupled to
// itself, which is what this did before).
class CouplerGraphTest final : public mp::test::Test {
public:
  CouplerGraphTest() : Test("functional.control.key-flow", Category::Functional) {}

  // Two manuals and a pedal, wired the way a real organ is.
  static mp::OrganModel organ() {
    mp::OrganModel m;
    for (int d = 1; d <= 3; ++d) {
      mp::Division div;
      div.divisionId = d;
      div.name = d == 1 ? "Pedal" : (d == 2 ? "Great" : "Swell");
      m.divisions[d] = div;
    }
    for (int k = 1; k <= 3; ++k) {
      mp::Keyboard kb;
      kb.keyboardId = k;
      kb.assignmentCode = k;
      kb.primaryDivisionHint = k;
      kb.numKeys = k == 1 ? 32 : 61;
      kb.firstMidiNote = 36;
      m.keyboards[k] = kb;
    }
    return m;
  }

  static mp::KeyAction edge(int from, int toDivision, mp::Id condition = 0,
                            int increment = 0) {
    mp::KeyAction a;
    a.sourceKeyboard = from;
    a.destIsKeyboard = false;
    a.destDivision = toDivision;
    a.conditionSwitchId = condition;
    a.midiIncrement = increment;
    return a;
  }

  static bool reaches(const std::vector<mp::ExpandedNote>& v, int div, int note) {
    for (const auto& n : v)
      if (n.divisionId == div && n.midiNote == note) return true;
    return false;
  }

  void run() override {
    // --- the plain case ---------------------------------------------------
    {
      auto m = organ();
      // Great to Pedal 8, the commonest coupler there is.
      m.keyActions.push_back(edge(1, 2, /*condition*/ 500));
      mp::CouplerMatrix flow;
      flow.reset(m);

      MP_CHECK(!flow.usingFallback(), "the organ declares key flow");
      MP_CHECK(flow.inputKeyboards().size() == 3,
               "all three manuals are playable");

      const std::unordered_set<mp::Id> nothing;
      auto pedal = flow.expand(1, 40, 1.0f, nothing);
      MP_CHECK(pedal.size() == 1 && reaches(pedal, 1, 40),
               "a pedal key sounds the pedal division and nothing else - "
               "this is the whole point: an uncoupled manual is separate");

      auto great = flow.expand(2, 60, 1.0f, nothing);
      MP_CHECK(great.size() == 1 && reaches(great, 2, 60),
               "and the Great sounds only the Great");

      const std::unordered_set<mp::Id> coupled{500};
      auto both = flow.expand(1, 40, 1.0f, coupled);
      MP_CHECK(both.size() == 2 && reaches(both, 1, 40) && reaches(both, 2, 40),
               "drawing the coupler adds the Great to the pedal key");
      MP_CHECK(flow.expand(2, 60, 1.0f, coupled).size() == 1,
               "and does not couple the other way round");
    }

    // --- transposition ----------------------------------------------------
    {
      auto m = organ();
      m.keyActions.push_back(edge(2, 2, 501, -12)); // sub-octave on itself
      m.keyActions.push_back(edge(2, 3, 502, 12));  // Swell to Great 4'
      mp::CouplerMatrix flow;
      flow.reset(m);

      auto sub = flow.expand(2, 60, 1.0f, {501});
      MP_CHECK(reaches(sub, 2, 60) && reaches(sub, 2, 48),
               "a sub-octave coupler sounds the same division an octave down");
      auto oct = flow.expand(2, 60, 1.0f, {502});
      MP_CHECK(reaches(oct, 3, 72),
               "and a 4 foot coupler sounds the other division an octave up");

      // Off the end of the compass is silence, not a wrapped note.
      auto high = flow.expand(2, 120, 1.0f, {502});
      MP_CHECK(!reaches(high, 3, 132) && reaches(high, 2, 120),
               "a coupler that runs off the top of MIDI simply does not sound");
    }

    // --- the inverted sense (unison off) ----------------------------------
    {
      auto m = organ();
      m.keyboards[2].primaryDivisionHint = 0; // reached only through the edge
      auto off = edge(2, 2, 503);
      off.conditionWhenEngaged = false; // live while the switch is NOT engaged
      m.keyActions.push_back(off);
      mp::CouplerMatrix flow;
      flow.reset(m);

      MP_CHECK(flow.expand(2, 60, 1.0f, {}).size() == 1,
               "a unison-off edge is live while its switch is out");
      const auto silenced = flow.expand(2, 60, 1.0f, {503});
      MP_CHECK(!reaches(silenced, 2, 60),
               "and drawing the switch takes the division away");
    }

    // --- the key window ---------------------------------------------------
    {
      auto m = organ();
      auto a = edge(1, 2, 0);
      a.firstSourceNote = 36;
      a.numKeys = 32; // a 32-note pedalboard
      m.keyboards[1].primaryDivisionHint = 0;
      m.keyActions.push_back(a);
      mp::CouplerMatrix flow;
      flow.reset(m);
      MP_CHECK(reaches(flow.expand(1, 60, 1.0f, {}), 2, 60),
               "a key inside the window carries");
      MP_CHECK(flow.expand(1, 90, 1.0f, {}).empty(),
               "and a key past the end of a 32-note pedalboard sounds nothing, "
               "which is what a pedalboard that short does");
    }

    // --- a cycle must not hang the audio thread ---------------------------
    {
      auto m = organ();
      mp::KeyAction there, back;
      there.sourceKeyboard = 2;
      there.destIsKeyboard = true;
      there.destKeyboard = 3;
      back.sourceKeyboard = 3;
      back.destIsKeyboard = true;
      back.destKeyboard = 2;
      m.keyActions.push_back(there);
      m.keyActions.push_back(back);
      mp::CouplerMatrix flow;
      flow.reset(m);
      const auto out = flow.expand(2, 60, 1.0f, {});
      MP_CHECK(reaches(out, 2, 60) && reaches(out, 3, 60),
               "a mutual coupling sounds both divisions once");
      MP_CHECK(out.size() == 2, "and does not sound either of them twice");
    }

    // --- an internal hub keyboard inherits its division -------------------
    // The shape a full-size organ actually uses: the manual feeds an internal
    // keyboard unison, every coupler leaves from there, and that keyboard
    // names no division of its own.
    {
      auto m = organ();
      mp::Keyboard hub;
      hub.keyboardId = 9;
      hub.accessibleForInput = false;
      m.keyboards[9] = hub;

      mp::KeyAction unison;
      unison.sourceKeyboard = 2;
      unison.destIsKeyboard = true;
      unison.destKeyboard = 9;
      m.keyActions.push_back(unison);
      m.keyboards[2].primaryDivisionHint = 2;
      m.keyActions.push_back(edge(9, 3, 504)); // Swell to Great, off the hub

      mp::CouplerMatrix flow;
      flow.reset(m);
      MP_CHECK(flow.inputKeyboards().size() == 3,
               "the hub is not something a player plays on");
      const auto out = flow.expand(2, 60, 1.0f, {504});
      MP_CHECK(reaches(out, 2, 60) && reaches(out, 3, 60),
               "a coupler taken off the hub still sounds");
    }

    // --- an organ that declares nothing is never silent --------------------
    {
      auto m = organ();
      mp::CouplerMatrix flow;
      flow.reset(m);
      MP_CHECK(flow.usingFallback(), "no key flow declared");
      MP_CHECK(flow.expand(2, 60, 1.0f, {}).size() == 3,
               "so every division sounds - too much, but never nothing");
    }
  }
};

class DefaultKeyboardTest final : public mp::test::Test {
 public:
  DefaultKeyboardTest()
      : Test("functional.control.default-keyboard", Category::Functional) {}

  // The Nancy shape: pedal + two manuals, no compass declared anywhere, the
  // manual with more pipework enclosed, the smaller one open.
  static mp::OrganModel organ() {
    mp::OrganModel m;
    for (int d = 1; d <= 3; ++d) {
      mp::Division div;
      div.divisionId = d;
      div.name = d == 1 ? "Pedal" : (d == 2 ? "Great" : "Swell");
      m.divisions[d] = div;
    }
    for (int k = 1; k <= 3; ++k) {
      mp::Keyboard kb;
      kb.keyboardId = k;
      kb.assignmentCode = k;
      kb.primaryDivisionHint = k;
      kb.numKeys = 0; // undeclared: the Nancy shape
      m.keyboards[k] = kb;
    }
    auto rankWith = [&](mp::Id id, int pipes) {
      mp::Rank r;
      r.rankId = id;
      for (int i = 0; i < pipes; ++i) {
        mp::Pipe p;
        p.pipeId = id * 100 + i;
        r.pipes.push_back(p);
      }
      m.ranks[id] = r;
    };
    rankWith(1, 4);  // pedal: one small rank
    rankWith(2, 10); // great: the most open pipework
    rankWith(3, 40); // swell: more pipes, but enclosed
    auto stopOn = [&](mp::Id id, int div, mp::Id rank) {
      mp::Stop s;
      s.stopId = id;
      s.divisionId = div;
      mp::StopRankEntry e;
      e.rankId = rank;
      s.ranks.push_back(e);
      m.stops[id] = s;
    };
    stopOn(1, 1, 1);
    stopOn(2, 2, 2);
    stopOn(3, 3, 3);
    // One swell pipe in a box marks the whole division enclosed.
    m.pipeEnclosure[300] = 7;
    mp::Enclosure enc;
    enc.enclosureId = 7;
    m.enclosures[7] = enc;
    return m;
  }

  void run() override {
    {
      auto m = organ();
      mp::CouplerMatrix flow;
      flow.reset(m);
      MP_CHECK(mp::defaultKeyboard(m, flow) == 2,
               "the unenclosed Great wins over a larger enclosed Swell and "
               "the pedal when no compass is declared");
      MP_CHECK(!mp::hasDrawnManuals(m), "no KeyImageSets means backdrop manuals");
    }
    {
      // A declared compass keeps the old answer: widest wins.
      auto m = organ();
      m.keyboards[1].numKeys = 32;
      m.keyboards[2].numKeys = 61;
      m.keyboards[3].numKeys = 61;
      mp::CouplerMatrix flow;
      flow.reset(m);
      MP_CHECK(mp::defaultKeyboard(m, flow) == 2,
               "widest compass still wins when the organ declares one");
    }
    {
      // Nothing playable anywhere: the first manual, never the pedal.
      mp::OrganModel m;
      mp::Division d;
      d.divisionId = 1;
      m.divisions[1] = d;
      for (int k = 1; k <= 2; ++k) {
        mp::Keyboard kb;
        kb.keyboardId = k;
        kb.assignmentCode = k;
        kb.primaryDivisionHint = 1;
        m.keyboards[k] = kb;
      }
      mp::CouplerMatrix flow;
      flow.reset(m);
      MP_CHECK(mp::defaultKeyboard(m, flow) == 2,
               "with nothing to score, fall back to the first manual");
    }
    {
      auto m = organ();
      m.keyboards[2].keyImageSetId = 9;
      MP_CHECK(mp::hasDrawnManuals(m), "one KeyImageSet means drawn manuals");
    }
  }
};

// Console LCD panels. Three things can go wrong here and only one of them is
// cosmetic: a high byte truncates the message, an un-padded short value leaves
// the tail of the previous one on the glass, and re-sending unchanged text
// floods a 31250-baud wire.
class LcdPanelTest final : public mp::test::Test {
public:
  LcdPanelTest() : Test("functional.midi.lcd-panels", Category::Functional) {}

  static mp::LcdPanels twoLine() {
    mp::LcdPanels p;
    mp::LcdPanel panel;
    panel.hardwareId = 3;
    panel.lineWidth = 16;
    panel.lines.push_back({mp::LcdField::OrganName, ""});
    panel.lines.push_back({mp::LcdField::Temperament, "Temp: "});
    p.addPanel(panel);
    return p;
  }

  void run() override {
    // --- seven-bit safety -------------------------------------------------
    // The whole point. Sysex ends at the first byte with the high bit set, so
    // an accented organ name would cut the message off mid-word and leave the
    // rest of it to be read as live MIDI.
    {
      const std::string folded =
          mp::LcdPanels::toDisplayAscii("Kraków Müller Dvořák");
      MP_CHECK(folded == "Krakow Mueller Dvorak",
               "accents fold to ASCII: got '" + folded + "'");
      for (char c : folded)
        MP_CHECK(static_cast<unsigned char>(c) < 0x80, "no high bytes survive");
    }

    // A name we have no folding for must still not break the stream.
    {
      const std::string cjk = mp::LcdPanels::toDisplayAscii("音");
      MP_CHECK(cjk == "?", "unfoldable becomes a placeholder, not a high byte");
    }

    // --- padding and truncation ------------------------------------------
    // A panel does not blank what it is not sent. "Valotti" replacing
    // "Werckmeister III" must overwrite the leftovers.
    {
      mp::LcdState s;
      s.temperament = "Valotti";
      const std::string line = mp::LcdPanels::renderLine(
          {mp::LcdField::Temperament, "Temp: "}, s, 16);
      MP_CHECK(line == "Temp: Valotti   ",
               "short value pads to width: '" + line + "'");

      s.temperament = "Werckmeister III";
      const std::string cut = mp::LcdPanels::renderLine(
          {mp::LcdField::Temperament, "Temp: "}, s, 16);
      MP_CHECK(cut.size() == 16 && cut == "Temp: Werckmeist",
               "long value truncates to width: '" + cut + "'");
    }

    // Transpose is the one field where the sign carries the meaning.
    {
      mp::LcdState s;
      s.transpose = 2;
      MP_CHECK(mp::LcdPanels::renderLine({mp::LcdField::Transpose, ""}, s, 4) ==
                   "+2  ", "up shows a plus");
      s.transpose = -3;
      MP_CHECK(mp::LcdPanels::renderLine({mp::LcdField::Transpose, ""}, s, 4) ==
                   "-3  ", "down shows a minus");
      s.transpose = 0;
      MP_CHECK(mp::LcdPanels::renderLine({mp::LcdField::Transpose, ""}, s, 4) ==
                   "0   ", "at pitch shows a bare zero");
    }

    // --- message framing --------------------------------------------------
    {
      auto p = twoLine();
      mp::LcdState s;
      s.organName = "Melcer";
      s.temperament = "Valotti";
      auto msgs = p.update(s);
      MP_CHECK(msgs.size() == 2, "both lines go out on the first update");

      const auto& m = msgs[0];
      MP_CHECK(m.front() == 0xF0 && m.back() == 0xF7, "framed F0..F7");
      MP_CHECK(m.size() == 1 + 1 + 1 + 1 + 16 + 1,
               "header, hardware id, line index, 16 chars");
      MP_CHECK(m[1] == 0x7D, "default manufacturer id is the reserved one");
      MP_CHECK(m[2] == 3, "hardware id addresses the panel");
      MP_CHECK(m[3] == 0 && msgs[1][3] == 1, "lines are numbered in order");
      for (size_t i = 1; i + 1 < m.size(); ++i)
        MP_CHECK(m[i] < 0x80, "every payload byte is seven-bit");
    }

    // --- only what changed ------------------------------------------------
    // The reason update() caches: a full 16-character refresh is ~6 ms of wire
    // time, and re-sending a steady temperament every block would fill it.
    {
      auto p = twoLine();
      mp::LcdState s;
      s.organName = "Melcer";
      s.temperament = "Valotti";
      MP_CHECK(p.update(s).size() == 2, "first update sends everything");
      MP_CHECK(p.update(s).empty(), "an unchanged state sends nothing");

      s.temperament = "Kirnberger III";
      auto moved = p.update(s);
      MP_CHECK(moved.size() == 1 && moved[0][3] == 1,
               "only the line that moved is re-sent");

      // Fields the panel does not show must not cause traffic either.
      s.stopsDrawn = 12;
      MP_CHECK(p.update(s).empty(), "an unshown field sends nothing");

      MP_CHECK(p.refreshAll(s).size() == 2,
               "refreshAll re-sends regardless, for a console plugged in late");
    }

    // --- the step readout the crescendo line needs ------------------------
    // A crossing and a position are different things, and the bank is built
    // around crossings. stepAt must answer the position question without
    // disturbing any of that.
    {
      mp::OrganModel m;
      auto add = [&](mp::Id ctrl, int value, mp::Id sw, bool up, bool down) {
        mp::ContinuousControlStageSwitch row;
        row.controlId = ctrl;
        row.value = value;
        row.controlledSwitchId = sw;
        row.engageWhenIncreasing = up;
        row.engageWhenDecreasing = down;
        row.disengageWhenIncreasing = up;
        row.disengageWhenDecreasing = down;
        m.controlStageSwitches.push_back(row);
      };
      // A five-step crescendo on 51. All four flags, which is how a real one
      // is written — Cracow's 55 rows every one of them — because the step you
      // land on has to be the same whichever way the shoe was travelling.
      for (int i = 0; i < 5; ++i) add(51, 20 * (i + 1), 900 + i, true, true);
      // ... and a blower on 60, which is two rows and must not be mistaken for
      // a crescendo. Its second row acts on the way DOWN only: the hysteresis
      // case, where one switch has a start threshold and a lower stop one.
      add(60, 120, 800, true, false);
      add(60, 126, 800, false, true);

      mp::StageSwitchBank bank;
      bank.reset(m);
      std::vector<mp::StageSwitchBank::Change> out;

      MP_CHECK(bank.crescendoControl() == 51,
               "the control with the most steps is the crescendo");
      MP_CHECK(bank.stepMax(51) == 5, "five engaging rows, five steps");
      MP_CHECK(bank.stepMax(60) == 1,
               "a down-going row is not counted as a step");

      // The case that made this a readout of engaged state rather than of the
      // control's value: a control sitting at 127 having crossed nothing is on
      // no step at all. Cracow loads exactly like this, and counting
      // thresholds reported step 49 of an organ with no stops drawn.
      MP_CHECK(bank.currentStep(51) == 0, "nothing crossed is step 0");

      bank.moveControl(51, 0, 45, out);
      MP_CHECK(bank.currentStep(51) == 2, "two thresholds crossed is step 2");
      out.clear();
      bank.moveControl(51, 45, 127, out);
      MP_CHECK(bank.currentStep(51) == 5, "a fully swept shoe is the last step");
      out.clear();
      bank.moveControl(51, 127, 0, out);
      MP_CHECK(bank.currentStep(51) == 1,
               "sweeping back down lands on the lowest step, not on none");
      MP_CHECK(bank.currentStep(999) == 0, "a control with no rows reads 0");
    }

    // --- a header we cannot validate is still validated for legality ------
    {
      mp::LcdPanels p;
      MP_CHECK(!p.setHeader({0x41, 0x90}), "a high byte is refused");
      MP_CHECK(!p.setHeader({}), "an empty header is refused");
      MP_CHECK(p.setHeader({0x41, 0x10, 0x00}), "a legal vendor prefix takes");
      MP_CHECK(p.header().size() == 3, "and is kept");
    }
  }
};

// What reaches a manual. A channel number is nowhere near enough for a real
// rig, so this follows GrandOrgue's manual receiver: device, channel, key
// range, transpose, velocity window, tracker action, short octave, debounce.
// Every one of those exists because some console does that.
class KeyboardBindingTest final : public mp::test::Test {
public:
  KeyboardBindingTest()
    : Test("functional.midi.manual-assign", Category::Functional) {}

  static mp::MidiMap::KeyboardBinding whole(mp::Id kb, int channel = 0) {
    mp::MidiMap::KeyboardBinding b;
    b.keyboardId = kb;
    b.channel = channel;
    return b;
  }

  static bool hit(const std::vector<mp::MidiMap::KeyHit>& v, mp::Id kb,
                  int note, bool on = true) {
    for (const auto& h : v)
      if (h.keyboardId == kb && h.midiNote == note && h.on == on) return true;
    return false;
  }

  void run() override {
    std::vector<mp::MidiMap::KeyHit> out;

    // --- splitting one keyboard across two manuals ------------------------
    // The reason a key RANGE exists. One physical board, bottom half to the
    // pedal, top half to the Great, each landing on its own compass.
    {
      mp::MidiMap m;
      auto lower = whole(1);
      lower.lowKey = 36;
      lower.highKey = 59;
      lower.transpose = 0;
      m.addKeyboardBinding(lower);

      auto upper = whole(2);
      upper.lowKey = 60;
      upper.highKey = 96;
      upper.transpose = -24; // back down onto the manual's own compass
      m.addKeyboardBinding(upper);

      out.clear();
      MP_CHECK(m.matchKeyboards(0, 1, 40, 100, 0.0, out) == 1 &&
                   hit(out, 1, 40),
               "a key in the lower half plays the lower manual");
      out.clear();
      MP_CHECK(m.matchKeyboards(0, 1, 72, 100, 0.0, out) == 1 &&
                   hit(out, 2, 48),
               "and one in the upper half plays the other, transposed onto it");
      out.clear();
      MP_CHECK(m.matchKeyboards(0, 1, 120, 100, 0.0, out) == 0,
               "a key above both ranges reaches nothing");
    }

    // --- one key, two manuals ---------------------------------------------
    // Legitimate and deliberate: the same press can be sent to two manuals.
    {
      mp::MidiMap m;
      m.addKeyboardBinding(whole(1));
      m.addKeyboardBinding(whole(2));
      out.clear();
      MP_CHECK(m.matchKeyboards(0, 1, 60, 100, 0.0, out) == 2,
               "a key mapped to two manuals reaches both");
    }

    // --- devices -----------------------------------------------------------
    {
      mp::MidiMap m;
      auto a = whole(1, 1);
      a.deviceId = 7;
      auto b = whole(2, 1);
      b.deviceId = 9;
      m.addKeyboardBinding(a);
      m.addKeyboardBinding(b);

      out.clear();
      MP_CHECK(m.matchKeyboards(7, 1, 60, 100, 0.0, out) == 1 && hit(out, 1, 60),
               "two consoles on the same channel play different manuals - "
               "which is the whole reason a message carries its device");
      out.clear();
      MP_CHECK(m.matchKeyboards(9, 1, 60, 100, 0.0, out) == 1 && hit(out, 2, 60),
               "and the other one plays the other");
      out.clear();
      MP_CHECK(m.matchKeyboards(3, 1, 60, 100, 0.0, out) == 0,
               "a third console that is not mapped plays nothing");
    }

    // --- channels no binding claims ---------------------------------------
    // Friesach's saved map bound three of four manuals (pedal ch1, third ch3,
    // fourth ch4) and left the main manual's channel unclaimed. Every note
    // there was dropped once ANY binding existed, silencing the manual, the
    // on-screen keys on it and any file playing through it. Unclaimed
    // channels keep the default assignment instead.
    {
      mp::MidiMap m;
      m.addKeyboardBinding(whole(1, 1));
      m.addKeyboardBinding(whole(3, 3));
      m.addKeyboardBinding(whole(4, 4));
      MP_CHECK(m.hasChannelBinding(0, 1) && m.hasChannelBinding(0, 3) &&
                   m.hasChannelBinding(0, 4),
               "claimed channels are claimed");
      MP_CHECK(!m.hasChannelBinding(0, 2),
               "the unmapped main manual's channel is not claimed");
      // The on-screen keys arrive with no device: a device-specific binding
      // must not claim them.
      mp::MidiMap d;
      auto pinned = whole(1, 1);
      pinned.deviceId = 7;
      d.addKeyboardBinding(pinned);
      MP_CHECK(d.hasChannelBinding(7, 1), "the pinned console is claimed");
      MP_CHECK(!d.hasChannelBinding(0, 1),
               "the on-screen keys are not claimed by a device-specific binding");
      // A fresh any-channel binding claims everything.
      mp::MidiMap a;
      a.addKeyboardBinding(whole(1));
      MP_CHECK(a.hasChannelBinding(0, 9) && a.hasChannelBinding(5, 16),
               "an any-channel binding claims every channel");
      // Nothing mapped claims nothing.
      mp::MidiMap e;
      MP_CHECK(!e.hasChannelBinding(0, 1), "an empty map claims nothing");
    }

    // --- velocity ----------------------------------------------------------
    {
      mp::MidiMap m;
      auto b = whole(1);
      b.lowVelocity = 20;
      b.highVelocity = 100;
      m.addKeyboardBinding(b);

      out.clear();
      m.matchKeyboards(0, 1, 60, 10, 0.0, out);
      MP_CHECK(!out.empty() && !out.front().on,
               "below the window the key is not down");
      out.clear();
      m.matchKeyboards(0, 1, 60, 100, 0.0, out);
      MP_CHECK(!out.empty() && out.front().on && out.front().velocity == 127,
               "at the top of the window it is full velocity - a console that "
               "never sends more than 100 still reaches the loudest layer");

      // Tracker action: down or not down, and nothing in between.
      mp::MidiMap t;
      auto tb = whole(1);
      tb.ignoreVelocity = true;
      t.addKeyboardBinding(tb);
      out.clear();
      t.matchKeyboards(0, 1, 60, 3, 0.0, out);
      MP_CHECK(!out.empty() && out.front().velocity == 127,
               "ignoring velocity reports full, not what the console sent");

      // An inverted window reverses the sense, which is how a
      // normally-closed key contact is read.
      mp::MidiMap inv;
      auto ib = whole(1);
      ib.lowVelocity = 127;
      ib.highVelocity = 1;
      inv.addKeyboardBinding(ib);
      out.clear();
      inv.matchKeyboards(0, 1, 60, 100, 0.0, out);
      MP_CHECK(!out.empty() && !out.front().on,
               "an inverted velocity window reads a press as a release");
    }

    // --- short octave ------------------------------------------------------
    // Historic keyboards whose bottom octave has no accidentals and puts other
    // notes on those keys.
    {
      mp::MidiMap m;
      auto b = whole(1);
      b.lowKey = 36;
      b.shortOctave = true;
      m.addKeyboardBinding(b);

      out.clear();
      MP_CHECK(m.matchKeyboards(0, 1, 36, 100, 0.0, out) == 0,
               "the first four keys of a short octave are dead");
      out.clear();
      MP_CHECK(m.matchKeyboards(0, 1, 40, 100, 0.0, out) == 1 &&
                   hit(out, 1, 36),
               "and the fifth sounds four semitones below where it looks");
      out.clear();
      MP_CHECK(m.matchKeyboards(0, 1, 41, 100, 0.0, out) == 1 &&
                   hit(out, 1, 41),
               "while its neighbour is where it looks");
    }

    // --- debounce ----------------------------------------------------------
    // Old contacts chatter. Without this one press retriggers the pipe.
    {
      mp::MidiMap m;
      auto b = whole(1);
      b.debounceMs = 50;
      m.addKeyboardBinding(b);

      out.clear();
      MP_CHECK(m.matchKeyboards(0, 1, 60, 100, 1000.0, out) == 1,
               "the first press sounds");
      out.clear();
      MP_CHECK(m.matchKeyboards(0, 1, 60, 100, 1010.0, out) == 0,
               "a bounce ten milliseconds later does not");
      out.clear();
      MP_CHECK(m.matchKeyboards(0, 1, 60, 100, 1100.0, out) == 1,
               "and a real second press, later, does");
    }

    // --- transposed off the end --------------------------------------------
    {
      mp::MidiMap m;
      auto b = whole(1);
      b.transpose = 60;
      m.addKeyboardBinding(b);
      out.clear();
      MP_CHECK(m.matchKeyboards(0, 1, 120, 100, 0.0, out) == 0,
               "a note transposed past the top of MIDI simply does not sound");
    }

    // --- saved and reloaded ------------------------------------------------
    {
      mp::MidiMap m;
      auto b = whole(3, 2);
      b.lowKey = 48;
      b.highKey = 84;
      b.transpose = -12;
      b.lowVelocity = 10;
      b.highVelocity = 110;
      b.ignoreVelocity = true;
      b.shortOctave = true;
      b.debounceMs = 25;
      b.deviceId = m.devices().idFor("My Console 61");
      m.addKeyboardBinding(b);

      mp::MidiMap loaded;
      MP_CHECK(loaded.fromText(m.toText()),
               "a manual assignment survives being written and read back");
      MP_CHECK(loaded.keyboardBindings().size() == 1, "one binding came back");
      const auto& r = loaded.keyboardBindings().front();
      MP_CHECK(r.keyboardId == 3 && r.channel == 2 && r.lowKey == 48 &&
                   r.highKey == 84 && r.transpose == -12 &&
                   r.lowVelocity == 10 && r.highVelocity == 110 &&
                   r.ignoreVelocity && r.shortOctave && r.debounceMs == 25,
               "with every field intact");
      MP_CHECK(loaded.devices().nameFor(r.deviceId) == "My Console 61",
               "and the console named rather than numbered, so replugging it "
               "into another port does not scramble the mapping");
    }
  }
};

class MidiMapTest final : public mp::test::Test {
public:
  MidiMapTest() : Test("functional.midi.map", Category::Functional) {}

  static mp::MidiSource cc(int number, int channel = 0) {
    return {mp::MidiSourceKind::ControlChange, channel, number};
  }
  static mp::MidiSource note(int number, int channel = 0) {
    return {mp::MidiSourceKind::Note, channel, number};
  }

  void run() override {
    mp::MidiMap map;
    MP_CHECK(map.size() == 0, "a new map is empty");
    MP_CHECK(!map.actionFor(cc(11), 64).valid(),
             "an unmapped message does nothing");

    // --- a shoe ---------------------------------------------------------
    mp::MidiBinding shoe;
    shoe.source = cc(11);
    shoe.targetKind = mp::MidiTargetKind::ContinuousControl;
    shoe.targetId = 1;
    map.bind(shoe);

    auto a = map.actionFor(cc(11), 100);
    MP_CHECK(a.kind == mp::MidiTargetKind::ContinuousControl && a.targetId == 1,
             "a CC drives the control it is bound to");
    MP_CHECK(a.value == 100, "the controller value passes through");
    MP_CHECK(map.actionFor(cc(11), 999).value == 127,
             "an out-of-range value is clamped, not wrapped");

    // A shoe wired backwards is common; inverting beats rewiring.
    shoe.invert = true;
    map.bind(shoe);
    MP_CHECK(map.actionFor(cc(11), 0).value == 127,
             "an inverted shoe reads open when the pedal is up");

    // --- a latching drawstop -------------------------------------------
    mp::MidiBinding stop;
    stop.source = note(36, 3);
    stop.targetKind = mp::MidiTargetKind::Switch;
    stop.targetId = 42;
    stop.latching = true;
    map.bind(stop);

    auto on = map.actionFor(note(36, 3), 100);
    MP_CHECK(on.kind == mp::MidiTargetKind::Switch && on.engage,
             "the first press engages a latching stop");
    MP_CHECK(!map.actionFor(note(36, 3), 0).valid(),
             "a latching stop ignores the release");
    MP_CHECK(!map.actionFor(note(36, 3), 100).engage,
             "the second press disengages it");

    // --- a momentary piston ---------------------------------------------
    mp::MidiBinding piston;
    piston.source = note(40, 3);
    piston.targetKind = mp::MidiTargetKind::Switch;
    piston.targetId = 43;
    piston.latching = false;
    map.bind(piston);
    MP_CHECK(map.actionFor(note(40, 3), 127).engage, "a piston is on while held");
    MP_CHECK(!map.actionFor(note(40, 3), 0).engage, "and off when released");

    // --- channels --------------------------------------------------------
    MP_CHECK(!map.actionFor(note(36, 5), 100).valid(),
             "a binding on channel 3 does not fire on channel 5");
    mp::MidiBinding anyChannel;
    anyChannel.source = cc(7); // channel 0 means any
    anyChannel.targetKind = mp::MidiTargetKind::ContinuousControl;
    anyChannel.targetId = 9;
    map.bind(anyChannel);
    MP_CHECK(map.actionFor(cc(7, 12), 50).targetId == 9,
             "a wildcard-channel binding fires on any channel");

    // --- learning --------------------------------------------------------
    MP_CHECK(!map.learning(), "not learning by default");
    map.beginLearn(mp::MidiTargetKind::Switch, 77, true);
    MP_CHECK(map.learning() && map.learningTarget() == 77, "learn armed");
    MP_CHECK(map.learnFrom(cc(64)), "the next message completes the binding");
    MP_CHECK(!map.learning(), "learning ends after one message");
    MP_CHECK(map.actionFor(cc(64), 127).targetId == 77,
             "the learned control now drives its target");

    // Re-learning a target MOVES it rather than stacking a second meaning.
    map.beginLearn(mp::MidiTargetKind::Switch, 77, true);
    map.learnFrom(cc(65));
    MP_CHECK(!map.actionFor(cc(64), 127).valid(),
             "the old binding is gone after re-learning");
    MP_CHECK(map.actionFor(cc(65), 127).targetId == 77, "the new one works");

    map.beginLearn(mp::MidiTargetKind::Switch, 88, true);
    map.cancelLearn();
    MP_CHECK(!map.learning(), "learning can be cancelled");

    // One source drives one target: re-using a button reassigns it.
    mp::MidiBinding reuse;
    reuse.source = cc(65);
    reuse.targetKind = mp::MidiTargetKind::ContinuousControl;
    reuse.targetId = 5;
    map.bind(reuse);
    MP_CHECK(map.actionFor(cc(65), 60).kind == mp::MidiTargetKind::ContinuousControl,
             "binding a used source replaces its meaning");

    // --- persistence -----------------------------------------------------
    const std::string saved = map.toText();
    MP_CHECK(!saved.empty(), "the map serialises");
    mp::MidiMap loaded;
    MP_CHECK(loaded.fromText(saved), "and loads back without complaint");
    MP_CHECK(loaded.size() == map.size(), "with the same number of bindings");
    MP_CHECK(loaded.actionFor(cc(11), 0).value == 127,
             "including the inverted shoe");
    MP_CHECK(loaded.actionFor(note(40, 3), 127).engage,
             "and the momentary piston");

    // A damaged line loses one binding, not the whole map.
    mp::MidiMap partial;
    MP_CHECK(!partial.fromText("cc 0 11 control 1 1 0\nthis is not a binding\n"),
             "a malformed line is reported");
    MP_CHECK(partial.size() == 1, "but the good bindings survive");

    map.clear();
    MP_CHECK(map.size() == 0 && !map.actionFor(cc(11), 64).valid(),
             "clearing removes everything");
  }
};

class ContinuousControlBankTest final : public mp::test::Test {
public:
  ContinuousControlBankTest()
    : Test("functional.expression.control-bank", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(loadFixture("m24.expression.Organ_Hauptwerk_xml", m, d),
             "expression fixture must load");

    mp::ContinuousControlBank bank;
    bank.reset(m);
    MP_CHECK(bank.size() == m.continuousControls.size(),
             "every control gets a runtime slot");

    // Defaults come from the ODF, and a shoe reads 0 when closed.
    MP_CHECK(bank.value(1) == 0, "shoe starts at its declared default");
    MP_CHECK(std::fabs(bank.normalised(1)) < 1e-9, "a closed shoe reads 0.0");

    bank.setValue(1, 127);
    MP_CHECK(std::fabs(bank.normalised(1) - 1.0) < 1e-9, "an open shoe reads 1.0");
    bank.setValue(1, 64);
    MP_CHECK(std::fabs(bank.normalised(1) - 64.0 / 127.0) < 1e-9,
             "a half-open shoe reads halfway");

    // Out-of-range input is clamped, never wrapped: a stray MIDI value must not
    // slam a shoe from open to shut.
    bank.setValue(1, 9999);
    MP_CHECK(bank.value(1) == 127, "above-range input clamps to the maximum");
    bank.setValue(1, -50);
    MP_CHECK(bank.value(1) == 0, "below-range input clamps to the minimum");

    // Unknown controls are inert rather than fatal.
    bank.setValue(4242, 100);
    MP_CHECK(bank.value(4242) == 0, "writing an unknown control is ignored");
    MP_CHECK(std::fabs(bank.normalised(4242)) < 1e-9,
             "an unknown control reads 0.0");

    // Linkage: control 1 drives control 2 at half scale.
    bank.setValue(1, 100);
    bank.propagate();
    MP_CHECK(bank.value(2) == 50,
             "a linkage carries the scaled value to its destination");
    bank.setValue(1, 0);
    bank.propagate();
    MP_CHECK(bank.value(2) == 0, "the linkage follows the source back down");

    // A cyclic linkage pair must settle rather than spin: the loader already
    // flagged 4<->5, and the bank must still terminate.
    bank.setValue(4, 77);
    bank.propagate();
    MP_CHECK(bank.value(4) >= 0 && bank.value(4) <= 127,
             "a cyclic linkage settles inside the control range");

    // Enclosure shutter lookup is what the audio graph actually calls.
    bank.setValue(1, 127);
    bank.propagate();
    MP_CHECK(std::fabs(bank.shutterFor(m.enclosures.at(1)) - 1.0) < 1e-9,
             "an open shoe opens the box it drives");
    bank.setValue(1, 0);
    bank.propagate();
    MP_CHECK(std::fabs(bank.shutterFor(m.enclosures.at(1))) < 1e-9,
             "a closed shoe closes the box it drives");

    // An enclosure with no control must read fully OPEN: an unexpressive rank
    // is a far smaller fault than a silent one.
    mp::Enclosure unbound;
    unbound.enclosureId = 99;
    MP_CHECK(std::fabs(bank.shutterFor(unbound) - 1.0) < 1e-9,
             "an enclosure with no shoe is treated as open, never as silent");

    // An inverted control reports position, not raw value, so a reversed shoe
    // still reads 1.0 when it is open.
    mp::OrganModel inv;
    mp::ContinuousControl c;
    c.controlId = 1;
    c.maxValue = 127;
    c.inverted = true;
    inv.continuousControls[c.controlId] = c;
    mp::ContinuousControlBank invBank;
    invBank.reset(inv);
    invBank.setValue(1, 0);
    MP_CHECK(std::fabs(invBank.normalised(1) - 1.0) < 1e-9,
             "an inverted control at 0 reads fully open");
    invBank.setValue(1, 127);
    MP_CHECK(std::fabs(invBank.normalised(1)) < 1e-9,
             "an inverted control at 127 reads fully closed");
  }
};

class ExpressionTablesTest final : public mp::test::Test {
public:
  ExpressionTablesTest()
    : Test("functional.expression.tables", Category::Functional) {}

  static bool contains(const std::vector<mp::Id>& v, mp::Id id) {
    return std::find(v.begin(), v.end(), id) != v.end();
  }

  void run() override {
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(loadFixture("m24.expression.Organ_Hauptwerk_xml", m, d),
             "expression fixture must load");
    MP_CHECK(d.errors.empty(), "expression fixture has no fatal errors");

    // --- enclosures ---
    MP_CHECK(m.enclosures.size() == 2, "both enclosures parsed");
    const auto& swell = m.enclosures.at(1);
    MP_CHECK(swell.name == "Swell Box", "enclosure name parsed");
    MP_CHECK(swell.continuousControlId == 1, "enclosure bound to its shoe");
    MP_CHECK(std::fabs(swell.closedFilterHz - 700.0) < 1e-9,
             "closed shade cutoff parsed");
    MP_CHECK(std::fabs(swell.openFilterHz - 14000.0) < 1e-9,
             "open shade cutoff parsed");
    MP_CHECK(std::fabs(swell.closedAttnDb + 20.0) < 1e-9,
             "closed attenuation parsed");
    MP_CHECK(swell.numShades == 2, "EnclosurePipe rows counted");

    // Per-rank expression: the pipe->enclosure map is what stops a swell box
    // filtering the whole organ.
    MP_CHECK(m.pipeEnclosure.count(9101) == 1, "an enclosed pipe is mapped");
    MP_CHECK(m.pipeEnclosure.at(9101) == 1, "mapped to the right box");
    MP_CHECK(m.pipeEnclosure.count(9102) == 0,
             "a pipe no box encloses stays unenclosed");
    // 9103 is named by an EnclosurePipe row but no such pipe exists; the map
    // records the intent rather than dropping the row silently.
    MP_CHECK(m.pipeEnclosure.count(9103) == 1,
             "an EnclosurePipe row is honoured even before its pipe parses");
    MP_CHECK(contains(d.enclosuresWithoutShades, 2),
             "an enclosure that encloses nothing is reported");
    MP_CHECK(!contains(d.enclosuresWithoutShades, 1),
             "a populated enclosure is not reported");

    // --- tremulants ---
    MP_CHECK(m.tremulants.size() == 2, "both tremulants parsed");
    const auto& trem = m.tremulants.at(1);
    MP_CHECK(std::fabs(trem.engagedHz - 6.4) < 1e-9, "engaged rate parsed");
    MP_CHECK(std::fabs(trem.disengagedHz - 5.0) < 1e-9, "disengaged rate parsed");
    MP_CHECK(std::fabs(trem.startPercent - 60.0) < 1e-9, "engaging ramp parsed");
    MP_CHECK(std::fabs(trem.stopPercent - 40.0) < 1e-9, "disengaging ramp parsed");
    // Depth is NOT a field on the Tremulant row — Hauptwerk puts modulation
    // depth on TremulantWaveformPipe, per pipe. We take the strongest as the
    // tremulant's nominal depth so "drawn but does nothing" stays checkable.
    MP_CHECK(std::fabs(trem.depthPercent - 3.5) < 1e-9,
             "depth comes from TremulantWaveformPipe, not the Tremulant row");
    MP_CHECK(trem.controllingSwitchId == 1, "tremulant bound to its drawstop");
    MP_CHECK(trem.hasWaveform && trem.waveformId == 500,
             "TremulantWaveform links the waveform to its tremulant");
    MP_CHECK(contains(d.tremulantsDepthZero, 2),
             "a drawn tremulant with no depth is reported");
    MP_CHECK(!contains(d.tremulantsDepthZero, 1),
             "a working tremulant is not reported");

    // --- continuous controls ---
    MP_CHECK(m.continuousControls.size() == 5, "all continuous controls parsed");
    const auto& shoe = m.continuousControls.at(1);
    MP_CHECK(shoe.name == "Swell Shoe", "control name parsed");
    MP_CHECK(shoe.typeCode == 1, "the default assignment code is parsed");
    // An ABSENT assignment code is normal (the control simply is not mapped to
    // a console input by default); a PRESENT one we have no behaviour for is
    // what deserves reporting. Flagging the absent case fired on all 1420
    // controls of a real organ.
    MP_CHECK(!contains(d.unmappedContinuousControls, 3),
             "a control with no assignment code is not a fault");
    MP_CHECK(contains(d.unmappedContinuousControls, 1),
             "a control carrying a code we cannot map is reported");

    // Hauptwerk continuous controls carry no declared range: they are plain
    // 0..127. The fixture no longer contains a reversed-range row because a
    // real file cannot express one.
    MP_CHECK(shoe.minValue == 0 && shoe.maxValue == 127,
             "a continuous control is 0..127 by definition");

    // --- linkages ---
    MP_CHECK(m.controlLinkages.size() == 3, "all linkages parsed");
    MP_CHECK(std::fabs(m.controlLinkages[0].scale - 0.5) < 1e-9,
             "linkage scaling parsed");
    // Mutually-referencing linkages are ordinary in Hauptwerk and are NOT a
    // fault; only a control wired directly to itself is.
    MP_CHECK(d.controlLinkageCycles.empty(),
             "a mutual linkage pair is not reported as a loop");

    // --- noise ranks ---
    MP_CHECK(m.ranks.at(91).isNoise, "a rank named by Noise is flagged");
    MP_CHECK(m.ranks.at(91).noiseTriggerSwitchId == 3,
             "noise trigger switch linked");
    MP_CHECK(contains(d.noiseRanksWithoutSample, 92),
             "a noise rank with no attacks is reported");
    MP_CHECK(!contains(d.noiseRanksWithoutSample, 91),
             "a noise rank that has audio is not reported");

    // Nothing above may leave a dangling reference behind.
    MP_CHECK(d.danglingIds.empty(),
             "the expression fixture resolves every reference");
  }
};

// Players who do not want mechanical sound switch the noises off at load; the
// ranks must actually leave the model rather than merely be muted later.
class NoiseOptOutTest final : public mp::test::Test {
public:
  NoiseOptOutTest()
    : Test("functional.expression.noise-opt-out", Category::Functional) {}
  void run() override {
    const std::string xml = readFixture("m24.expression.Organ_Hauptwerk_xml");
    MP_CHECK(!xml.empty(), "fixture readable");

    mp::OdfLoader loader;
    mp::OdfLoader::Options opts;
    opts.includeKeyNoises = false;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(loader.loadFromXmlString(xml, "m24.expression.Organ_Hauptwerk_xml",
                                      opts, m, d),
             "fixture loads with noises disabled");
    MP_CHECK(m.ranks.count(91) == 0 && m.ranks.count(92) == 0,
             "noise ranks are dropped when the player opts out");
    MP_CHECK(d.noiseRanksWithoutSample.empty(),
             "dropped noise ranks are not then reported as silent");
  }
};

class TemperamentRatioTest final : public mp::test::Test {
public:
  TemperamentRatioTest()
    : Test("functional.temperament.ratio", Category::Functional) {}
  void run() override {
    mp::Temperament t{"Equal", std::vector<double>(12, 0.0)};

    // A4 (MIDI 69) on an 8' rank is the anchor: it sounds at the base pitch.
    MP_CHECK(std::fabs(mp::pipeTargetHz(69, 8, 440.0, 0.0, t, 0) - 440.0) < 1e-9,
             "A4 on an 8' rank sounds at the organ base pitch");
    const double r0 = mp::temperedPlaybackRatio(69, 8, 440.0, 0.0, t, 0);
    MP_CHECK(std::fabs(r0 - 1.0) < 1e-9, "8' A4 at base pitch = ratio 1");
    const double rOct = mp::temperedPlaybackRatio(81, 8, 440.0, 0.0, t, 0);
    MP_CHECK(std::fabs(rOct - 2.0) < 1e-9, "one octave up = ratio 2");

    // 64' harmonic numbering: 16 is a 4' rank (an octave up), 4 is 16' (down).
    const double r4ft = mp::temperedPlaybackRatio(69, 16, 440.0, 0.0, t, 0);
    MP_CHECK(std::fabs(r4ft - 2.0) < 1e-9, "harmonic 16 = 4' rank, one octave up");
    const double r16ft = mp::temperedPlaybackRatio(69, 4, 440.0, 0.0, t, 0);
    MP_CHECK(std::fabs(r16ft - 0.5) < 1e-9, "harmonic 4 = 16' rank, one octave down");

    // Transposer and per-pipe tuning deviation.
    MP_CHECK(std::fabs(mp::pipeTargetHz(69, 8, 440.0, 0.0, t, 12) - 880.0) < 1e-9,
             "transposing up an octave doubles the sounding pitch");
    const double sharp = mp::pipeTargetHz(69, 8, 440.0, 100.0, t, 0);
    MP_CHECK(std::fabs(mp::centsBetween(sharp, 440.0) - 100.0) < 1e-6,
             "a 100-cent deviation raises the pipe by a semitone");

    const double rOrig = mp::originalPitchRatio(440.0, 465.0);
    MP_CHECK(std::fabs(rOrig - 465.0 / 440.0) < 1e-9,
             "original-pitch ratio = target/sample");
    MP_CHECK(std::fabs(mp::playbackRatio(880.0, 440.0) - 2.0) < 1e-9,
             "playback ratio = target/recorded");
    MP_CHECK(std::fabs(mp::playbackRatio(880.0, 0.0) - 1.0) < 1e-9,
             "a zero recorded pitch falls back to unity, never divides by zero");

    // Range guard (validator query pipe-pitch-out-of-range).
    MP_CHECK(mp::pipeHzInRange(440.0), "A4 is in range");
    MP_CHECK(!mp::pipeHzInRange(2.0), "sub-64' infrasound is out of range");
    // 30 kHz is IN range on purpose: the top of a 1 3/5' Tierce reaches it.
    MP_CHECK(mp::pipeHzInRange(30000.0),
             "a real mutation rank can exceed hearing and is still valid");
    MP_CHECK(!mp::pipeHzInRange(1.0e6),
             "a mis-declared footage lands in the megahertz and is caught");

    // Degenerate temperament vectors must never index out of bounds.
    mp::Temperament empty{"Empty", {}};
    mp::Temperament wrongSize{"Wrong", std::vector<double>(7, 5.0)};
    MP_CHECK(std::fabs(mp::pipeTargetHz(69, 8, 440.0, 0.0, empty, 0) - 440.0) < 1e-9,
             "an empty temperament behaves as equal");
    MP_CHECK(std::fabs(mp::pipeTargetHz(69, 8, 440.0, 0.0, wrongSize, 0) - 440.0) < 1e-9,
             "a wrong-size temperament behaves as equal");
  }
};

#ifdef MP_TEST_HAS_SAMPLER
class DiskTierClassifyTest final : public mp::test::Test {
public:
  DiskTierClassifyTest()
    : Test("functional.sampler.disk-tier", Category::Functional) {}
  void run() override {
    MP_CHECK(mp::classifyTier(-1.0) == mp::DiskTier::Unknown,
             "probe error -> Unknown");
    MP_CHECK(mp::classifyTier(0.0) == mp::DiskTier::Unknown,
             "zero throughput -> Unknown");
    MP_CHECK(mp::classifyTier(120.0) == mp::DiskTier::Hdd,
             "120 MB/s -> Hdd");
    MP_CHECK(mp::classifyTier(500.0) == mp::DiskTier::SataSsd,
             "500 MB/s -> SataSsd");
    MP_CHECK(mp::classifyTier(3500.0) == mp::DiskTier::Nvme,
             "3500 MB/s -> Nvme");
    MP_CHECK(mp::headScaleForTier(mp::DiskTier::Nvme) == 100,
             "NVMe keeps base heads");
    MP_CHECK(mp::headScaleForTier(mp::DiskTier::SataSsd) == 200,
             "SATA SSD doubles heads");
    MP_CHECK(mp::headScaleForTier(mp::DiskTier::Hdd) == 400,
             "HDD quadruples heads");
    MP_CHECK(mp::headScaleForTier(mp::DiskTier::Unknown) == 200,
             "Unknown tier is conservative (SATA-class heads)");
  }
};

class SampleHandleTest final : public mp::test::Test {
public:
  SampleHandleTest()
    : Test("functional.sampler.sample-handle", Category::Functional) {}
  void run() override {
    std::string err;
    mp::SampleHandle empty;
    MP_CHECK(!empty.open("", err), "empty path must fail");
    MP_CHECK(!err.empty(), "failure must explain itself");

    err.clear();
    mp::SampleHandle enc;
    MP_CHECK(!enc.open("pipe.hbw", err),
             "encrypted sample must be refused (ADR-003)");
    MP_CHECK(err.find("encrypted") != std::string::npos,
             "error must mention encryption");

    err.clear();
    mp::SampleHandle wav;
    MP_CHECK(wav.open("pipe.wav", err), "plain WAV path accepted (M2 wires real mmap)");
    MP_CHECK(err.empty(), "no error on plain WAV");
    MP_CHECK(wav.isOpen(), "handle open after successful path validation");

    err.clear();
    mp::SampleHandle wv;
    MP_CHECK(wv.open("pipe.wv", err),
             "WavPack path accepted (ADR-011: HW-compatibility must)");
    MP_CHECK(err.empty(), "no error on WavPack");
    MP_CHECK(wv.isOpen(), "handle open for WavPack");

    err.clear();
    mp::SampleHandle other;
    MP_CHECK(!other.open("pipe.flac", err),
             "non-WAV/WavPack refused in v1 (ADR-011)");
    MP_CHECK(!err.empty(), "refusal must explain itself");
  }
};
#endif

#ifdef MP_TEST_HAS_AUDIO
// The backing store reads real audio files and hands the voice engine resident
// buffers. This test writes actual WAVs to a temp directory and loads them, so
// it covers the decode path, the Windows-backslash path translation that every
// Hauptwerk ODF depends on, and the preload-head cap.
class SampleLibraryTest final : public mp::test::Test {
public:
  SampleLibraryTest()
    : Test("functional.samples.library", Category::Functional) {}

  static void writeWav(const juce::File& file, int frames, int channels,
                       double sr, int loopStart = -1, int loopEnd = -1) {
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
    MP_CHECK(os != nullptr, "temp wav is writable");
    // JUCE writes an smpl chunk from these flat metadata keys; see
    // juce_WavAudioFormat.cpp SMPLChunk.
    juce::StringPairArray meta;
    if (loopStart >= 0 && loopEnd > loopStart) {
      meta.set("NumSampleLoops", "1");
      meta.set("Loop0Start", juce::String(loopStart));
      meta.set("Loop0End", juce::String(loopEnd));
      meta.set("Loop0Type", "0");
    }
    std::unique_ptr<juce::AudioFormatWriter> w(
        fmt.createWriterFor(os.release(), sr, static_cast<unsigned>(channels),
                            16, meta, 0));
    MP_CHECK(w != nullptr, "wav writer created");
    juce::AudioBuffer<float> buf(channels, frames);
    for (int c = 0; c < channels; ++c)
      for (int i = 0; i < frames; ++i)
        buf.setSample(c, i, static_cast<float>(
            0.5 * std::sin(2.0 * 3.141592653589793 * 440.0 * i / sr)));
    w->writeFromAudioSampleBuffer(buf, 0, frames);
  }

  void run() override {
    const auto root = juce::File::getSpecialLocation(
        juce::File::tempDirectory).getChildFile("mp_sample_library_test");
    root.deleteRecursively();
    root.createDirectory();

    writeWav(root.getChildFile("sub").getChildFile("a.wav"), 2400, 1, 48000.0);
    writeWav(root.getChildFile("stereo.wav"), 1200, 2, 44100.0);

    mp::OrganModel model;
    // Deliberately Windows-style: this is how every real ODF spells a path.
    mp::SampleRef mono;
    mono.sampleId = 1;
    mono.fileName = "sub\\a.wav";
    model.samples[1] = mono;

    mp::SampleRef stereo;
    stereo.sampleId = 2;
    stereo.fileName = "stereo.wav";
    model.samples[2] = stereo;

    mp::SampleRef missing;
    missing.sampleId = 3;
    missing.fileName = "nope.wav";
    model.samples[3] = missing;

    mp::SampleRef locked;
    locked.sampleId = 4;
    locked.fileName = "secret.hbw"; // ADR-003: never decoded
    model.samples[4] = locked;

    mp::SampleLibrary lib;
    const auto report = lib.loadAll(model, root.getFullPathName().toStdString());

    MP_CHECK(report.loaded == 2, "both readable samples loaded");
    MP_CHECK(report.missing == 1, "the absent file is reported, not fatal");
    MP_CHECK(report.encrypted == 1, "an encrypted sample is skipped and counted");
    MP_CHECK(report.failed == 0, "nothing failed to decode");
    MP_CHECK(lib.residentCount() == 2, "two buffers are resident");
    MP_CHECK(lib.residentBytes() > 0, "resident bytes are accounted");

    auto provider = lib.provider();
    const mp::SampleBuffer* a = provider(1);
    MP_CHECK(a != nullptr, "a Windows-style relative path resolves");
    MP_CHECK(a->numFrames == 2400, "all frames loaded");
    MP_CHECK(a->numChannels == 1, "mono stays mono");
    MP_CHECK(std::fabs(a->sampleRate - 48000.0) < 1e-6, "sample rate read");

    const mp::SampleBuffer* st = provider(2);
    MP_CHECK(st != nullptr, "the stereo file loaded");
    MP_CHECK(st->numChannels == 2, "stereo stays stereo");
    MP_CHECK(std::fabs(st->sampleRate - 44100.0) < 1e-6,
             "a differing sample rate is preserved, not resampled at load");
    // Whichever container holds the audio, it holds one value per channel per
    // frame. Asserting against `frames` alone pinned the test to 32-bit float
    // residency, which stopped being the default when 24-bit arrived.
    const size_t held = st->frames.size() + st->pcm16.size() + st->pcm24.size();
    MP_CHECK(held == static_cast<size_t>(st->numFrames) * 2,
             "stereo frames are interleaved");

    MP_CHECK(provider(3) == nullptr, "a missing sample has no buffer");
    MP_CHECK(provider(4) == nullptr, "an encrypted sample has no buffer");
    MP_CHECK(provider(999) == nullptr, "an unknown id yields nullptr, not a crash");

    // The buffer must actually contain the tone, not zeros. Read it through
    // sample(), which returns the true value whatever the resident format is;
    // walking `frames` directly only worked while 32-bit float was the
    // default and reported silence the day that changed.
    double peak = 0.0;
    for (int64_t i = 0; i < a->numFrames; ++i)
      peak = std::max(peak, std::fabs(static_cast<double>(a->sample(i, 0))));
    MP_CHECK(peak > 0.3, "the decoded buffer holds real audio");

    // A sustain loop in the file's smpl chunk must reach the voice engine:
    // without it a held note plays once and stops, which is the difference
    // between an instrument and a demo.
    writeWav(root.getChildFile("looped.wav"), 2000, 1, 48000.0, 400, 1600);
    mp::SampleRef looped;
    looped.sampleId = 5;
    looped.fileName = "looped.wav";
    model.samples[5] = looped;

    mp::SampleLibrary loopLib;
    loopLib.loadAll(model, root.getFullPathName().toStdString());
    const mp::SampleBuffer* lb = loopLib.provider()(5);
    MP_CHECK(lb != nullptr, "the looped file loaded");
    MP_CHECK(lb->loops(), "the smpl chunk loop was read");
    // dwEnd is the last sample IN the loop; SampleBuffer wants a half-open
    // end, so 1600 in the file means 1601 here. GrandOrgue does the same.
    MP_CHECK(lb->loopStart == 400 && lb->loopEnd == 1601,
             "an inclusive dwEnd becomes a half-open end");

    // A file with no smpl chunk must report no loop rather than inventing one.
    MP_CHECK(!loopLib.provider()(1)->loops(),
             "a file without a loop chunk is not looped");

    // A head shorter than the loop does NOT drop the loop: it is extended to
    // cover it. Dropping it would mean the note stops when the sample runs
    // out, which is not "less preloaded" — it is a broken instrument. Real
    // organ samples run 6-7 s with the loop ending at 3-6 s, so the loop is
    // what dictates how much has to be resident.
    mp::SampleLibrary shortHead;
    shortHead.loadAll(model, root.getFullPathName().toStdString(), 800);
    const mp::SampleBuffer* extended = shortHead.provider()(5);
    MP_CHECK(extended != nullptr, "the short-head load succeeded");
    MP_CHECK(extended->loops(),
             "a head shorter than the loop is extended, not truncated");
    MP_CHECK(extended->numFrames >= extended->loopEnd,
             "the loop end is resident so the note can sustain");
    MP_CHECK(extended->numFrames > 800,
             "the head grew past the requested minimum to reach the loop");

    // And the engine must actually sustain on it.
    {
      mp::VoiceEngine le;
      le.prepare(48000.0, 4, 1);
      le.setSampleProvider(loopLib.provider());
      mp::Pipe lp;
      lp.pipeId = 5;
      mp::PipeLayer ll;
      mp::AttackSample la;
      la.id = 5;
      la.sample.sampleId = 5;
      ll.attacks.push_back(la);
      lp.layers.push_back(ll);
      mp::VoiceStart lvs;
      lvs.pipe = &lp;
      lvs.layer = &lp.layers[0];
      MP_CHECK(le.startVoice(lvs, 1) >= 0, "looped voice started");
      std::vector<float> lo(4096, 0.0f);
      float* lptr[1] = {lo.data()};
      // Well past the 2000-frame file: only a working loop keeps this alive.
      for (int i = 0; i < 6; ++i) le.render(lptr, 1, 4096);
      MP_CHECK(le.activeVoiceCount() == 1,
               "a looped sample sustains past its own length");
    }

    // Several loops in one file is the normal case for an organ sample, and
    // which one to use is a listening preference. Cross-checked against two
    // working players: GrandOrgue reads every loop and selects among them,
    // rusty-pipes takes only the first. Both treat the smpl chunk's end as
    // EXCLUSIVE (wrap when position >= end), which is what the engine does.
    {
      const auto multi = root.getChildFile("multi.wav");
      multi.deleteFile();
      juce::WavAudioFormat fmt;
      std::unique_ptr<juce::FileOutputStream> os(multi.createOutputStream());
      MP_CHECK(os != nullptr, "multi-loop wav is writable");
      juce::StringPairArray meta;
      meta.set("NumSampleLoops", "3");
      meta.set("Loop0Start", "100"); meta.set("Loop0End", "200");  // len 100
      meta.set("Loop1Start", "300"); meta.set("Loop1End", "900");  // len 600
      meta.set("Loop2Start", "950"); meta.set("Loop2End", "1000"); // len  50
      std::unique_ptr<juce::AudioFormatWriter> w(
          fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
      MP_CHECK(w != nullptr, "multi-loop writer created");
      juce::AudioBuffer<float> tone(1, 2000);
      for (int i = 0; i < 2000; ++i)
        tone.setSample(0, i, static_cast<float>(
            0.5 * std::sin(2.0 * 3.141592653589793 * 440.0 * i / 48000.0)));
      w->writeFromAudioSampleBuffer(tone, 0, 2000);
      w.reset();

      mp::SampleRef mref;
      mref.sampleId = 6;
      mref.fileName = "multi.wav";
      mp::OrganModel mm;
      mm.samples[6] = mref;
      const auto rootPath = root.getFullPathName().toStdString();

      mp::SampleLibrary lng, con, fst;
      lng.loadAll(mm, rootPath, 0, mp::LoopSelection::Longest);
      con.loadAll(mm, rootPath, 0, mp::LoopSelection::Conservative);
      fst.loadAll(mm, rootPath, 0, mp::LoopSelection::First);

      const mp::SampleBuffer* bl = lng.provider()(6);
      const mp::SampleBuffer* bc = con.provider()(6);
      const mp::SampleBuffer* bf = fst.provider()(6);
      MP_CHECK(bl && bc && bf, "all three selections loaded the file");
      MP_CHECK(bl->loopStart == 300 && bl->loopEnd == 901,
               "Longest picks the widest loop");
      MP_CHECK(bc->loopStart == 950 && bc->loopEnd == 1001,
               "Conservative picks the narrowest loop");
      MP_CHECK(bf->loopStart == 100 && bf->loopEnd == 201,
               "First picks the file's own first loop");

      // The preload head is a MINIMUM, not a cap. A head of 500 frames would
      // cut off every loop in this file, and a sample whose loop is missing
      // does not sustain — the note simply dies. So the read is extended to
      // cover the loop that was chosen, and the head only decides how much
      // MORE than that is resident.
      mp::SampleLibrary capped;
      capped.loadAll(mm, rootPath, 500, mp::LoopSelection::Longest);
      const mp::SampleBuffer* bcap = capped.provider()(6);
      MP_CHECK(bcap != nullptr, "the capped load succeeded");
      MP_CHECK(bcap->loops(),
               "a short head is extended to cover the loop, not truncated");
      MP_CHECK(bcap->loopStart == 300 && bcap->loopEnd == 901,
               "the chosen loop is still the longest one");
      MP_CHECK(bcap->numFrames >= bcap->loopEnd,
               "enough audio is resident to reach the loop end");
      // The read also takes a margin past the loop so the crossfade has
      // material to fade out of. On a file this short that margin covers the
      // rest of it, which is why this checks the bound rather than a size.
      MP_CHECK(bcap->numFrames <= 2000,
               "never reads past the end of the file");
    }

    // Preload head: capping frames is how streaming mode stays bounded.
    mp::SampleLibrary head;
    head.loadAll(model, root.getFullPathName().toStdString(), 500);
    const mp::SampleBuffer* capped = head.provider()(1);
    MP_CHECK(capped != nullptr && capped->numFrames == 500,
             "maxFramesPerSample caps the resident head");

    // Reloading publishes a new generation; the old provider keeps working,
    // which is what lets a sounding voice survive a reload.
    const mp::SampleBuffer* before = provider(1);
    lib.loadAll(model, root.getFullPathName().toStdString());
    MP_CHECK(before != nullptr, "the retired generation is still readable");
    MP_CHECK(provider(1) != nullptr, "the provider follows the new generation");
    MP_CHECK(provider(1) != before, "a reload really did swap generations");
    lib.retireOldGenerations();
    MP_CHECK(provider(1) != nullptr, "the live generation survives retirement");

    // Feed it straight into the voice engine: the two halves must fit.
    mp::VoiceEngine eng;
    eng.prepare(48000.0, 8, 1);
    eng.setSampleProvider(lib.provider());
    mp::Pipe pipe;
    pipe.pipeId = 1;
    mp::PipeLayer layer;
    mp::AttackSample atk;
    atk.id = 1;
    atk.sample.sampleId = 1;
    layer.attacks.push_back(atk);
    pipe.layers.push_back(layer);
    mp::VoiceStart vs;
    vs.pipe = &pipe;
    vs.layer = &pipe.layers[0];
    MP_CHECK(eng.startVoice(vs, 1) >= 0,
             "the voice engine plays a buffer the library loaded");
    std::vector<float> out(256, 0.0f);
    float* ptrs[1] = {out.data()};
    eng.render(ptrs, 1, 256);
    double rms = 0.0;
    for (float x : out) rms += (double)x * x;
    MP_CHECK(std::sqrt(rms / out.size()) > 0.05,
             "a library-loaded sample actually sounds");

    root.deleteRecursively();
  }
};

// The recorder writes from a background thread, so "it compiled" says very
// little: what matters is that the file is finalised, has the right length,
// and holds the samples that were handed over rather than silence.
class AudioRecorderTest final : public mp::test::Test {
 public:
  AudioRecorderTest()
      : Test("functional.audio.recorder", Category::Functional) {}
  void run() override {
    const auto dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("mp_rec_test");
    dir.createDirectory();
    const auto wav = dir.getChildFile("take.wav");

    constexpr double kRate = 48000.0;
    constexpr int kBlock = 256;
    constexpr int kBlocks = 40; // 10240 frames
    {
      mp::AudioRecorder rec;
      MP_CHECK(rec.start(wav, kRate, 2), "the recorder opens its file");
      MP_CHECK(rec.isRecording(), "and reports that it is running");

      // A full-scale tone, so silence in the file cannot be mistaken for a
      // quiet-but-present signal.
      juce::AudioBuffer<float> buf(2, kBlock);
      double phase = 0.0;
      for (int b = 0; b < kBlocks; ++b) {
        for (int i = 0; i < kBlock; ++i) {
          const float s = 0.5f * std::sin(phase);
          phase += 2.0 * juce::MathConstants<double>::pi * 440.0 / kRate;
          buf.setSample(0, i, s);
          buf.setSample(1, i, s);
        }
        rec.write(buf);
      }
      rec.stop();
      MP_CHECK(!rec.isRecording(), "and stops when told to");
    }
    // The destructor has run: whatever the background thread still held must
    // be on disk by now, or a player who quits loses the take.

    MP_CHECK(wav.existsAsFile(), "the file survives the recorder");

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(
        fm.createReaderFor(wav));
    MP_CHECK(reader != nullptr, "and reads back as a valid WAV");
    if (reader == nullptr) {
      dir.deleteRecursively();
      return;
    }

    MP_CHECK(reader->numChannels == 2, "with both channels");
    MP_CHECK(static_cast<int>(reader->sampleRate) == 48000,
             "at the rate it was given");
    // Nothing dropped: the FIFO has to have carried every block.
    MP_CHECK(reader->lengthInSamples == kBlocks * kBlock,
             "and every frame that was written");

    juce::AudioBuffer<float> back(2, static_cast<int>(reader->lengthInSamples));
    reader->read(&back, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
    const float mag = back.getMagnitude(0, 0, back.getNumSamples());
    MP_CHECK(mag > 0.4f && mag <= 1.0f,
             "holding the signal, not silence");

    dir.deleteRecursively();
  }
};

// Issue #13: the master fader lived only for the running session. Nothing
// wrote it down, and switching organs (or restarting the app, which is the
// same load path with a colder cache) always found it back at unity.
//
// This exercises the processor's real per-organ files under the player's
// own AppData, the way the app actually saves and loads — graphics-only, per
// the note on loadOrgan(), is exactly the path meant for a headless check
// like this one. A small RAII guard puts every touched file back exactly as
// it was found (including the GLOBAL defaults file, which a real profile on
// this machine may already have), so the test leaves no trace either way.
class MasterGainPersistenceTest final : public mp::test::Test {
public:
  MasterGainPersistenceTest()
    : Test("functional.settings.master-gain", Category::Functional) {}

  struct RestoreFile {
    juce::File file;
    bool existed = false;
    juce::String content;
    explicit RestoreFile(juce::File f) : file(std::move(f)) {
      existed = file.existsAsFile();
      if (existed) content = file.loadFileAsString();
    }
    ~RestoreFile() {
      if (existed) file.replaceWithText(content);
      else file.deleteFile();
    }
  };

  void run() override {
    const juce::File odfA(juce::String(MP_TEST_FIXTURES_DIR) +
                          "/minimal.Organ_Hauptwerk_xml");
    const juce::File odfB(juce::String(MP_TEST_FIXTURES_DIR) +
                          "/minimal.CustomOrgan_Hauptwerk_xml");
    MP_CHECK(odfA.existsAsFile() && odfB.existsAsFile(),
             "both fixtures are present on disk");

    mp::MasterpieceProcessor proc;

    // Guards constructed before anything runs, so whatever state each file
    // is in right now — including "does not exist" — is what comes back.
    RestoreFile keepGlobal(proc.globalSettingsFile());
    RestoreFile keepA(proc.settingsFileFor(odfA));
    RestoreFile keepB(proc.settingsFileFor(odfB));

    auto gain = [&] {
      const auto* g = proc.apvts().getRawParameterValue("masterGain");
      return g != nullptr ? g->load() : -1.0f;
    };
    auto setGain = [&](float v) {
      if (auto* p = proc.apvts().getParameter("masterGain"))
        p->setValueNotifyingHost(p->convertTo0to1(v));
    };

    // A synthetic organ that has never had anything saved for it starts at
    // unity (0 dB) — whatever the previous processor state happened to be.
    auto r1 = proc.loadOrgan(odfA, 0, /*graphicsOnly=*/true);
    MP_CHECK(r1.ok, "the first fixture loads graphics-only");
    MP_CHECK(std::abs(gain() - 1.0f) < 1e-4f,
             "an organ with nothing saved starts at unity");

    // Move the fader the way the volume slider does, and flush it the way
    // the editor's timer does.
    setGain(0.5f);
    proc.markMasterGainDirty();
    MP_CHECK(proc.saveMasterGainIfDirty(), "the dirty flag causes a write");
    MP_CHECK(!proc.saveMasterGainIfDirty(),
             "and only once — the flag clears itself");
    MP_CHECK(proc.settingsFileFor(odfA).existsAsFile(),
             "a per-organ settings file now holds the level");

    // A different organ, never touched, must not inherit this one's live
    // value just because it is still sitting in the parameter.
    auto r2 = proc.loadOrgan(odfB, 0, true);
    MP_CHECK(r2.ok, "the second fixture loads graphics-only");
    MP_CHECK(std::abs(gain() - 1.0f) < 1e-4f,
             "switching to an untouched organ resets to unity rather than "
             "carrying the first organ's level");

    // Coming back to the first organ restores exactly what was saved for it,
    // unprompted — no Settings dialog, no explicit save.
    auto r3 = proc.loadOrgan(odfA, 0, true);
    MP_CHECK(r3.ok, "the first fixture reloads");
    MP_CHECK(std::abs(gain() - 0.5f) < 1e-4f,
             "and its own saved level comes back on its own");

    // The write itself has to leave everything else in the file alone: a
    // Settings-dialog change the player only "kept" for the session must
    // never ride along on the next tick of the volume slider.
    const auto fileA = proc.settingsFileFor(odfA);
    fileA.replaceWithText(
        juce::String("# Masterpiece per-organ settings\n") +
        "mono 1\ngain 1.0000\nwind 1\n");
    setGain(0.75f);
    proc.markMasterGainDirty();
    proc.saveMasterGainIfDirty();
    const auto lines = juce::StringArray::fromLines(fileA.loadFileAsString());
    MP_CHECK(lines.contains("mono 1") && lines.contains("wind 1"),
             "lines the gain save did not own survive it untouched");
    MP_CHECK(lines.contains("gain 0.7500"),
             "and the gain line carries the new value");
    // Counted without blank entries: fromLines reports one for the file's
    // closing newline, and that is formatting, not content.
    int contentLines = 0;
    for (const auto& l : lines)
      if (l.trim().isNotEmpty()) ++contentLines;
    MP_CHECK(contentLines == 4,
             "the gain-only save neither duplicated nor dropped a line");

    // And saving again must not grow the file. Before the writer dropped the
    // trailing blank entry, every save added a blank line.
    const auto sizeAfterOne = fileA.getSize();
    setGain(0.6f);
    proc.markMasterGainDirty();
    proc.saveMasterGainIfDirty();
    setGain(0.75f);
    proc.markMasterGainDirty();
    proc.saveMasterGainIfDirty();
    MP_CHECK(fileA.getSize() == sizeAfterOne,
             "repeated saves leave the file the same size");
  }
};

class RecentOrgansTest final : public mp::test::Test {
public:
  RecentOrgansTest()
      : Test("functional.organs.recent-organs", Category::Functional) {}
  void run() override {
    const auto dir = mp::MasterpieceProcessor::dataDirectory();
    MP_CHECK(dir.getFullPathName().isNotEmpty(), "data directory must not be empty");
    MP_CHECK(dir.getFileName() == "Masterpiece", "data directory name is Masterpiece");

    mp::MasterpieceProcessor proc;
    const juce::File f1("/path/to/Organ1.Organ_Hauptwerk_xml");
    const juce::File f2("/path/to/Organ2.Organ_Hauptwerk_xml");

    proc.addRecentOrgan(f1);
    proc.addRecentOrgan(f2);
    MP_CHECK(proc.recentOrgans().size() >= 2, "recent organs added");
    MP_CHECK(proc.recentOrgans()[0] == f2, "most recent organ is at the front");
    MP_CHECK(proc.recentOrgans()[1] == f1, "earlier organ follows");

    proc.addRecentOrgan(f1);
    MP_CHECK(proc.recentOrgans()[0] == f1, "re-added organ moves to front");
    MP_CHECK(proc.recentOrgans()[1] == f2, "other organ pushed back");

    proc.removeRecentOrgan(f2);
    MP_CHECK(std::find(proc.recentOrgans().begin(), proc.recentOrgans().end(), f2) ==
             proc.recentOrgans().end(), "removed organ is gone");
  }
};

class ArchiveFilteringTest final : public mp::test::Test {
public:
  ArchiveFilteringTest()
      : Test("functional.organs.archive-filtering", Category::Functional) {}
  void run() override {
    const auto d = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("mp_archive_test_" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()));
    d.createDirectory();

    const auto singleRar = d.getChildFile("SinglePackage.CompPkg_Hauptwerk_rar");
    singleRar.create();

    const auto p1 = d.getChildFile("MultiSet.part1.rar");
    const auto p2 = d.getChildFile("MultiSet.part2.rar");
    const auto p3 = d.getChildFile("MultiSet.part3.rar");
    p1.create();
    p2.create();
    p3.create();

    const auto b01 = d.getChildFile("SetB.part01.rar");
    const auto b02 = d.getChildFile("SetB.part02.rar");
    b01.create();
    b02.create();

    const auto oldRar = d.getChildFile("OldStyle.rar");
    const auto oldR00 = d.getChildFile("OldStyle.r00");
    const auto oldR01 = d.getChildFile("OldStyle.r01");
    oldRar.create();
    oldR00.create();
    oldR01.create();

    juce::Array<juce::File> allFiles = {singleRar, p1, p2, p3, b01, b02, oldRar, oldR00, oldR01};
    const auto filtered = mp::ui::filterArchivesForExtraction(allFiles);

    MP_CHECK(filtered.size() == 4, "filtered down to primary archives of each set");
    MP_CHECK(filtered.contains(singleRar), "standalone archive kept");
    MP_CHECK(filtered.contains(p1), "part1 kept");
    MP_CHECK(!filtered.contains(p2), "part2 excluded");
    MP_CHECK(!filtered.contains(p3), "part3 excluded");
    MP_CHECK(filtered.contains(b01), "part01 kept");
    MP_CHECK(!filtered.contains(b02), "part02 excluded");
    MP_CHECK(filtered.contains(oldRar), "old style .rar kept");
    MP_CHECK(!filtered.contains(oldR00), ".r00 excluded");
    MP_CHECK(!filtered.contains(oldR01), ".r01 excluded");

    juce::Array<juce::File> onlyPart2 = {p2};
    const auto resolved = mp::ui::filterArchivesForExtraction(onlyPart2);
    MP_CHECK(resolved.size() == 1 && resolved.contains(p1),
             "selecting secondary part resolves to part1 if present");

    d.deleteRecursively();
  }
};

class OrganNameParsingTest final : public mp::test::Test {
public:
  OrganNameParsingTest()
      : Test("functional.organs.name-parsing", Category::Functional) {}
  void run() override {
    const auto fixturePath = juce::File::getCurrentWorkingDirectory()
                                 .getChildFile("tests")
                                 .getChildFile("minimal.Organ_Hauptwerk_xml");
    if (fixturePath.existsAsFile()) {
      const auto name = mp::ui::readOrganNameFromOdf(fixturePath);
      MP_CHECK(name == "Masterpiece Test Church", "organ name parsed from Identification_Name");
    }

    const auto d = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("mp_odf_name_test_" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()));
    d.createDirectory();

    const auto tempFile = d.getChildFile("Custom.Organ_Hauptwerk_xml");
    tempFile.replaceWithText(
        "<?xml version=\"1.0\"?>\n"
        "<Hauptwerk FileFormat=\"Organ\">\n"
        "  <ObjectList ObjectType=\"_General\">\n"
        "    <_General>\n"
        "      <Identification_OrganName>St. Sulpice Paris</Identification_OrganName>\n"
        "    </_General>\n"
        "  </ObjectList>\n"
        "</Hauptwerk>");
    const auto parsedName = mp::ui::readOrganNameFromOdf(tempFile);
    MP_CHECK(parsedName == "St. Sulpice Paris", "organ name parsed from Identification_OrganName");

    const auto fallbackFile = d.getChildFile("FallbackName.Organ_Hauptwerk_xml");
    fallbackFile.replaceWithText("<Hauptwerk></Hauptwerk>");
    const auto fallbackName = mp::ui::readOrganNameFromOdf(fallbackFile);
    MP_CHECK(fallbackName == "FallbackName", "falls back to file basename when no name tag exists");

    d.deleteRecursively();
  }
};

class OrganEtaTest final : public mp::test::Test {
public:
  OrganEtaTest()
      : Test("functional.organs.eta-calculation", Category::Functional) {}
  void run() override {
    MP_CHECK(mp::ui::humaniseEta(10.0) == "less than a minute", "under 45s is less than a minute");
    MP_CHECK(mp::ui::humaniseEta(44.0) == "less than a minute", "44s is less than a minute");
    MP_CHECK(mp::ui::humaniseEta(45.0) == "about a minute", "45s rounds to about a minute");
    MP_CHECK(mp::ui::humaniseEta(80.0) == "about a minute", "80s rounds to about a minute");
    MP_CHECK(mp::ui::humaniseEta(90.0) == "about 2 minutes", "90s rounds to about 2 minutes");
    MP_CHECK(mp::ui::humaniseEta(150.0) == "about 3 minutes", "150s rounds to about 3 minutes");
    MP_CHECK(mp::ui::humaniseEta(600.0) == "about 10 minutes", "600s is about 10 minutes");
  }
};

class UnrarDiscoveryTest final : public mp::test::Test {
public:
  UnrarDiscoveryTest()
      : Test("functional.organs.unrar-discovery", Category::Functional) {}
  void run() override {
    const auto unrar = mp::ui::findUnrarBinary();
    if (unrar.existsAsFile()) {
      MP_CHECK(unrar.getFileNameWithoutExtension().toLowerCase() == "unrar",
               "discovered binary is unrar");
    }
  }
};

class OrganDiskSpaceFormatTest final : public mp::test::Test {
public:
  OrganDiskSpaceFormatTest()
      : Test("functional.organs.disk-space-format", Category::Functional) {}
  void run() override {
    MP_CHECK(mp::ui::formatByteSize(0) == "0 B", "0 bytes formatted");
    MP_CHECK(mp::ui::formatByteSize(512) == "512 B", "512 B formatted");
    MP_CHECK(mp::ui::formatByteSize(1024) == "1.0 KB", "1 KB formatted");
    MP_CHECK(mp::ui::formatByteSize(1572864) == "1.5 MB", "1.5 MB formatted");
    MP_CHECK(mp::ui::formatByteSize(static_cast<juce::int64>(2.5 * 1024 * 1024 * 1024)) == "2.50 GB", "2.5 GB formatted");
  }
};

class OrganDetailsAndPackagesTest final : public mp::test::Test {
public:
  OrganDetailsAndPackagesTest()
      : Test("functional.organs.details-and-packages", Category::Functional) {}
  void run() override {
    const auto fixturePath = juce::File::getCurrentWorkingDirectory()
                                 .getChildFile("tests")
                                 .getChildFile("minimal.Organ_Hauptwerk_xml");
    if (fixturePath.existsAsFile()) {
      mp::MasterpieceProcessor proc;
      const auto details = mp::ui::getOrganDetails(fixturePath, proc);
      MP_CHECK(details.name == "Masterpiece Test Church", "details organ name matches");
      MP_CHECK(details.uniqueOrganId == "90001", "details unique organ ID matches");
      MP_CHECK(details.odfSizeBytes > 0, "ODF size is positive");
      MP_CHECK(details.diskSpaceBytes >= details.odfSizeBytes, "total disk space includes ODF");
      MP_CHECK(details.packages.size() == 1, "found 1 required package");
      if (details.packages.size() == 1) {
        MP_CHECK(details.packages[0].packageId == 1, "package ID is 1");
        MP_CHECK(details.packages[0].name == "MinimalTestPackage", "package name is MinimalTestPackage");
        MP_CHECK(details.packages[0].directory.getFileName() == "000001", "directory ends in 000001");
      }
    }
  }
};

class OrganHidingTest final : public mp::test::Test {
public:
  OrganHidingTest()
      : Test("functional.organs.hidden-organs", Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor proc;
    const juce::File testOdf("/tmp/dummy_test_organ.Organ_Hauptwerk_xml");

    MP_CHECK(!proc.isOrganHidden(testOdf), "organ not initially hidden");
    proc.hideOrgan(testOdf);
    MP_CHECK(proc.isOrganHidden(testOdf), "organ is hidden after hideOrgan");
    proc.unhideOrgan(testOdf);
    MP_CHECK(!proc.isOrganHidden(testOdf), "organ unhidden after unhideOrgan");
  }
};

class OrganOfflineAudioSettingsTest final : public mp::test::Test {
public:
  OrganOfflineAudioSettingsTest()
      : Test("functional.organs.offline-audio-settings", Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor proc;
    const auto d = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("mp_audio_cfg_test_" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()));
    d.createDirectory();

    const auto odf = d.getChildFile("OfflineOrgan.Organ_Hauptwerk_xml");
    odf.create();

    auto cfg = mp::ui::loadOrganAudioConfig(proc, odf);
    MP_CHECK(cfg.storage == mp::SampleStorage::Int24, "default storage is 24-bit");
    MP_CHECK(!cfg.mono, "default mono is false");

    cfg.storage = mp::SampleStorage::Int16;
    cfg.mono = true;
    cfg.streamReleases = true;
    cfg.sampleRate = 48000.0;
    cfg.engineSwitch.simpleWavOnly = true;

    MP_CHECK(mp::ui::saveOrganAudioConfig(proc, odf, cfg), "saveOrganAudioConfig succeeded");

    const auto loadedCfg = mp::ui::loadOrganAudioConfig(proc, odf);
    MP_CHECK(loadedCfg.storage == mp::SampleStorage::Int16, "storage restored as 16-bit");

    MP_CHECK(loadedCfg.mono, "mono restored as true");
    MP_CHECK(loadedCfg.streamReleases, "stream releases restored as true");
    MP_CHECK(loadedCfg.sampleRate == 48000.0, "sample rate restored as 48000");
    MP_CHECK(loadedCfg.engineSwitch.simpleWavOnly, "simpleWavOnly switch restored");

    const auto organKey = mp::MasterpieceProcessor::organKeyFor(odf);
    const auto organFile = mp::MasterpieceProcessor::dataDirectory().getChildFile("organs").getChildFile(juce::String(organKey) + ".mporgan");
    if (organFile.existsAsFile()) organFile.deleteFile();
    d.deleteRecursively();
  }
};

class OrganRamEstimationTest final : public mp::test::Test {
public:
  OrganRamEstimationTest()
      : Test("functional.organs.ram-estimation", Category::Functional) {}
  void run() override {
    mp::ui::OrganAudioStat stat;
    stat.totalAudioFrames = 1000000;
    stat.attackFrames = 600000;
    stat.releaseFrames = 400000;
    stat.attackLoopFrames = 200000;
    stat.attackCount = 10;
    stat.releaseCount = 10;
    stat.rawPcmBytes = 6000000;
    stat.hasStats = true;

    // 24-bit stereo full hold: (600000 + 400000) * 3 * 2 + 128MB overhead
    mp::ui::OrganAudioConfig c24;
    c24.storage = mp::SampleStorage::Int24;
    c24.mono = false;
    c24.streamReleases = false;
    c24.preloadHeadFrames = 0;
    juce::int64 ram24 = mp::ui::estimateRamFootprintBytes(stat, c24);
    MP_CHECK(ram24 == (1000000LL * 6LL + 128LL * 1024 * 1024), "24-bit stereo matches formula");

    // 16-bit stereo full hold: (1000000) * 2 * 2 + 128MB overhead
    mp::ui::OrganAudioConfig c16;
    c16.storage = mp::SampleStorage::Int16;
    c16.mono = false;
    c16.streamReleases = false;
    c16.preloadHeadFrames = 0;
    juce::int64 ram16 = mp::ui::estimateRamFootprintBytes(stat, c16);
    MP_CHECK(ram16 == (1000000LL * 4LL + 128LL * 1024 * 1024), "16-bit stereo matches formula");

    // 16-bit mono full hold: (1000000) * 2 * 1 + 128MB overhead
    mp::ui::OrganAudioConfig cMono;
    cMono.storage = mp::SampleStorage::Int16;
    cMono.mono = true;
    cMono.streamReleases = false;
    cMono.preloadHeadFrames = 0;
    juce::int64 ramMono = mp::ui::estimateRamFootprintBytes(stat, cMono);
    MP_CHECK(ramMono == (1000000LL * 2LL + 128LL * 1024 * 1024), "16-bit mono matches formula");

    // 16-bit stereo stream releases (streamHead=44100):
    // resident attack = 600000, resident release = 10 * 44100 = 441000.
    // resident frames = 600000 + 441000 = 1041000, but capped at total release frames (400000) -> 600000 + 400000.
    // If streamHead = 10000: release = 10 * 10000 = 100000. resident frames = 700000.
    mp::ui::OrganAudioConfig cStream;
    cStream.storage = mp::SampleStorage::Int16;
    cStream.mono = false;
    cStream.streamReleases = true;
    cStream.streamHeadFrames = 10000;
    cStream.preloadHeadFrames = 0;
    juce::int64 ramStream = mp::ui::estimateRamFootprintBytes(stat, cStream);
    MP_CHECK(ramStream == (700000LL * 4LL + 128LL * 1024 * 1024), "stream releases reduces release RAM");

    // Check discoverOrgans filters out placeholder "Organ1"
    mp::MasterpieceProcessor proc;
    proc.addRecentOrgan(juce::File("/nonexistent/path/Organ1.Organ_Hauptwerk_xml"));
    auto organs = mp::ui::discoverOrgans(proc);
    bool foundDummyOrgan1 = false;
    for (const auto& o : organs) {
      if (o.name == "Organ1" && !o.exists) {
        foundDummyOrgan1 = true;
        break;
      }
    }
    MP_CHECK(!foundDummyOrgan1, "discoverOrgans eliminated missing dummy Organ1 entry");
  }
};

class OrganAsyncStatScanTest final : public mp::test::Test {
public:
  OrganAsyncStatScanTest()
      : Test("functional.organs.async-stat-scan", Category::Functional) {}
  void run() override {
    const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("mp_stat_test_" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()));
    tempDir.createDirectory();

    const auto odfFile = tempDir.getChildFile("TestOrgan.Organ_Hauptwerk_xml");
    odfFile.replaceWithText("<Organ><General Identification_UniqueOrganID=\"TEST_SCAN_ORGAN_999\"><Identification_Name>Test Organ</Identification_Name></General></Organ>");

    const auto pipeDir = tempDir.getChildFile("PipeSamples").getChildFile("000001");
    pipeDir.createDirectory();

    const auto wavFile = pipeDir.getChildFile("test.wav");
    {
      juce::WavAudioFormat wavFmt;
      std::unique_ptr<juce::AudioFormatWriter> writer(
          wavFmt.createWriterFor(wavFile.createOutputStream().release(), 44100.0, 2, 24, {}, 0));
      if (writer != nullptr) {
        juce::AudioBuffer<float> buf(2, 4410);
        buf.clear();
        writer->writeFromAudioSampleBuffer(buf, 0, 4410);
      }
    }

    mp::MasterpieceProcessor proc;
    auto entry = mp::ui::getOrganDetails(odfFile, proc);
    MP_CHECK(entry.exists, "entry exists");

    const auto cacheFile = mp::ui::getOrganAudioStatCacheFile(entry);
    if (cacheFile.existsAsFile()) cacheFile.deleteFile();

    MP_CHECK(!mp::ui::hasCachedOrganAudioStat(entry), "hasCachedOrganAudioStat is false before calculation");

    std::atomic<bool> cancelFlag{true};
    auto cancelledStat = mp::ui::computeOrganAudioStat(entry, proc, nullptr, &cancelFlag);
    MP_CHECK(!cancelledStat.hasStats, "cancelled stat aborted");
    MP_CHECK(!mp::ui::hasCachedOrganAudioStat(entry), "still not cached after cancel");

    cancelFlag.store(false);
    double reportedProgress = 0.0;
    int reportedFiles = 0;
    auto stat = mp::ui::computeOrganAudioStat(
        entry, proc,
        [&](double p, int cur, int tot) {
          reportedProgress = p;
          reportedFiles = cur;
        },
        &cancelFlag);

    MP_CHECK(stat.hasStats, "stat calculation succeeded");
    MP_CHECK(stat.totalAudioFrames == 4410, "scanned 4410 audio frames");
    MP_CHECK(reportedProgress >= 1.0, "progress reached 1.0");
    MP_CHECK(reportedFiles >= 1, "reported at least 1 file");
    MP_CHECK(mp::ui::hasCachedOrganAudioStat(entry), "hasCachedOrganAudioStat is true after scan");

    auto cachedStat = mp::ui::computeOrganAudioStat(entry, proc);
    MP_CHECK(cachedStat.hasStats, "cached stat loaded");
    MP_CHECK(cachedStat.totalAudioFrames == 4410, "cached totalAudioFrames matches");

    if (cacheFile.existsAsFile()) cacheFile.deleteFile();
    tempDir.deleteRecursively();
  }
};

#endif // MP_TEST_HAS_AUDIO

#ifdef MP_TEST_HAS_DSP
// Measures what the swell shades actually do to a signal, rather than only
// checking that the code runs: RMS of a sine through the filter at a given
// shutter position. Every assertion below is a property of a swell box.
class EnclosureResponseTest final : public mp::test::Test {
public:
  EnclosureResponseTest()
    : Test("functional.dsp.enclosure-response", Category::Functional) {}

  // RMS of `hz` fed through the enclosure held at `shutter`, after letting the
  // shutter smoother and the filter settle.
  static double rmsAt(double hz, double shutter, const mp::Enclosure& e,
                      const mp::EngineSwitch& sw, double sr = 48000.0) {
    mp::dsp::EnclosureFilter enc;
    enc.reset(sr);
    enc.snapTo(shutter);
    const double w = 2.0 * 3.141592653589793 * hz / sr;
    // Settle: one full shutter ramp plus filter ring-down.
    for (int i = 0; i < 12000; ++i)
      enc.process(static_cast<float>(std::sin(w * i)), e, shutter, sw);
    double acc = 0.0;
    const int n = 24000;
    for (int i = 0; i < n; ++i) {
      const double y = enc.process(static_cast<float>(std::sin(w * (12000 + i))),
                                   e, shutter, sw);
      acc += y * y;
    }
    return std::sqrt(acc / n);
  }

  void run() override {
    mp::EngineSwitch full;
    const mp::Enclosure e{1, "Swell", 0, 800.0, 12000.0, -24.0, 0.0};

    // Closing the box must attenuate, and must attenuate treble harder than
    // bass — that is the whole point of a swell shade.
    const double lowOpen = rmsAt(200.0, 1.0, e, full);
    const double lowShut = rmsAt(200.0, 0.0, e, full);
    const double highOpen = rmsAt(6000.0, 1.0, e, full);
    const double highShut = rmsAt(6000.0, 0.0, e, full);
    MP_CHECK(lowShut < lowOpen, "closing the box attenuates the bass");
    MP_CHECK(highShut < highOpen, "closing the box attenuates the treble");
    MP_CHECK((highShut / highOpen) < (lowShut / lowOpen),
             "a closed box rolls off treble more than bass");

    // The closed attenuation must land near the declared closedAttnDb at a
    // frequency well inside the closed passband (so the filter is not the
    // dominant term).
    const double closedDb = 20.0 * std::log10(rmsAt(100.0, 0.0, e, full) /
                                              rmsAt(100.0, 1.0, e, full));
    MP_CHECK(closedDb < -18.0 && closedDb > -30.0,
             "closed box attenuates a low tone by roughly closedAttnDb");

    // Open box at 0 dB with a 12 kHz corner must be near unity in the speech
    // range: the shades are out of the way.
    MP_CHECK(std::fabs(20.0 * std::log10(rmsAt(440.0, 1.0, e, full) /
                                         std::sqrt(0.5))) < 1.0,
             "an open box is within a dB of unity at 440 Hz");

    // Loudness must rise monotonically as the shoe opens - no dead spots or
    // reversals anywhere in the shoe's travel.
    double prev = -1.0;
    for (int step = 0; step <= 10; ++step) {
      const double r = rmsAt(440.0, step / 10.0, e, full);
      MP_CHECK(r > prev, "loudness rises monotonically as the shoe opens");
      prev = r;
    }

    // Out-of-range shutter values must be clamped, not extrapolated.
    MP_CHECK(std::fabs(rmsAt(440.0, 2.0, e, full) - rmsAt(440.0, 1.0, e, full)) < 1e-6,
             "a shutter above 1 clamps to fully open");
    MP_CHECK(std::fabs(rmsAt(440.0, -1.0, e, full) - rmsAt(440.0, 0.0, e, full)) < 1e-6,
             "a shutter below 0 clamps to fully closed");
  }
};

// A swell shoe is swept constantly while notes sound; the filter must stay
// well-behaved under that modulation and must never emit a non-finite sample.
class EnclosureModulationTest final : public mp::test::Test {
public:
  EnclosureModulationTest()
    : Test("functional.dsp.enclosure-modulation", Category::Functional) {}
  void run() override {
    mp::EngineSwitch full;
    const mp::Enclosure e{1, "Swell", 0, 800.0, 12000.0, -24.0, 0.0};
    mp::dsp::EnclosureFilter enc;
    enc.reset(48000.0);

    // Sweep the shoe as fast as a player can kick it, for ten seconds.
    double peak = 0.0;
    for (int i = 0; i < 480000; ++i) {
      const double shutter = 0.5 + 0.5 * std::sin(2.0 * 3.141592653589793 * 4.0 * i / 48000.0);
      const float x = static_cast<float>(std::sin(2.0 * 3.141592653589793 * 440.0 * i / 48000.0));
      const float y = enc.process(x, e, shutter, full);
      MP_CHECK(std::isfinite(y), "a swept enclosure never emits NaN or inf");
      peak = std::max(peak, std::fabs(static_cast<double>(y)));
    }
    MP_CHECK(peak < 2.0, "a swept enclosure does not blow up");

    // A degenerate enclosure (zero/negative corner frequencies from a broken
    // ODF) must be survivable, not a crash or a NaN factory.
    const mp::Enclosure bad{2, "Broken", 0, 0.0, -5.0, -24.0, 0.0};
    mp::dsp::EnclosureFilter badEnc;
    badEnc.reset(48000.0);
    for (int i = 0; i < 4800; ++i) {
      const float y = badEnc.process(0.5f, bad, 0.5, full);
      MP_CHECK(std::isfinite(y), "a malformed enclosure still produces finite audio");
    }

    // The filter must stay stable at any sample rate the host might hand us.
    for (double sr : {44100.0, 48000.0, 96000.0, 192000.0}) {
      mp::dsp::EnclosureFilter r;
      r.reset(sr);
      for (int i = 0; i < 4800; ++i)
        MP_CHECK(std::isfinite(r.process(0.5f, e, 0.7, full)),
                 "the enclosure is stable at every supported sample rate");
      MP_CHECK(r.cutoffHz() < sr * 0.5,
               "cutoff stays below Nyquist at every sample rate");
    }
  }
};

// The tremulant ramps its rate and depth rather than switching instantly, and
// both must stay bounded and monotonic whatever the ODF asks for.
class TremulantRampTest final : public mp::test::Test {
public:
  TremulantRampTest()
    : Test("functional.dsp.tremulant-ramp", Category::Functional) {}
  void run() override {
    mp::EngineSwitch full;
    const mp::Tremulant t{1, "Trem", 6.0, 5.0, 50.0, 50.0, false, 0};

    mp::dsp::TremulantLfo trem;
    trem.reset(48000.0);
    MP_CHECK(trem.currentDepth() == 0.0f, "a tremulant starts at zero depth");

    // Engaging ramps depth up monotonically and reaches full depth.
    //
    // "Monotonically" needs a tolerance, and the honest way to set one is to
    // measure the dip rather than guess at it. A linear ramp of ~25k steps
    // accumulates float error as it climbs, and JUCE snaps exactly to the
    // target on the last step, so that step can correct DOWNWARD by whatever
    // the accumulation had drifted upward. That artifact is a fraction of a
    // percent over one sample and inaudible; a real dip in a tremulant
    // envelope — a re-armed ramp, a reset mid-flight — would be orders of
    // magnitude larger and would be heard as a stutter.
    float prev = -1.0f;
    float worstDip = 0.0f;
    for (int i = 0; i < 48000; ++i) {
      trem.nextSample(t, true, full);
      const float now = trem.currentDepth();
      if (prev >= 0.0f) worstDip = std::max(worstDip, prev - now);
      prev = now;
    }
    MP_CHECK(worstDip < 0.01f,
             "engaging never dips the depth envelope audibly");
    MP_CHECK(trem.currentDepth() > 0.99f, "depth reaches full within a second");
    MP_CHECK(std::fabs(trem.currentRateHz() - t.engagedHz) < 0.05,
             "the rate glides to the engaged rate");

    // Cancelling ramps back down to silence.
    for (int i = 0; i < 96000; ++i) trem.nextSample(t, false, full);
    MP_CHECK(trem.currentDepth() < 0.01f, "cancelling returns the depth to zero");
    MP_CHECK(std::fabs(trem.currentRateHz() - t.disengagedHz) < 0.05,
             "the rate glides back to the disengaged rate");

    // snapTo skips the ramp for registration recall.
    mp::dsp::TremulantLfo snap;
    snap.reset(48000.0);
    snap.snapTo(t, true);
    MP_CHECK(snap.currentDepth() > 0.99f, "snapTo reaches full depth at once");

    // A faster start percentage must reach depth sooner than a slow one.
    auto samplesToFullDepth = [&](double startPercent) {
      mp::Tremulant tt = t;
      tt.startPercent = startPercent;
      mp::dsp::TremulantLfo lfo;
      lfo.reset(48000.0);
      for (int i = 0; i < 480000; ++i) {
        lfo.nextSample(tt, true, full);
        if (lfo.currentDepth() > 0.99f) return i;
      }
      return 480000;
    };
    MP_CHECK(samplesToFullDepth(100.0) < samplesToFullDepth(0.0),
             "a higher start percentage engages the tremulant sooner");

    // Output stays in [-1, 1] and finite even with a nonsense rate.
    mp::Tremulant wild = t;
    wild.engagedHz = 1.0e6;
    mp::dsp::TremulantLfo wildLfo;
    wildLfo.reset(48000.0);
    for (int i = 0; i < 48000; ++i) {
      const float y = wildLfo.nextSample(wild, true, full);
      MP_CHECK(std::isfinite(y) && std::fabs(y) <= 1.0001f,
               "a nonsense tremulant rate stays bounded and finite");
    }
  }
};

class DspFastPathTest final : public mp::test::Test {
public:
  DspFastPathTest()
    : Test("functional.dsp.simple-wav-fast-path", Category::Functional) {}
  void run() override {
    // ADR-005: simpleWavOnly must bypass every DSP block identity-wise.
    const mp::EngineSwitch simple;
    MP_CHECK(!simple.simpleWavOnly, "default EngineSwitch is full DSP");
    mp::EngineSwitch bypass = simple;
    bypass.simpleWavOnly = true; // ADR-005 bypass mode under test

    mp::EngineSwitch full; // full DSP path
    full.simpleWavOnly = false;

    // The wind solver has its own test now; here it only has to prove it obeys
    // the bypass, like every other DSP block.
    mp::OrganModel model;
    mp::WindSolver wind;
    wind.reset(model);
    const std::unordered_set<mp::Id> nothingEngaged;
    MP_CHECK(!wind.advance(0.001, bypass, nothingEngaged),
             "wind solver must no-op in simple mode");
    const auto mod = wind.modFor(1);
    MP_CHECK(mod.ampMul == 1.0 && mod.pitchRatio == 1.0,
             "and modulate nothing while it is off");

    mp::dsp::TremulantLfo trem;
    trem.reset(48000.0);
    const mp::Tremulant t{1, "Trem", 6.0, 5.0, 0.0, 100.0, false, 0};
    MP_CHECK(trem.nextSample(t, true, bypass) == 0.0f,
             "tremulant must output 0 in simple mode");
    for (int i = 0; i < 1000; ++i) {
      const float s = trem.nextSample(t, true, full);
      MP_CHECK(std::fabs(s) <= 1.0001f, "tremulant output bounded");
    }

    mp::dsp::EnclosureFilter enc;
    enc.reset(48000.0);
    const mp::Enclosure e{1, "Sw", 0, 800.0, 12000.0, -24.0, 0.0};
    MP_CHECK(enc.process(0.5f, e, 0.0, bypass) == 0.5f,
             "enclosure must pass through in simple mode");
    float out = 0.0f;
    for (int i = 0; i < 2000; ++i)
      out = enc.process(0.5f, e, 1.0, full);
    MP_CHECK(std::fabs(out - 0.5f) < 1e-3f,
             "open enclosure converges to identity (inertia smoothing)");
  }
};
#endif

// ---------------------------------------------------------------- perf
// CPU-time based (rusage / GetProcessTimes), floors relaxed in Debug builds
// (GO runs perf in Debug+Release; our CI perf job is Release-only, ADR-009).
// Engine render/stress benches join this category with M1.5/M2.2 (ROADMAP).

class OdfScanThroughputTest final : public mp::test::Test {
public:
  OdfScanThroughputTest()
    : Test("perf.odf.scan-throughput", Category::Perf) {}
  void run() override {
    std::string xml =
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"_General\"><_General>"
        "<Identification_UniqueOrganID>1</Identification_UniqueOrganID>"
        "</_General></ObjectList>"
        "<ObjectList ObjectType=\"Rank\"><Rank><RankID>7</RankID><Name>P</Name></Rank></ObjectList>"
        "<ObjectList ObjectType=\"Pipe_SoundEngine01\">";
    const int n = 60000;
    xml.reserve(static_cast<size_t>(n) * 220 + xml.size());
    for (int i = 0; i < n; ++i)
      xml += "<Pipe_SoundEngine01><PipeID>" + std::to_string(1000 + i) +
             "</PipeID><RankID>7</RankID><NormalMIDINoteNumber>" +
             std::to_string(36 + (i % 61)) +
             "</NormalMIDINoteNumber></Pipe_SoundEngine01>";
    xml += "</ObjectList></Hauptwerk>";

    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    mp::test::Timing t = mp::test::measure([&] {
      l.loadFromXmlString(xml, "perf.Organ_Hauptwerk_xml", o, m, d);
    });
    const double mbPerSec = (static_cast<double>(xml.size()) / 1e6) / t.wallSeconds;
#ifdef NDEBUG
    const double floorMbPerSec = 5.0;
#else
    const double floorMbPerSec = 1.0;
#endif
    std::printf("        %zu bytes in %.3fs wall / %.3fs cpu -> %.1f MB/s (floor %.1f)\n",
                xml.size(), t.wallSeconds, t.cpuSeconds, mbPerSec, floorMbPerSec);
    MP_CHECK(mbPerSec > floorMbPerSec,
             "ODF scan slower than floor (was " + std::to_string(mbPerSec) + ")");
  }
};

// Polyphony benchmark against the perf budget (ROADMAP: 500 voices on a
// laptop, 2000 on a console, underrun-free at 256-1024 frames). Reports the
// real-time ratio: how much faster than real time the render runs. Anything
// at or below 1.0 means the engine cannot keep up at this polyphony.
// The console target is 2000 voices, which one thread does not reach. This
// measures the same workload across the worker pool and reports the speedup,
// so a regression in the threading shows up as a number rather than a stall.
class VoicePolyphonyThreadedPerfTest final : public mp::test::Test {
public:
  VoicePolyphonyThreadedPerfTest()
    : Test("perf.voice.polyphony-threaded", Category::Perf) {}
  void run() override {
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlock = 256;
    constexpr int kVoices = 2000; // the console target
    constexpr int kBlocks = 200;

    auto measure = [&](int threads) {
      voicetest::Fixture fx;
      mp::VoiceEngine eng;
      eng.prepare(kSampleRate, kVoices, 2, kBlock);
      eng.setSampleProvider(fx.provider());
      if (threads > 1) eng.setRenderThreads(threads, 8);
      for (int i = 0; i < kVoices; ++i) {
        mp::VoiceStart st;
        st.pipe = &fx.pipe;
        st.layer = &fx.pipe.layers[0];
        st.ratio = 1.0 + 0.0005 * (i % 97);
        st.gain = 0.01f;
        eng.startVoice(st, static_cast<uint64_t>(i + 1));
      }
      std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
      float* out[2] = {l.data(), r.data()};
      // WALL time, not CPU time: std::clock() sums across threads, so it can
      // never show a speedup — it would rise with thread count even when the
      // render finishes sooner. What an audio callback cares about is elapsed
      // time against the block deadline.
      const auto t0 = std::chrono::steady_clock::now();
      for (int b = 0; b < kBlocks; ++b) {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        eng.render(out, 2, kBlock);
      }
      const auto t1 = std::chrono::steady_clock::now();
      return std::chrono::duration<double>(t1 - t0).count();
    };

    const double audioSec = (kBlocks * kBlock) / kSampleRate;
    const double oneThread = measure(1);
    const unsigned hw = std::thread::hardware_concurrency();
    const int threads = static_cast<int>(hw > 1 ? std::min(hw, 8u) : 2u);
    const double manyThreads = measure(threads);

    const double rtOne = audioSec / oneThread;
    const double rtMany = audioSec / manyThreads;
    std::printf("        %d voices @ %d frames: 1 thread %.3fs wall (%.2fx rt), "
                "%d threads %.3fs wall (%.2fx rt) -> %.2fx speedup\n",
                kVoices, kBlock, oneThread, rtOne, threads, manyThreads, rtMany,
                oneThread / manyThreads);

    MP_CHECK(oneThread > 0.0 && manyThreads > 0.0, "both runs were measurable");
    // The pool must actually help. A modest floor, because CI machines are
    // shared and a strict speedup target would be flaky — but a pool that
    // makes things SLOWER is a real regression and must fail here.
    MP_CHECK(manyThreads < oneThread,
             "the worker pool renders 2000 voices faster than one thread");
  }
};

// Is holding samples as 16-bit a good trade, or does converting on every tap
// eat what halving the bytes buys?
//
// The answer depends entirely on the working set, so this measures the case
// that actually matters. A benchmark that plays 512 voices out of ONE small
// buffer answers the wrong question: that buffer lives in cache, the loads
// never miss, and the conversion is pure added cost — measured that way 16-bit
// came out 30% SLOWER. A real organ holds gigabytes and every tap is a miss.
// So each voice here reads its own multi-second sample, and the pool is sized
// past any last-level cache.
class CompactStoragePerfTest final : public mp::test::Test {
public:
  CompactStoragePerfTest()
    : Test("perf.voice.polyphony-compact", Category::Perf) {}

  static constexpr int kVoices = 512;
  static constexpr int kFramesPerSample = 120000; // 2.5 s at 48k
  static constexpr int kBlock = 256;
  static constexpr int kBlocks = 200;

  // One pipe per voice, each with its own sample, so no two voices share a
  // cache line and the gather is as scattered as a real tutti's.
  struct Bank {
    std::vector<mp::Pipe> pipes;
    std::vector<mp::SampleBuffer> buffers;

    mp::SampleProvider provider() {
      return [this](mp::Id id) -> const mp::SampleBuffer* {
        const auto i = static_cast<size_t>(id) - 1;
        return i < buffers.size() ? &buffers[i] : nullptr;
      };
    }
    int64_t bytes() const {
      int64_t n = 0;
      for (const auto& b : buffers) n += b.residentBytes();
      return n;
    }
  };

  static Bank makeBank(bool compact) {
    Bank bank;
    bank.buffers.reserve(kVoices);
    bank.pipes.reserve(kVoices);
    for (int i = 0; i < kVoices; ++i) {
      // Detuned per voice: a real chord is never one pitch, and identical
      // buffers would let the allocator or the prefetcher flatter the result.
      auto b = voicetest::makeTone(220.0 + i * 0.37, 48000.0, kFramesPerSample);
      bank.buffers.push_back(compact ? CompactStorageTest::toInt16(b)
                                     : std::move(b));

      mp::Pipe pipe;
      pipe.pipeId = static_cast<mp::Id>(200 + i);
      pipe.midiNote = 36 + (i % 61);
      mp::PipeLayer layer;
      layer.layerId = 1;
      mp::AttackSample a;
      a.id = 11;
      a.sample.sampleId = static_cast<mp::Id>(i + 1);
      layer.attacks.push_back(a);
      pipe.layers.push_back(std::move(layer));
      bank.pipes.push_back(std::move(pipe));
    }
    return bank;
  }

  static double runPool(Bank& bank) {
    mp::VoiceEngine eng;
    eng.prepare(48000.0, kVoices, 2);
    eng.setSampleProvider(bank.provider());
    for (int i = 0; i < kVoices; ++i) {
      mp::VoiceStart st;
      st.pipe = &bank.pipes[static_cast<size_t>(i)];
      st.layer = &bank.pipes[static_cast<size_t>(i)].layers[0];
      st.velocity = 64;
      st.ratio = 1.0 + 0.0005 * (i % 97);
      st.gain = 0.02f;
      eng.startVoice(st, static_cast<uint64_t>(i + 1));
    }

    std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
    float* out[2] = {l.data(), r.data()};

    // Wall time, not std::clock: the engine may hand work to its pool, and CPU
    // time sums across threads, so it could never show a speedup.
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < kBlocks; ++b) {
      std::fill(l.begin(), l.end(), 0.0f);
      std::fill(r.begin(), r.end(), 0.0f);
      eng.render(out, 2, kBlock);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    const double audioSec = (kBlocks * kBlock) / 48000.0;
    return sec > 0.0 ? audioSec / sec : 9999.0;
  }

  void run() override {
    Bank wide = makeBank(false);
    Bank compact = makeBank(true);
    MP_CHECK(compact.bytes() * 2 == wide.bytes(),
             "the compact bank is exactly half the size");
    MP_CHECK(compact.buffers[0].loops() && wide.buffers[0].loops(),
             "both banks loop, or the pool would drain mid-run");

    const double wideRt = runPool(wide);
    const double compactRt = runPool(compact);

    std::printf("        %d voices over %.0f MB / %.0f MB working set:"
                " 32-bit float %.2fx rt, 16-bit %.2fx rt\n",
                kVoices, wide.bytes() / (1024.0 * 1024.0),
                compact.bytes() / (1024.0 * 1024.0), wideRt, compactRt);

    MP_CHECK(wideRt > 1.0 && compactRt > 1.0,
             "both resident formats render faster than real time");
  }
};

class VoicePolyphonyPerfTest final : public mp::test::Test {
public:
  VoicePolyphonyPerfTest()
    : Test("perf.voice.polyphony", Category::Perf) {}
  void run() override {
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlock = 256;
    constexpr int kVoices = 512;
    constexpr int kBlocks = 400; // ~2.1 s of audio

    voicetest::Fixture fx;
    mp::VoiceEngine eng;
    eng.prepare(kSampleRate, kVoices, 2);
    eng.setSampleProvider(fx.provider());

    // Fill the pool. Slightly detuned per voice so no two cursors march in
    // lockstep — that is what a real chord with temperament looks like, and it
    // defeats any accidental cache-friendliness a uniform ratio would give.
    for (int i = 0; i < kVoices; ++i) {
      mp::VoiceStart st;
      st.pipe = &fx.pipe;
      st.layer = &fx.pipe.layers[0];
      st.velocity = 64 + (i % 60);
      st.ratio = 1.0 + 0.0005 * (i % 97);
      st.gain = 0.02f;
      eng.startVoice(st, static_cast<uint64_t>(i + 1));
    }
    MP_CHECK(eng.activeVoiceCount() == kVoices, "the full pool is sounding");

    std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
    float* out[2] = {l.data(), r.data()};

    const auto t0 = std::clock();
    for (int b = 0; b < kBlocks; ++b) {
      std::fill(l.begin(), l.end(), 0.0f);
      std::fill(r.begin(), r.end(), 0.0f);
      eng.render(out, 2, kBlock);
    }
    const auto t1 = std::clock();

    const double cpuSec = static_cast<double>(t1 - t0) / CLOCKS_PER_SEC;
    const double audioSec = (kBlocks * kBlock) / kSampleRate;
    const double realtimeRatio = cpuSec > 0.0 ? audioSec / cpuSec : 9999.0;

    std::printf("        %d voices, %d-frame blocks: %.3fs cpu for %.2fs audio"
                " -> %.1fx realtime (floor 1.0)\n",
                kVoices, kBlock, cpuSec, audioSec, realtimeRatio);

    MP_CHECK(eng.activeVoiceCount() == kVoices,
             "every voice survived the run (loops kept them sounding)");
    MP_CHECK(realtimeRatio > 1.0,
             "512 voices must render faster than real time");
  }
};

class TemperamentThroughputTest final : public mp::test::Test {
public:
  TemperamentThroughputTest()
    : Test("perf.temperament.throughput", Category::Perf) {}
  void run() override {
    mp::Temperament t{"Equal", std::vector<double>(12, 0.0)};
    const int n = 1000000;
    volatile double sink = 0.0;
    mp::test::Timing tm = mp::test::measure([&] {
      double acc = 0.0;
      for (int i = 0; i < n; ++i)
        acc += mp::temperedPlaybackRatio(36 + (i % 61), 8, 440.0, 0.0, t, 0);
      sink = acc;
    });
    const double mops = static_cast<double>(n) / tm.wallSeconds / 1e6;
#ifdef NDEBUG
    const double floorMops = 5.0;
#else
    const double floorMops = 0.5;
#endif
    std::printf("        %d calls in %.3fs wall / %.3fs cpu -> %.2f M ops/s (floor %.2f)\n",
                n, tm.wallSeconds, tm.cpuSeconds, mops, floorMops);
    MP_CHECK(mops > floorMops,
             "temperament solver slower than floor (was " +
                 std::to_string(mops) + ")");
  }
};

class RoutingAllocationTest final : public mp::test::Test {
public:
  RoutingAllocationTest()
    : Test("functional.routing.allocation", Category::Functional) {}
  void run() override {
    mp::BusGroup g4{5, {mp::BusId{5}, mp::BusId{6}, mp::BusId{7}, mp::BusId{8}}}; // default group 0005
    // Deterministic: same pipe always lands on the same bus.
    MP_CHECK(mp::allocateBus(g4, 60, 201, mp::AllocationAlgorithm::StaticChromatic, 0) ==
                 mp::allocateBus(g4, 60, 201, mp::AllocationAlgorithm::StaticChromatic, 0),
             "allocation must be deterministic per (key, rank)");
    // Chromatic stepping spreads adjacent semitones across buses.
    const mp::BusId bC = mp::allocateBus(g4, 60, 201, mp::AllocationAlgorithm::StaticChromatic, 0);
    const mp::BusId bCs = mp::allocateBus(g4, 61, 201, mp::AllocationAlgorithm::StaticChromatic, 0);
    MP_CHECK(bC != bCs, "adjacent semitones must spread across buses");
    // Offset shifts the whole mapping (HW allocation-offset knob).
    MP_CHECK(mp::allocateBus(g4, 60, 201, mp::AllocationAlgorithm::StaticChromatic, 1) != bC,
             "note offset must shift the mapping");
    // Rank salt spreads ranks: for some key in the octave, two ranks differ.
    bool saltMatters = false;
    for (int k = 36; k < 48; ++k)
      if (mp::allocateBus(g4, k, 201, mp::AllocationAlgorithm::StaticChromatic, 0) !=
          mp::allocateBus(g4, k, 202, mp::AllocationAlgorithm::StaticChromatic, 0))
        saltMatters = true;
    MP_CHECK(saltMatters, "rank salt must spread ranks across buses somewhere");
    // Octave-cycled differs musically from chromatic for wide intervals.
    const mp::BusId oct = mp::allocateBus(g4, 72, 201, mp::AllocationAlgorithm::StaticOctaveCycled, 0);
    MP_CHECK(oct.value >= 5 && oct.value <= 8, "octave-cycled result stays in group");
    // Empty group = silence, never a crash (validator flags it).
    const mp::BusGroup empty{9, {}};
    MP_CHECK(mp::allocateBus(empty, 60, 201, mp::AllocationAlgorithm::StaticChromatic, 0).value == 0,
             "empty group must yield bus 0 (silent)");
    // Single-member group always hits it.
    const mp::BusGroup one{1, {mp::BusId{3}}};
    MP_CHECK(mp::allocateBus(one, 99, 7, mp::AllocationAlgorithm::StaticOctaveCycled, 5).value == 3,
             "single-member group is a fixed route");
    // Simple-routing defaults: perspectives 1-4 straight to buses 1-4.
    const mp::RankRouting simple = mp::simpleRoutingForRank(201);
    MP_CHECK(std::get<mp::BusId>(simple.perspectives[0].dest).value == 1 &&
                 std::get<mp::BusId>(simple.perspectives[3].dest).value == 4,
             "simple routing maps perspectives to buses 1-4");
  }
};

class FixtureStopsTest final : public mp::test::Test {
public:
  FixtureStopsTest()
    : Test("functional.fixtures.stops", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(loadFixture("minimal.Organ_Hauptwerk_xml", m, d),
             "minimal fixture must load");
    MP_CHECK(d.errors.empty(), "minimal fixture must load error-free");
    // Keyboards + divisions + link.
    MP_CHECK(m.keyboards.size() == 1, "one keyboard registered");
    MP_CHECK(m.divisions.size() == 1, "one division registered");
    const auto divIt = m.divisions.find(801);
    MP_CHECK(divIt != m.divisions.end(), "division 801 present");
    MP_CHECK(divIt->second.name == "Great", "division name parsed");
    MP_CHECK(divIt->second.keyboardIds.size() == 1 &&
                 divIt->second.keyboardIds.front() == 701,
             "keyboard 701 linked to division 801 via hint");
    // KeyAction flow.
    MP_CHECK(m.keyActions.size() == 1, "one key action registered");
    MP_CHECK(m.keyActions.front().destDivision == 801,
             "key action targets division 801");
    // Stop + mapping + switch.
    const auto stopIt = m.stops.find(901);
    MP_CHECK(stopIt != m.stops.end(), "stop 901 present");
    MP_CHECK(stopIt->second.divisionId == 801, "stop assigned to division 801");
    MP_CHECK(stopIt->second.controllingSwitchId == 1101,
             "stop controlling switch parsed");
    MP_CHECK(stopIt->second.defaultAsgnCode == 2100, "stop asgn code parsed");
    MP_CHECK(stopIt->second.ranks.size() == 1, "one stop-rank row linked");
    MP_CHECK(stopIt->second.ranks.front().rankId == 201,
             "stop-rank row points at rank 201");
    MP_CHECK(m.switches.size() == 1, "one switch registered");
    MP_CHECK(m.switches.find(1101) != m.switches.end(), "switch 1101 present");
    MP_CHECK(d.stopsWithoutRanks.empty(), "no stop lacks ranks");
  }
};

class ResolverTest final : public mp::test::Test {
public:
  ResolverTest() : Test("functional.resolve.key-on", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(loadFixture("minimal.Organ_Hauptwerk_xml", m, d),
             "minimal fixture must load");
    // Key 36 on Great with stop 901 drawn -> pipe 301.
    const std::unordered_set<mp::Id> drawn{901};
    const auto hit = mp::resolvePipes(m, 801, 36, drawn);
    MP_CHECK(hit.size() == 1, "drawn stop sounds exactly one pipe");
    MP_CHECK(hit.front().stopId == 901, "resolved stop 901");
    MP_CHECK(hit.front().rankId == 201, "resolved rank 201");
    MP_CHECK(hit.front().pipeId == 301, "resolved pipe 301");
    // Outside the 1-note mapping window -> silent.
    MP_CHECK(mp::resolvePipes(m, 801, 37, drawn).empty(),
             "unmapped key is silent");
    // Stop not drawn -> silent.
    MP_CHECK(mp::resolvePipes(m, 801, 36, {}).empty(),
             "undrawn stop is silent");
    // Wrong division -> silent.
    MP_CHECK(mp::resolvePipes(m, 999, 36, drawn).empty(),
             "other division is silent");
  }
};

class FixtureDisplayTest final : public mp::test::Test {
public:
  FixtureDisplayTest()
    : Test("functional.fixtures.display", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(loadFixture("minimal.Organ_Hauptwerk_xml", m, d),
             "minimal fixture must load");
    MP_CHECK(d.errors.empty(), "minimal fixture must load error-free");
    MP_CHECK(m.displayPages.size() == 1, "one display page registered");
    const auto pageIt = m.displayPages.find(1);
    MP_CHECK(pageIt != m.displayPages.end(), "page 1 present");
    MP_CHECK(pageIt->second.name == "Console", "page name parsed");
    MP_CHECK(pageIt->second.instances.size() == 1, "one instance on page");
    const mp::ImageSetInstance& inst = pageIt->second.instances.front();
    MP_CHECK(inst.imageSetId == 10, "instance links image set 10");
    MP_CHECK(inst.leftPx == 100 && inst.topPx == 200, "instance geometry parsed");
    MP_CHECK(inst.layer == 5, "instance layer parsed");
    MP_CHECK(pageIt->second.texts.size() == 1, "one text on page");
    MP_CHECK(pageIt->second.texts.front().text == "Masterpiece Test",
             "text content parsed");
    MP_CHECK(m.imageSets.size() == 1, "one image set registered");
    const auto setIt = m.imageSets.find(10);
    MP_CHECK(setIt != m.imageSets.end(), "image set 10 present");
    MP_CHECK(setIt->second.elements.size() == 2, "two frames in set");
    MP_CHECK(setIt->second.elements.front().bitmapFile == "knobs/knob_off.bmp",
             "element bitmap filename parsed");
    MP_CHECK(d.emptyDisplayPages.empty(), "console page is not empty");
    MP_CHECK(d.unreferencedImageSets.empty(), "set 10 is referenced");
    // With an organ root that has no images, all three bitmaps go missing.
    mp::OrganModel m2;
    mp::OdfDiagnostics d2;
    mp::OdfLoader l;
    mp::OdfLoader::Options o;
    o.organRootDir = "nonexistent-organ-root-xyz";
    MP_CHECK(l.loadFromXmlString(readFixture("minimal.Organ_Hauptwerk_xml"),
                                 "minimal.Organ_Hauptwerk_xml", o, m2, d2),
             "missing images warn, never fail (headless still plays)");
    MP_CHECK(d2.missingImageFiles.size() == 3, "mask + 2 frames reported missing");
  }
};

// Issue #12: a set reorganised with symbolic links -- OrganInstallationPackages
// itself, a single package folder inside it, or OrganDefinitions relocated
// onto another drive and linked back in -- has to resolve exactly like the
// same set laid out directly. std::filesystem already follows a symlink
// transparently in exists()/is_directory(), so what actually needed fixing
// was the M1.2 missing-sample check building the wrong path (it never looked
// under the sample's own installation package) and deriveOrganRoot needing to
// fall back to the ODF's resolved path when the one it was given does not
// have OrganInstallationPackages beside it.
//
// Windows refuses create_directory_symlink outright without Developer Mode
// enabled; each layout below is skipped (not failed) when that happens, so
// this test is a no-op verification stub on an unprivileged Windows machine
// and a real one everywhere CI runs it (Linux, macOS).
class SymlinkedOrganTest final : public mp::test::Test {
public:
  SymlinkedOrganTest()
    : Test("functional.loader.symlinked-organ", Category::Functional) {}

  void run() override {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base =
        fs::temp_directory_path(ec) / "mp_symlink_test_9f3a1c2e";
    if (ec) return; // no temp directory available; nothing to test against
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    if (ec) return; // read-only or otherwise unusable; skip cleanly

    struct Cleanup {
      fs::path p;
      ~Cleanup() { std::error_code e; std::filesystem::remove_all(p, e); }
    } cleanup{base};

    const std::string xml = readFixture("minimal.Organ_Hauptwerk_xml");
    MP_CHECK(!xml.empty(), "fixture must read");

    layoutPackagesFolderSymlinked(base, xml);
    layoutSinglePackageSymlinked(base, xml);
    layoutDefinitionsFolderSymlinked(base, xml);
    layoutDefinitionsLinkedOutOfTree(base, xml);
  }

private:
  static void writeFile(const std::filesystem::path& p, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f << content;
  }

  static bool trySymlinkDir(const std::filesystem::path& target,
                            const std::filesystem::path& link) {
    std::error_code ec;
    std::filesystem::create_directory_symlink(target, link, ec);
    // Said out loud when it happens. The layouts return early on a refusal,
    // and the test then reports PASS having checked nothing -- on a Windows
    // box without Developer Mode that is every layout. A skip that looks like
    // a pass is how a fix goes unexercised without anyone noticing.
    if (ec)
      std::cout << "        SKIPPED symlink layout (" << link.filename().string()
                << "): the OS refused to create a symlink -- " << ec.message()
                << "\n";
    return !ec;
  }

  // Layout A: OrganInstallationPackages itself is a symlink to a folder that
  // lives elsewhere (the common case: the audio is the bulk of a set, so it
  // is the folder actually worth relocating to another drive).
  void layoutPackagesFolderSymlinked(const std::filesystem::path& base,
                                     const std::string& xml) {
    namespace fs = std::filesystem;
    const fs::path root = base / "layoutA";
    const fs::path realPackages = base / "layoutA-real-packages";
    writeFile(root / "OrganDefinitions" / "test.Organ_Hauptwerk_xml", xml);
    writeFile(realPackages / "000001" / "001-C.wav", "x");
    writeFile(realPackages / "000001" / "001-C_Trem.wav", "x");
    if (!trySymlinkDir(realPackages, root / "OrganInstallationPackages")) return;

    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    o.organRootDir = root.string();
    MP_CHECK(l.loadFromXmlString(xml, "test.Organ_Hauptwerk_xml", o, m, d),
             "layout A: set must load");
    MP_CHECK(d.missingSampleFiles.empty(),
             "layout A: samples reached through a symlinked "
             "OrganInstallationPackages must be found");
  }

  // Layout B: an individual package folder under OrganInstallationPackages is
  // the symlink; the rest of the tree is ordinary.
  void layoutSinglePackageSymlinked(const std::filesystem::path& base,
                                    const std::string& xml) {
    namespace fs = std::filesystem;
    const fs::path root = base / "layoutB";
    const fs::path realPackage = base / "layoutB-real-package";
    writeFile(root / "OrganDefinitions" / "test.Organ_Hauptwerk_xml", xml);
    writeFile(realPackage / "001-C.wav", "x");
    writeFile(realPackage / "001-C_Trem.wav", "x");
    std::error_code ec;
    fs::create_directories(root / "OrganInstallationPackages", ec);
    if (!trySymlinkDir(realPackage, root / "OrganInstallationPackages" / "000001"))
      return;

    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    o.organRootDir = root.string();
    MP_CHECK(l.loadFromXmlString(xml, "test.Organ_Hauptwerk_xml", o, m, d),
             "layout B: set must load");
    MP_CHECK(d.missingSampleFiles.empty(),
             "layout B: samples reached through a symlinked package folder "
             "must be found");
  }

  // Layout C: the whole organ was found through a symlink whose own name (as
  // given to the loader) is not "OrganDefinitions" -- the closest a synthetic
  // test can get to what a macOS file dialog can hand back after resolving a
  // symlink partway through a path, where the component name the code goes
  // looking for is simply not there anymore. deriveOrganRoot has to notice
  // that the root implied by the given path has no OrganInstallationPackages
  // and fall back to the path's own resolved (canonical) form, which does.
  // Layout D, reported against 0.5.0: OrganDefinitions is a link whose TARGET
  // lives in a tree of its own, and the packages sit beside the link rather
  // than beside the target. Opened through the link the path leads to the
  // packages; opened through the resolved path -- which is what a file
  // chooser can hand back -- nothing in the path leads anywhere near them.
  // No search of the path can find them, so this is what the organ root
  // setting is for.
  void layoutDefinitionsLinkedOutOfTree(const std::filesystem::path& base,
                                        const std::string& xml) {
    namespace fs = std::filesystem;
    const fs::path setRoot = base / "layoutD-set";
    const fs::path defsElsewhere = base / "layoutD-defs";
    writeFile(defsElsewhere / "test.Organ_Hauptwerk_xml", xml);
    writeFile(setRoot / "OrganInstallationPackages" / "000001" / "001-C.wav", "x");
    writeFile(setRoot / "OrganInstallationPackages" / "000001" / "001-C_Trem.wav", "x");
    if (!trySymlinkDir(defsElsewhere, setRoot / "OrganDefinitions")) return;

    // Through the link, the plain parent walk already lands on the set.
    const fs::path throughLink =
        setRoot / "OrganDefinitions" / "test.Organ_Hauptwerk_xml";
    std::error_code ec;
    MP_CHECK(fs::equivalent(mp::deriveOrganRoot(throughLink.string()), setRoot, ec) && !ec,
             "layout D: the path as given leads to the packages");

    // Through the resolved path it cannot, and must not invent one.
    const fs::path resolved = defsElsewhere / "test.Organ_Hauptwerk_xml";
    const std::string derived = mp::deriveOrganRoot(resolved.string());
    MP_CHECK(!fs::is_directory(fs::path(derived) / "OrganInstallationPackages", ec),
             "layout D: a resolved path genuinely has no packages to find");

    // Naming the root is what loads it.
    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    o.organRootDir = setRoot.string();
    MP_CHECK(l.loadFromXmlString(xml, "test.Organ_Hauptwerk_xml", o, m, d),
             "layout D: set must load with the root named");
    MP_CHECK(d.missingSampleFiles.empty(),
             "layout D: samples must be found under the named root");
  }

  void layoutDefinitionsFolderSymlinked(const std::filesystem::path& base,
                                        const std::string& xml) {
    namespace fs = std::filesystem;
    const fs::path realRoot = base / "layoutC-real";
    writeFile(realRoot / "OrganDefinitions" / "test.Organ_Hauptwerk_xml", xml);
    writeFile(realRoot / "OrganInstallationPackages" / "000001" / "001-C.wav", "x");
    writeFile(realRoot / "OrganInstallationPackages" / "000001" / "001-C_Trem.wav", "x");

    const fs::path localRoot = base / "layoutC-local";
    std::error_code ec;
    fs::create_directories(localRoot, ec);
    // Linked under a name that is deliberately NOT "OrganDefinitions", so the
    // logical parent walk cannot recognise it and has to fall back.
    if (!trySymlinkDir(realRoot / "OrganDefinitions", localRoot / "LinkedDefs"))
      return;

    const fs::path odfPath = localRoot / "LinkedDefs" / "test.Organ_Hauptwerk_xml";
    const std::string derivedRoot = mp::deriveOrganRoot(odfPath.string());
    const bool sameAsReal = fs::equivalent(derivedRoot, realRoot, ec) && !ec;
    MP_CHECK(sameAsReal,
             "layout C: deriveOrganRoot must resolve through the symlink to "
             "find OrganInstallationPackages when the given path's own "
             "parent does not have one");

    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    o.organRootDir = derivedRoot;
    MP_CHECK(l.loadFromXmlString(xml, "test.Organ_Hauptwerk_xml", o, m, d),
             "layout C: set must load");
    MP_CHECK(d.missingSampleFiles.empty(),
             "layout C: samples must be found once the root is derived "
             "through the symlink");
  }
};

// Engraved console text: the stop names a set writes over its artwork rather
// than painting into it. Issue #1 was that these were parsed and then never
// drawn, which left such an organ with blank drawstops.
// A set that states no concert pitch. Hauptwerk reads a zero as "unstated"
// and tunes to 440; taking it literally tuned every pipe to 0 Hz, which is an
// organ that loads and draws and makes no sound.
class BasePitchZeroTest final : public mp::test::Test {
public:
  BasePitchZeroTest()
    : Test("functional.loader.base-pitch-zero", Category::Functional) {}
  void run() override {
    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    MP_CHECK(l.loadFromXmlString(
                 "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                 "<ObjectList ObjectType=\"_General\"><_General>"
                 "<Identification_Name>X</Identification_Name>"
                 "<Identification_UniqueOrganID>1</Identification_UniqueOrganID>"
                 "<AudioEngine_BasePitchHz>0</AudioEngine_BasePitchHz>"
                 "</_General></ObjectList></Hauptwerk>",
                 "a.Organ_Hauptwerk_xml", o, m, d),
             "an organ that states no pitch must load");
    MP_CHECK(m.basePitchHz == 440.0, "a stated zero means 440, not silence");
  }
};

// Which pitch source a Sample row DECLARES, and what happens when the
// declared one is empty. Friesach declares code 1 on 11400 of its 12146
// samples and leaves every pitch field blank; reading the fields and not the
// code resolved nothing, so every pipe served by a recording made for a
// different note played at that other note. In a mixture, where one recording
// commonly serves two or three pipes, the stop drifts into a chord.
class SamplePitchRouteTest final : public mp::test::Test {
public:
  SamplePitchRouteTest()
    : Test("functional.pitch.sample-route", Category::Functional) {}
  void run() override {
    const double a4 = 440.0;

    // Code 4: the exact frequency, believed as stated.
    {
      mp::SamplePitchInputs in;
      in.methodCode = 4;
      in.exactHz = 261.3;
      in.fileMidiNote = 69.0; // present, and must NOT win over the declaration
      const auto r = mp::resolveSamplePitch(in, a4);
      MP_CHECK(r.route == mp::PitchRoute::ExactHz, "code 4 is the exact Hz");
      MP_CHECK(std::abs(r.hz - 261.3) < 1e-9, "stated Hz, unaltered");
    }

    // Code 1: the file's own metadata, fraction and all.
    {
      mp::SamplePitchInputs in;
      in.methodCode = 1;
      in.fileMidiNote = 69.5; // half a semitone above concert A
      in.exactHz = 100.0;     // a stale neighbour that must not be used
      const auto r = mp::resolveSamplePitch(in, a4);
      MP_CHECK(r.route == mp::PitchRoute::FileMetadata, "code 1 is the file");
      const double want = 440.0 * std::pow(2.0, 0.5 / 12.0);
      MP_CHECK(std::abs(r.hz - want) < 1e-6, "the fraction is kept, not rounded");
    }

    // Code 3: the note AND the harmonic number. A 1 1/3' rank (harmonic 48)
    // sounds 31 semitones above the note its pipes are keyed at; dropping the
    // harmonic puts the rank two and a half octaves flat.
    {
      mp::SamplePitchInputs in;
      in.methodCode = 3;
      in.normalMidiNote = 69;
      in.rankBasePitch64ftHarmonicNum = 48;
      const auto r = mp::resolveSamplePitch(in, a4);
      MP_CHECK(r.route == mp::PitchRoute::Tempered, "code 3 is the fields");
      const double want = 440.0 * (48.0 / 8.0);
      MP_CHECK(std::abs(r.hz - want) < 1e-6, "harmonic 48 is six times 8 foot");
    }

    // Code 0 says apply nothing, and codes 2 and 5 are tremulant waveforms.
    // All three must resolve to "no pitch", which the caller plays as it is --
    // NOT fall through to a neighbouring field.
    for (int code : {0, 2, 5}) {
      mp::SamplePitchInputs in;
      in.methodCode = code;
      in.exactHz = 261.6;
      in.fileMidiNote = 60.0;
      const auto r = mp::resolveSamplePitch(in, a4);
      MP_CHECK(r.hz == 0.0, "a declared no-tuning code applies no tuning");
      MP_CHECK(r.route != mp::PitchRoute::ExactHz, "and does not fall through");
    }

    // No code at all: take the most precise field actually present.
    {
      mp::SamplePitchInputs in;
      in.fileMidiNote = 72.0;
      const auto r = mp::resolveSamplePitch(in, a4);
      MP_CHECK(r.route == mp::PitchRoute::FileMetadata,
               "with no code, metadata beats nothing");
    }

    // Nothing declared anywhere: the file name is the last resort, and it is
    // reported as such so a caller can say the pitch was guessed.
    {
      mp::SamplePitchInputs in;
      in.methodCode = 1;          // says "the file knows"...
      in.fileMidiNote = -1.0;     // ...and the file does not
      in.fileName = "far/09_Subbas16/036-c.wav";
      const auto r = mp::resolveSamplePitch(in, a4);
      MP_CHECK(r.route == mp::PitchRoute::Filename, "the name is last");
      const double want = 440.0 * std::pow(2.0, (36.0 - 69.0) / 12.0);
      MP_CHECK(std::abs(r.hz - want) < 1e-6, "036 means MIDI note 36");
    }

    // A noise sample's placeholder frequency is not a pitch. Friesach ships
    // no Noise table, so its key- and stop-action ranks are ordinary ranks
    // keyed 1..88 against a declared 100 Hz; believing it transposed a
    // tracker click down by as much as forty semitones.
    {
      mp::SamplePitchInputs in;
      in.methodCode = 4;
      in.exactHz = 100.0;
      const auto r = mp::resolveSamplePitch(in, a4);
      MP_CHECK(r.route == mp::PitchRoute::NoisePlaceholder,
               "100 Hz exactly is the documented stand-in");
      MP_CHECK(r.hz == 0.0, "so the click plays as recorded");
    }
    {
      // The other spelling of the same placeholder: the organ's base pitch.
      mp::SamplePitchInputs in;
      in.methodCode = 4;
      in.exactHz = 465.0;
      const auto r = mp::resolveSamplePitch(in, a4, 465.0);
      MP_CHECK(r.route == mp::PitchRoute::NoisePlaceholder,
               "the base pitch is the other stand-in");
    }
    {
      // ...but a real pipe that happens to sit near it is still a pipe: the
      // rule is exact equality, and a file that declares its own note is
      // evidence the sample is pitched after all.
      mp::SamplePitchInputs in;
      in.methodCode = 4;
      in.exactHz = 100.5;
      const auto r = mp::resolveSamplePitch(in, a4);
      MP_CHECK(r.route == mp::PitchRoute::ExactHz, "100.5 Hz is a pitch");
      // The declaration beats the file, though: Friesach's key-action
      // releases share one silent BlankLoop.wav whose metadata claims note
      // 98.3, and obeying that transposed a blank by six semitones.
      mp::SamplePitchInputs in2;
      in2.methodCode = 4;
      in2.exactHz = 100.0;
      in2.fileMidiNote = 98.336;
      const auto r2 = mp::resolveSamplePitch(in2, a4);
      MP_CHECK(r2.route == mp::PitchRoute::NoisePlaceholder,
               "the set saying 'unpitched' beats the file's own metadata");
    }

    // And when even that says nothing, say so rather than inventing a pitch.
    {
      mp::SamplePitchInputs in;
      in.fileName = "Noises/Blower.wav";
      const auto r = mp::resolveSamplePitch(in, a4);
      MP_CHECK(r.route == mp::PitchRoute::Unresolved, "no pitch is not 440");
      MP_CHECK(r.hz == 0.0, "zero, which the caller plays unaltered");
    }
  }
};

// The leading digits of a file name, which are a MIDI note by convention in
// every set that ships one -- and are not a note when they are part of a
// longer number.
class SampleFileNameNoteTest final : public mp::test::Test {
public:
  SampleFileNameNoteTest()
    : Test("functional.pitch.filename-note", Category::Functional) {}
  void run() override {
    MP_CHECK(mp::midiNoteFromFileName("036-c.wav") == 36, "036 is 36");
    MP_CHECK(mp::midiNoteFromFileName("091-g.wav") == 91, "091 is 91");
    MP_CHECK(mp::midiNoteFromFileName("far/A0/037-c#.wav") == 37, "after a slash");
    MP_CHECK(mp::midiNoteFromFileName("far\\A0\\037-c#.wav") == 37, "after a backslash");
    MP_CHECK(mp::midiNoteFromFileName("Blower.wav") == -1, "no digits, no note");
    MP_CHECK(mp::midiNoteFromFileName("1234-c.wav") == -1,
             "four digits are a serial number, not note 123");
    MP_CHECK(mp::midiNoteFromFileName("") == -1, "an empty name is not a note");
  }
};

// The ODF must carry the method code into the model. Before this it was
// parsed nowhere, so the one field that says which pitch to believe was the
// one field the loader ignored.
class SamplePitchCodeParsedTest final : public mp::test::Test {
public:
  SamplePitchCodeParsedTest()
    : Test("functional.loader.sample-pitch-code", Category::Functional) {}
  void run() override {
    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    MP_CHECK(l.loadFromXmlString(
                 "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                 "<ObjectList ObjectType=\"_General\"><_General>"
                 "<Identification_Name>X</Identification_Name>"
                 "<Identification_UniqueOrganID>1</Identification_UniqueOrganID>"
                 "</_General></ObjectList>"
                 "<ObjectList ObjectType=\"Sample\"><Sample>"
                 "<SampleID>7</SampleID>"
                 "<SampleFilename>R/036-c.wav</SampleFilename>"
                 "<Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
                 "<Pitch_ExactSamplePitch></Pitch_ExactSamplePitch>"
                 "</Sample></ObjectList></Hauptwerk>",
                 "a.Organ_Hauptwerk_xml", o, m, d),
             "a set that names its pitch method must load");
    const auto it = m.samples.find(7);
    MP_CHECK(it != m.samples.end(), "the sample row is in the registry");
    MP_CHECK(it->second.pitchMethodCode == 1, "code 1 is carried into the model");
    MP_CHECK(it->second.pitchHz == 0.0, "an empty pitch field stays empty");

    // The compact v7 spelling of the same field.
    mp::OrganModel m2;
    mp::OdfDiagnostics d2;
    MP_CHECK(l.loadFromXmlString(
                 "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                 "<ObjectList ObjectType=\"_General\"><_General>"
                 "<c>X</c><b>1</b></_General></ObjectList>"
                 "<ObjectList ObjectType=\"Sample\">"
                 "<o><a>7</a><c>R/036-c.wav</c><d>4</d><g>261.3</g></o>"
                 "</ObjectList></Hauptwerk>",
                 "a.Organ_Hauptwerk_xml", o, m2, d2),
             "the compact spelling must load too");
    const auto it2 = m2.samples.find(7);
    MP_CHECK(it2 != m2.samples.end(), "the compact sample row is registered");
    MP_CHECK(it2->second.pitchMethodCode == 4, "short code d is the method");
    MP_CHECK(std::abs(it2->second.pitchHz - 261.3) < 1e-9, "short code g is the Hz");
  }
};

// One manual per channel. A saved mapping on a real machine had three manuals
// all claiming channel 1: two of them became unplayable, the on-screen manual
// selector ticked three entries at once, and every manual sounded the pedal.
// setKeyboardForChannel's own comment always said a channel is taken rather
// than shared; it only removed the previous claim of the same KEYBOARD.
class MidiChannelExclusiveTest final : public mp::test::Test {
public:
  MidiChannelExclusiveTest()
    : Test("functional.midi.channel-is-exclusive", Category::Functional) {}
  void run() override {
    mp::MidiMap map;

    auto claim = [&map](int channel, mp::Id keyboardId, int deviceId) {
      map.removeKeyboardBindingsFor(keyboardId);
      map.releaseChannel(channel, deviceId, keyboardId);
      mp::MidiMap::KeyboardBinding b;
      b.channel = channel;
      b.deviceId = deviceId;
      b.keyboardId = keyboardId;
      map.addKeyboardBinding(b);
    };

    // Three manuals, each in turn told to play channel 1.
    claim(1, 1, 0);
    claim(1, 2, 0);
    claim(1, 3, 0);
    MP_CHECK(map.keyboardBindings().size() == 1,
             "a channel is taken, not shared: one binding survives");
    MP_CHECK(map.keyboardBindings().front().keyboardId == 3,
             "the last claim wins");

    // Distinct channels coexist, which is the ordinary case.
    claim(2, 2, 0);
    claim(3, 1, 0);
    MP_CHECK(map.keyboardBindings().size() == 3, "distinct channels coexist");

    // Every keyboard answers to exactly one channel, so a menu keyed by
    // channel cannot collapse two manuals onto one entry.
    std::set<int> channels;
    std::set<mp::Id> keyboards;
    for (const auto& b : map.keyboardBindings()) {
      MP_CHECK(channels.insert(b.channel).second, "no channel claimed twice");
      MP_CHECK(keyboards.insert(b.keyboardId).second, "no keyboard bound twice");
    }

    // A named console and "any console" collide: they can both receive the
    // same channel, so one must still give way.
    mp::MidiMap named;
    auto claimOn = [&named](int channel, mp::Id kb, int dev) {
      named.removeKeyboardBindingsFor(kb);
      named.releaseChannel(channel, dev, kb);
      mp::MidiMap::KeyboardBinding b;
      b.channel = channel;
      b.deviceId = dev;
      b.keyboardId = kb;
      named.addKeyboardBinding(b);
    };
    claimOn(1, 1, 0);   // any console
    claimOn(1, 2, 7);   // console 7 takes channel 1
    MP_CHECK(named.keyboardBindings().size() == 1,
             "any-console and a named console cannot share a channel");
  }
};

// The mapping found on a real machine, verbatim: three manuals on channel 1.
// Loaded as it was, two manuals were unplayable and every manual sounded the
// pedal. It must come back repaired, with the organ's own channels in charge.
class MidiMapRepairTest final : public mp::test::Test {
public:
  MidiMapRepairTest() : Test("functional.midi.repair-saved-map", Category::Functional) {}
  void run() override {
    const std::string corrupt =
        "# Masterpiece MIDI map\n"
        "# <source> <channel> <number> <target> <id> <latching> <invert>\n"
        "manual 3 1 0 127 0 1 127 0 0 0 any\n"
        "manual 2 1 0 127 0 1 127 0 0 0 any\n"
        "manual 1 1 0 127 0 1 127 0 0 0 any\n";
    const std::vector<mp::Id> playable{1, 2, 3, 4};

    mp::MidiMap map;
    MP_CHECK(map.fromText(corrupt), "the damaged file still parses");
    MP_CHECK(map.keyboardBindings().size() == 3, "all three claims were read");
    MP_CHECK(map.repairKeyboardBindings(playable) == 3,
             "every manual in the collision gives way");
    MP_CHECK(map.keyboardBindingsEmpty(),
             "nothing ambiguous survives: the organ's own channels apply");

    // A sound mapping is left exactly alone.
    mp::MidiMap good;
    good.fromText("manual 2 1 0 127 0 1 127 0 0 0 any\n"
                  "manual 3 2 0 127 0 1 127 0 0 0 any\n");
    MP_CHECK(good.repairKeyboardBindings(playable) == 0, "a good mapping is kept");
    MP_CHECK(good.keyboardBindings().size() == 2, "both assignments survive");

    // A split keyboard -- one manual bound twice -- is legitimate.
    mp::MidiMap split;
    split.fromText("manual 2 1 36 60 0 1 127 0 0 0 any\n"
                   "manual 2 1 61 96 0 1 127 0 0 0 any\n");
    MP_CHECK(split.repairKeyboardBindings(playable) == 0,
             "one manual bound twice is not a collision");

    // A manual this organ does not have is stale, whatever else is right.
    mp::MidiMap stale;
    stale.fromText("manual 9 5 0 127 0 1 127 0 0 0 any\n"
                   "manual 2 2 0 127 0 1 127 0 0 0 any\n");
    MP_CHECK(stale.repairKeyboardBindings(playable) == 1, "the stale manual goes");
    MP_CHECK(stale.keyboardBindings().size() == 1 &&
                 stale.keyboardBindings().front().keyboardId == 2,
             "the valid one stays");

    // Round trip: the repaired map written out and read back needs no repair.
    mp::MidiMap again;
    again.fromText(map.toText());
    MP_CHECK(again.repairKeyboardBindings(playable) == 0,
             "a repaired file stays repaired");
  }
};

class DisplayTextTest final : public mp::test::Test {
public:
  DisplayTextTest()
    : Test("functional.display.text-instances", Category::Functional) {}
  void run() override {
    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    MP_CHECK(l.loadFromXmlString(
                 "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                 "<ObjectList ObjectType=\"_General\"><_General>"
                 "<Identification_Name>X</Identification_Name>"
                 "<Identification_UniqueOrganID>1</Identification_UniqueOrganID>"
                 "</_General></ObjectList>"
                 "<ObjectList ObjectType=\"DisplayPage\">"
                 "<DisplayPage><PageID>1</PageID><Name>Console</Name></DisplayPage>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"TextStyle\">"
                 "<TextStyle><StyleID>5</StyleID><Name>Engraved</Name>"
                 "<Face_WindowsName>Times New Roman</Face_WindowsName>"
                 "<Font_SizePixels>14</Font_SizePixels>"
                 "<Font_WeightCode>3</Font_WeightCode>"
                 "<Colour_Red>200</Colour_Red><Colour_Green>180</Colour_Green>"
                 "<Colour_Blue>120</Colour_Blue>"
                 "<HorizontalAlignmentCode>1</HorizontalAlignmentCode>"
                 "<VerticalAlignmentCode>2</VerticalAlignmentCode>"
                 "</TextStyle></ObjectList>"
                 "<ObjectList ObjectType=\"TextInstance\">"
                 "<TextInstance><TextInstanceID>7</TextInstanceID>"
                 "<TextStyleID>5</TextStyleID><Text>Tibia Clausa 8</Text>"
                 "<DisplayPageID>1</DisplayPageID>"
                 "<XPosPixels>40</XPosPixels><YPosPixels>60</YPosPixels>"
                 "<AttachedToAnImageSetInstance>Y</AttachedToAnImageSetInstance>"
                 "<AttachedToImageSetInstanceID>20</AttachedToImageSetInstanceID>"
                 "<PosRelativeToTopLeftOfImageSetInstance>Y"
                 "</PosRelativeToTopLeftOfImageSetInstance>"
                 "</TextInstance>"
                 "<TextInstance><TextInstanceID>8</TextInstanceID>"
                 "<Text>Vox Humana</Text><DisplayPageID>1</DisplayPageID>"
                 "</TextInstance></ObjectList>"
                 "</Hauptwerk>",
                 "a.Organ_Hauptwerk_xml", o, m, d),
             "an organ whose labels are text must load");

    const auto page = m.displayPages.find(1);
    MP_CHECK(page != m.displayPages.end() && page->second.texts.size() == 2,
             "both labels reach the page");
    const auto& t = page->second.texts.front();
    MP_CHECK(t.text == "Tibia Clausa 8", "the label keeps its text");
    MP_CHECK(t.styleId == 5 && t.xPx == 40 && t.yPx == 60, "style and anchor read");
    MP_CHECK(t.attachedInstanceId == 20 && t.posRelativeToInstance,
             "a label attached to a knob is positioned from that knob");

    const auto st = m.textStyles.find(5);
    MP_CHECK(st != m.textStyles.end(), "the style is loaded");
    MP_CHECK(st->second.faceWindows == "Times New Roman" && st->second.sizePx == 14,
             "face and size read");
    MP_CHECK(st->second.weightCode == 3, "bold read");
    MP_CHECK(st->second.red == 200 && st->second.green == 180 && st->second.blue == 120,
             "colour read");
    MP_CHECK(st->second.hAlignCode == 1 && st->second.vAlignCode == 2,
             "alignment codes read");

    // A label with no style of its own is still drawn: Hauptwerk's defaults
    // are Arial 10, black, centred across its position and hung from its top.
    const auto& plain = page->second.texts.back();
    MP_CHECK(plain.styleId == 0 && plain.attachedInstanceId == 0,
             "an unattached, unstyled label is loaded as such");
    mp::TextStyle dflt;
    MP_CHECK(dflt.sizePx == 10 && dflt.weightCode == 2 && dflt.hAlignCode == 0 &&
                 dflt.vAlignCode == 1,
             "the defaults are Hauptwerk's own");
  }
};

class DisplayEmptyTest final : public mp::test::Test {
public:
  DisplayEmptyTest()
    : Test("functional.display.empty-and-orphan", Category::Functional) {}
  void run() override {
    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    MP_CHECK(l.loadFromXmlString(
                 "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                 "<ObjectList ObjectType=\"_General\"><_General>"
                 "<Identification_Name>X</Identification_Name>"
                 "<Identification_UniqueOrganID>1</Identification_UniqueOrganID>"
                 "</_General></ObjectList>"
                 "<ObjectList ObjectType=\"DisplayPage\">"
                 "<DisplayPage><PageID>1</PageID><Name>Empty</Name></DisplayPage>"
                 "<DisplayPage><PageID>2</PageID><Name>Full</Name></DisplayPage>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"ImageSet\">"
                 "<ImageSet><ImageSetID>10</ImageSetID><Name>Used</Name></ImageSet>"
                 "<ImageSet><ImageSetID>11</ImageSetID><Name>Orphan</Name></ImageSet>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"ImageSetElement\">"
                 "<ImageSetElement><ImageSetID>10</ImageSetID>"
                 "<ImageIndexWithinSet>1</ImageIndexWithinSet><Name>A</Name>"
                 "<BitmapFilename>a.bmp</BitmapFilename></ImageSetElement>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"ImageSetInstance\">"
                 "<ImageSetInstance><ImageSetInstanceID>20</ImageSetInstanceID>"
                 "<ImageSetID>10</ImageSetID><DisplayPageID>2</DisplayPageID>"
                 "</ImageSetInstance></ObjectList>"
                 "</Hauptwerk>",
                 "a.Organ_Hauptwerk_xml", o, m, d),
             "display edge case must load");
    MP_CHECK(d.emptyDisplayPages.size() == 1 && d.emptyDisplayPages.front() == 1,
             "empty page 1 flagged, page 2 not");
    MP_CHECK(d.unreferencedImageSets.size() == 1 && d.unreferencedImageSets.front() == 11,
             "orphan set 11 flagged, used set 10 not");
  }
};

class FixtureCombinationsTest final : public mp::test::Test {
public:
  FixtureCombinationsTest()
    : Test("functional.fixtures.combinations", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(loadFixture("minimal.Organ_Hauptwerk_xml", m, d),
             "minimal fixture must load");
    MP_CHECK(d.errors.empty(), "minimal fixture must load error-free");
    const auto comboIt = m.combinations.find(1201);
    MP_CHECK(comboIt != m.combinations.end(), "combination 1201 present");
    MP_CHECK(comboIt->second.name == "General 1", "combination name parsed");
    MP_CHECK(comboIt->second.type == 2, "combination type code parsed");
    MP_CHECK(comboIt->second.elements.size() == 1, "one element linked");
    const mp::CombinationElement& el = comboIt->second.elements.front();
    MP_CHECK(el.controlledSwitchId == 1101, "controlled switch parsed");
    MP_CHECK(el.capturedSwitchId == 1101, "captured switch parsed");
    MP_CHECK(!el.storedEngaged,
             "the stored state defaults to out - an organ ships its pistons "
             "empty and the player fills them");
    MP_CHECK(!el.invertWhenActivating, "and nothing is inverted on the way out");
    MP_CHECK(d.danglingIds.empty(), "no dangling combination refs");
  }
};

class CombinationDanglingTest final : public mp::test::Test {
public:
  CombinationDanglingTest()
    : Test("functional.combinations.dangling", Category::Functional) {}
  void run() override {
    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    MP_CHECK(l.loadFromXmlString(
                 "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                 "<ObjectList ObjectType=\"_General\"><_General>"
                 "<Identification_Name>X</Identification_Name>"
                 "<Identification_UniqueOrganID>1</Identification_UniqueOrganID>"
                 "</_General></ObjectList>"
                 "<ObjectList ObjectType=\"Switch\">"
                 "<Switch><SwitchID>5</SwitchID><Name>S</Name></Switch>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"Combination\">"
                 "<Combination><CombinationID>9</CombinationID><Name>G</Name>"
                 "<CombinationTypeCode>2</CombinationTypeCode></Combination>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"CombinationElement\">"
                 "<CombinationElement><CombinationElementID>1</CombinationElementID>"
                 "<CombinationID>9</CombinationID>"
                 "<ControlledSwitchID>5</ControlledSwitchID>"
                 "<CapturedSwitchID>777</CapturedSwitchID></CombinationElement>"
                 "<CombinationElement><CombinationElementID>2</CombinationElementID>"
                 "<CombinationID>888</CombinationID>"
                 "<ControlledSwitchID>5</ControlledSwitchID>"
                 "<CapturedSwitchID>5</CapturedSwitchID></CombinationElement>"
                 "</ObjectList></Hauptwerk>",
                 "a.Organ_Hauptwerk_xml", o, m, d),
             "dangling combination refs report, never fail");
    bool cap777 = false, combo888 = false;
    for (mp::Id id : d.danglingIds) {
      if (id == 777) cap777 = true;
      if (id == 888) combo888 = true;
    }
    MP_CHECK(cap777, "captured switch 777 reported dangling");
    MP_CHECK(combo888, "combination 888 reported dangling");
    MP_CHECK(m.combinations.find(9) != m.combinations.end() &&
                 m.combinations.find(9)->second.elements.size() == 1,
             "valid element still linked");
  }
};

class CodmStructuralTest final : public mp::test::Test {
public:
  CodmStructuralTest()
    : Test("functional.codm.structure", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(loadFixture("minimal.CustomOrgan_Hauptwerk_xml", m, d),
             "CODM fixture must load");
    MP_CHECK(m.odfType == mp::OdfType::Codm, "CODM type detected");
    MP_CHECK(m.uniqueOrganId == 90002, "CODM UniqueOrganID parsed");
    MP_CHECK(m.organName == "Masterpiece CODM Test Church", "CODM name parsed");
    const auto pageIt = m.displayPages.find(1);
    MP_CHECK(pageIt != m.displayPages.end(), "CODM display page compiled");
    MP_CHECK(pageIt->second.name == "Console", "CODM page name parsed");
    // M1.5: the division table now compiles into a Division + its keyboard.
    MP_CHECK(m.divisions.size() == 1, "CODM division compiled");
    MP_CHECK(m.keyboards.size() == 1, "each CODM division gets one keyboard");
    for (const auto& t : d.codmUnmappedElements)
      MP_CHECK(t != "division", "division must no longer be unmapped");
    bool futureWarned = false;
    for (const auto& w : d.warnings)
      if (w.find("somefuturetable") != std::string::npos) futureWarned = true;
    MP_CHECK(futureWarned, "unknown ObjectType warns (forward-compat)");
    bool fieldListed = false;
    for (const auto& f : d.codmUnhandledFields)
      if (f == "customdisplaypage.BackgroundImageFilename") fieldListed = true;
    MP_CHECK(fieldListed, "ignored CODM fields listed for verification");
    MP_CHECK(!m.combinations.empty(), "CODM defaults applied (setter)");
  }
};

class CodmExample2Test final : public mp::test::Test {
public:
  CodmExample2Test()
    : Test("functional.codm.example2", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(loadFixture("example2-equivalent.CustomOrgan_Hauptwerk_xml", m, d),
             "Example2-equivalent must load");
    MP_CHECK(m.odfType == mp::OdfType::Codm, "CODM type detected");
    MP_CHECK(m.uniqueOrganId == 90003, "Example2 organ ID parsed");
    MP_CHECK(d.errors.empty(), "no errors on rich CODM file");
    // M1.5: every attested CODM table compiles into real model objects.
    MP_CHECK(m.divisions.size() == 3, "3 divisions compiled");
    MP_CHECK(m.keyboards.size() == 3, "one keyboard per division");
    MP_CHECK(m.ranks.size() == 2, "2 ranks compiled");
    MP_CHECK(m.stops.size() == 2, "2 stops compiled");
    MP_CHECK(m.tremulants.size() == 1, "1 tremulant compiled");
    MP_CHECK(m.enclosures.size() == 1, "1 enclosure compiled");
    MP_CHECK(m.keyActions.size() == 1, "coupler compiled into a key action");
    for (const auto& t : d.codmUnmappedElements)
      MP_CHECK(t != "division" && t != "rank" && t != "stop" && t != "coupler" &&
                   t != "tremulant" && t != "enclosure",
               "attested CODM tables must no longer be unmapped");

    // Division order fixes ids and manual numbers: Great, Swell, Pedal.
    const auto great = m.divisions.find(1000);
    const auto pedal = m.divisions.find(1002);
    MP_CHECK(great != m.divisions.end() && great->second.name == "Great",
             "first division keeps file order id 1000");
    MP_CHECK(great->second.manualNumber == 1, "Great is manual 1");
    MP_CHECK(pedal != m.divisions.end() && pedal->second.manualNumber == 0,
             "a division named Pedal becomes manual 0");
    MP_CHECK(m.keyboards.at(1102).numKeys == 32, "pedalboard is 32 notes");
    MP_CHECK(m.keyboards.at(1100).numKeys == 61, "manuals are 61 notes");

    // Windchest pressure drop declared -> a modelled (non-infinite) compartment.
    MP_CHECK(m.wind.size() == 2, "two divisions declare a pressure drop");
    MP_CHECK(!m.wind.at(8000).infiniteVolume,
             "a non-zero pressure drop means the chest is modelled");

    // Rank pitch contract is carried on the per-rank template SampleRef.
    MP_CHECK(m.samples.at(2000).rankBasePitch64ftHarmonicNum == 8,
             "rank sample-assumed footage compiled");

    // Stops get a drawstop switch and land on a division.
    const auto& stop0 = m.stops.at(3000);
    MP_CHECK(stop0.controllingSwitchId == 4000, "stop gets its drawstop switch");
    MP_CHECK(m.switches.count(stop0.controllingSwitchId) == 1,
             "drawstop switch exists in the model");
    MP_CHECK(stop0.divisionId != 0, "stop is attached to a division");
    MP_CHECK(d.stopsWithoutRanks.size() == 2,
             "CODM stops have no StopRank rows until the sample set installs");

    // Coupler and tremulant switches are typed, never orphaned.
    MP_CHECK(m.switches.at(5000).isCoupler, "coupler switch flagged");
    MP_CHECK(m.switches.at(6500).isTremulant, "tremulant switch flagged");
    MP_CHECK(m.keyActions.front().conditionSwitchId == 5000,
             "key action is gated by the coupler switch");

    // Enclosure keeps the raw HW shoe code (mapping verified at M2).
    MP_CHECK(m.enclosures.at(7000).continuousControlId == 211,
             "Encl_EnclosureCode preserved as the continuous-control id");

    // Ids are deterministic: a second load must produce the same model.
    mp::OrganModel m2;
    mp::OdfDiagnostics d2;
    MP_CHECK(loadFixture("example2-equivalent.CustomOrgan_Hauptwerk_xml", m2, d2),
             "second load succeeds");
    MP_CHECK(m2.divisions.count(1000) == 1 && m2.stops.count(3000) == 1 &&
                 m2.enclosures.count(7000) == 1,
             "CODM id allocation is deterministic across loads");
  }
};

class CodmFileFormatTest final : public mp::test::Test {
public:
  CodmFileFormatTest()
    : Test("functional.codm.fileformat", Category::Functional) {}
  void run() override {
    mp::OdfLoader l;
    mp::OdfLoader::Options o;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(!l.loadFromXmlString(
                 "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\"><Foo/></Hauptwerk>",
                 "a.CustomOrgan_Hauptwerk_xml", o, m, d),
             "non-CustomOrgan root must be rejected");
    MP_CHECK(!d.errors.empty(), "rejection must explain itself");
  }
};

class PipeReserveStressTest final : public mp::test::Test {
public:
  PipeReserveStressTest()
    : Test("functional.odf.pipe-reserve-stress", Category::Functional) {}
  void run() override {
    // Nancy-class row counts in miniature: enough pipes/layers/attacks to
    // force vector reallocations. Guards the reserve passes in the loader
    // (dangling pipeById/layerById pointers corrupted the heap on Nancy).
    const int nPipes = 300;
    std::string xml =
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"_General\"><_General>"
        "<Identification_UniqueOrganID>1</Identification_UniqueOrganID>"
        "</_General></ObjectList>"
        "<ObjectList ObjectType=\"Sample\"><Sample>"
        "<SampleID>1</SampleID><SampleFilename>036-C.wav</SampleFilename>"
        "</Sample></ObjectList>"
        "<ObjectList ObjectType=\"Rank\"><Rank><RankID>7</RankID><Name>R</Name></Rank></ObjectList>"
        "<ObjectList ObjectType=\"Pipe_SoundEngine01\">";
    for (int i = 0; i < nPipes; ++i)
      xml += "<Pipe_SoundEngine01><PipeID>" + std::to_string(1000 + i) +
             "</PipeID><RankID>7</RankID><NormalMIDINoteNumber>" +
             std::to_string(36 + (i % 61)) + "</NormalMIDINoteNumber></Pipe_SoundEngine01>";
    xml += "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">";
    for (int i = 0; i < nPipes; ++i)
      xml += "<Pipe_SoundEngine01_Layer><LayerID>" + std::to_string(2000 + i) +
             "</LayerID><PipeID>" + std::to_string(1000 + i) + "</PipeID></Pipe_SoundEngine01_Layer>";
    xml += "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">";
    for (int i = 0; i < nPipes; ++i)
      xml += "<Pipe_SoundEngine01_AttackSample><UniqueID>" + std::to_string(3000 + i) +
             "</UniqueID><LayerID>" + std::to_string(2000 + i) +
             "</LayerID><SampleID>1</SampleID></Pipe_SoundEngine01_AttackSample>";
    xml += "</ObjectList></Hauptwerk>";

    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    MP_CHECK(l.loadFromXmlString(xml, "stress.Organ_Hauptwerk_xml", o, m, d),
             "stress organ must load");
    MP_CHECK(d.errors.empty(), "stress organ must load error-free");
    const auto rankIt = m.ranks.find(7);
    MP_CHECK(rankIt != m.ranks.end(), "rank present");
    MP_CHECK(static_cast<int>(rankIt->second.pipes.size()) == nPipes,
             "all pipes linked");
    const mp::Pipe& mid = rankIt->second.pipes[150];
    MP_CHECK(mid.pipeId == 1150, "pipe identity stable across reallocations");
    MP_CHECK(mid.layers.size() == 1, "layer linked to mid pipe");
    MP_CHECK(mid.layers.front().layerId == 2150, "layer identity stable");
    MP_CHECK(mid.layers.front().attacks.size() == 1, "attack linked to mid layer");
    MP_CHECK(mid.layers.front().attacks.front().sample.fileName == "036-C.wav",
             "attack sample resolved after reallocations");
  }
};

class MatrixSelectionTest final : public mp::test::Test {
public:
  MatrixSelectionTest()
    : Test("functional.matrix.selection", Category::Functional) {}
  void run() override {
    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    MP_CHECK(l.loadFromXmlString(
                 "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                 "<ObjectList ObjectType=\"_General\"><_General>"
                 "<Identification_UniqueOrganID>1</Identification_UniqueOrganID>"
                 "</_General></ObjectList>"
                 "<ObjectList ObjectType=\"Sample\">"
                 "<Sample><SampleID>1</SampleID><SampleFilename>a.wav</SampleFilename></Sample>"
                 "<Sample><SampleID>2</SampleID><SampleFilename>b.wav</SampleFilename></Sample>"
                 "<Sample><SampleID>3</SampleID><SampleFilename>c.wav</SampleFilename></Sample>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"Rank\">"
                 "<Rank><RankID>7</RankID><Name>R</Name></Rank>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
                 "<Pipe_SoundEngine01><PipeID>70</PipeID><RankID>7</RankID>"
                 "<NormalMIDINoteNumber>60</NormalMIDINoteNumber></Pipe_SoundEngine01>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
                 "<Pipe_SoundEngine01_Layer><LayerID>70</LayerID><PipeID>70</PipeID>"
                 "<AudioOut_OptimalChannelFormatCode>2</AudioOut_OptimalChannelFormatCode>"
                 "</Pipe_SoundEngine01_Layer>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
                 "<Pipe_SoundEngine01_AttackSample><UniqueID>71</UniqueID>"
                 "<LayerID>70</LayerID><SampleID>1</SampleID>"
                 "<AttackSelCriteria_HighestVelocity>60</AttackSelCriteria_HighestVelocity>"
                 "</Pipe_SoundEngine01_AttackSample>"
                 "<Pipe_SoundEngine01_AttackSample><UniqueID>72</UniqueID>"
                 "<LayerID>70</LayerID><SampleID>2</SampleID>"
                 "<AttackSelCriteria_HighestVelocity>127</AttackSelCriteria_HighestVelocity>"
                 "<AttackSelCriteria_MinTimeSincePrevPipeCloseMs>50</AttackSelCriteria_MinTimeSincePrevPipeCloseMs>"
                 "<AttackSelCriteria_HighestCtsCtrlValue>100</AttackSelCriteria_HighestCtsCtrlValue>"
                 "</Pipe_SoundEngine01_AttackSample>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"Pipe_SoundEngine01_ReleaseSample\">"
                 "<Pipe_SoundEngine01_ReleaseSample><UniqueID>73</UniqueID>"
                 "<LayerID>70</LayerID><SampleID>3</SampleID>"
                 "<ReleaseSelCriteria_HighestVelocity>127</ReleaseSelCriteria_HighestVelocity>"
                 "</Pipe_SoundEngine01_ReleaseSample>"
                 "<Pipe_SoundEngine01_ReleaseSample><UniqueID>74</UniqueID>"
                 "<LayerID>70</LayerID><SampleID>3</SampleID>"
                 "<ReleaseSelCriteria_HighestVelocity>127</ReleaseSelCriteria_HighestVelocity>"
                 "<ReleaseSelCriteria_PreferThisRelForAttackID>72</ReleaseSelCriteria_PreferThisRelForAttackID>"
                 "</Pipe_SoundEngine01_ReleaseSample>"
                 "<Pipe_SoundEngine01_ReleaseSample><UniqueID>75</UniqueID>"
                 "<LayerID>70</LayerID><SampleID>3</SampleID>"
                 "<ReleaseSelCriteria_HighestVelocity>127</ReleaseSelCriteria_HighestVelocity>"
                 "<ReleaseSelCriteria_PreferThisRelForAttackID>999</ReleaseSelCriteria_PreferThisRelForAttackID>"
                 "</Pipe_SoundEngine01_ReleaseSample>"
                 "</ObjectList></Hauptwerk>",
                 "matrix.Organ_Hauptwerk_xml", o, m, d),
             "matrix organ must load");
    MP_CHECK(d.errors.empty(), "matrix organ loads error-free");
    const mp::PipeLayer& layer = m.ranks.find(7)->second.pipes.front().layers.front();
    MP_CHECK(layer.attacks.size() == 2, "two attacks parsed");
    MP_CHECK(layer.attacks[1].minTimeSinceCloseMs == 50, "min-time parsed");
    MP_CHECK(layer.attacks[1].ctsHigh == 100, "cts ceiling parsed");
    MP_CHECK(layer.optimalChannel == 2, "optimal channel parsed");
    MP_CHECK(layer.releases[1].preferLinkedAttackId == 72, "prefer-link parsed");

    mp::NoteStrike soft{40, 1000, 127};
    mp::NoteStrike loud{100, 1000, 90};
    mp::NoteStrike fast{100, 10, 90}; // retriggered inside min-time window
    mp::NoteStrike loudCts{100, 1000, 120}; // cts above loud attack's ceiling
    MP_CHECK(mp::selectAttack(layer, soft) == 0, "soft velocity takes attack 0");
    MP_CHECK(mp::selectAttack(layer, loud) == 1, "loud velocity takes attack 1");
    MP_CHECK(mp::selectAttack(layer, fast) == -1, "retrigger inside window is silent");
    MP_CHECK(mp::selectAttack(layer, loudCts) == -1, "cts above ceilings is silent");

    mp::NoteRelease playedLoud{1, 72, 100, 90, 100, 200, 90};
    mp::NoteRelease playedSoft{0, 71, 40, 127, 40, 200, 127};
    MP_CHECK(mp::selectRelease(layer, playedLoud) == 1,
             "prefer-linked release wins for its attack");
    MP_CHECK(mp::selectRelease(layer, playedSoft) == 0,
             "unlinked attack falls back to file order");
    MP_CHECK(d.deadReleaseBranches.size() == 1 && d.deadReleaseBranches.front() == 75,
             "dead prefer-link (attack 999) reported");
    MP_CHECK(d.layersUncoveredVelocity.empty(), "ceilings 60+127 cover all velocities");
  }
};

class MatrixCoverageQueryTest final : public mp::test::Test {
public:
  MatrixCoverageQueryTest()
    : Test("functional.matrix.coverage-query", Category::Functional) {}
  void run() override {
    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    MP_CHECK(l.loadFromXmlString(
                 "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                 "<ObjectList ObjectType=\"_General\"><_General>"
                 "<Identification_UniqueOrganID>1</Identification_UniqueOrganID>"
                 "</_General></ObjectList>"
                 "<ObjectList ObjectType=\"Sample\">"
                 "<Sample><SampleID>1</SampleID><SampleFilename>a.wav</SampleFilename></Sample>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"Rank\">"
                 "<Rank><RankID>7</RankID><Name>R</Name></Rank>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
                 "<Pipe_SoundEngine01><PipeID>70</PipeID><RankID>7</RankID>"
                 "<NormalMIDINoteNumber>60</NormalMIDINoteNumber></Pipe_SoundEngine01>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
                 "<Pipe_SoundEngine01_Layer><LayerID>70</LayerID><PipeID>70</PipeID>"
                 "</Pipe_SoundEngine01_Layer>"
                 "</ObjectList>"
                 "<ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
                 "<Pipe_SoundEngine01_AttackSample><UniqueID>71</UniqueID>"
                 "<LayerID>70</LayerID><SampleID>1</SampleID>"
                 "<AttackSelCriteria_HighestVelocity>60</AttackSelCriteria_HighestVelocity>"
                 "</Pipe_SoundEngine01_AttackSample>"
                 "</ObjectList></Hauptwerk>",
                 "gap.Organ_Hauptwerk_xml", o, m, d),
             "gapped organ must load (queries report, never fail)");
    MP_CHECK(d.layersUncoveredVelocity.size() == 1 &&
                 d.layersUncoveredVelocity.front() == 70,
             "velocity gap above 60 reported for layer 70");
  }
};

static RoutingAllocationTest g_routing;
static MatrixSelectionTest g_matrix;
static MatrixCoverageQueryTest g_matrixCoverage;
static PipeReserveStressTest g_reserveStress;
static FixtureDisplayTest g_display;
static SymlinkedOrganTest g_symlinkedOrgan;
static DisplayEmptyTest g_displayEmpty;
static DisplayTextTest g_displayText;
static MidiChannelExclusiveTest g_midiChannelExclusive;
static MidiMapRepairTest g_midiMapRepair;
static BasePitchZeroTest g_basePitchZero;
static SamplePitchRouteTest g_samplePitchRoute;
static SampleFileNameNoteTest g_sampleFileNameNote;
static SamplePitchCodeParsedTest g_samplePitchCodeParsed;
static FixtureCombinationsTest g_combinations;
static CombinationDanglingTest g_combDangling;
static CodmStructuralTest g_codmStruct;
static CodmExample2Test g_codmEx2;
static CodmFileFormatTest g_codmFormat;
static FixtureStopsTest g_stops;
static ResolverTest g_resolver;
// The producer's output trim. Every set on disk declares one and they span a
// far wider range than a spot check suggests: Friesach -13 dB to Lipiny +12,
// with Melcer writing its +12 as "1.2e+1". Ignoring the field leaves a 25 dB
// step between sets that the producers put it there to remove.
class AudioOutTrimTest final : public mp::test::Test {
public:
  AudioOutTrimTest()
    : Test("functional.odf.audio-out-trim", Category::Functional) {}

  static double trimFrom(const char* generalBody) {
    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    const std::string xml =
        std::string("<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                    "<ObjectList ObjectType=\"_General\"><_General>") +
        generalBody + "</_General></ObjectList></Hauptwerk>";
    l.loadFromXmlString(xml.c_str(), "a.Organ_Hauptwerk_xml", o, m, d);
    return m.audioOutputTrimDb;
  }

  void run() override {
    MP_CHECK(trimFrom("<Identification_Name>X</Identification_Name>") == 0.0,
             "a set that declares no trim is not trimmed");
    MP_CHECK(trimFrom("<AudioOut_AmplitudeLevelAdjustDecibels>2"
                      "</AudioOut_AmplitudeLevelAdjustDecibels>") == 2.0,
             "a positive trim is read (Azzio declares +2)");
    MP_CHECK(trimFrom("<AudioOut_AmplitudeLevelAdjustDecibels>-4"
                      "</AudioOut_AmplitudeLevelAdjustDecibels>") == -4.0,
             "a negative trim is read (Raszczyce declares -4)");
    MP_CHECK(trimFrom("<AudioOut_AmplitudeLevelAdjustDecibels>-1.5"
                      "</AudioOut_AmplitudeLevelAdjustDecibels>") == -1.5,
             "a fractional trim keeps its fraction");
    // Melcer writes its +12 dB as "1.2e+1". A parser that stopped at the 'e'
    // would read 1.2 and be quietly 11 dB wrong -- the kind of defect that
    // sounds like a mastering choice rather than a bug.
    MP_CHECK(trimFrom("<AudioOut_AmplitudeLevelAdjustDecibels>1.2e+1"
                      "</AudioOut_AmplitudeLevelAdjustDecibels>") == 12.0,
             "scientific notation is read as the number it is");
  }
};

// The mixer's two roadmap queries. Neither can be answered from the organ
// file: not one set on disk declares an audio-routing object, so what is being
// checked is whether the PLAYER's routing covers the organ they loaded.
// The engine side of the mixer: a voice must render into exactly one bus.
// Both axes are filtered independently, so the test that matters is that they
// AND rather than override each other — a rank in the Swell is enclosed by the
// Swell shades whichever output pair it is sent to.
class MixBusRenderTest final : public mp::test::Test {
public:
  MixBusRenderTest()
    : Test("functional.mixer.bus-render", Category::Functional) {}

  // Four voices across two enclosures and two mixer buses, one per corner, so
  // every combination is distinguishable.
  struct Corner { int enclosure, mix; };

  // A FRESH engine per measurement. Rendering advances every voice's cursor,
  // so two renders of "the same" thing read different parts of the sample and
  // any comparison between them is meaningless.
  static double energy(voicetest::Fixture& f, int enclosure, int mix) {
    static const Corner corners[] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
    mp::VoiceEngine e;
    e.prepare(48000.0, 64, 1);
    e.setSampleProvider(f.provider());
    uint64_t noteId = 1;
    for (const auto& c : corners) {
      mp::VoiceStart s;
      s.pipe = &f.pipe;
      s.layer = &f.pipe.layers[0];
      s.busIndex = c.enclosure;
      s.mixBus = c.mix;
      e.startVoice(s, noteId++);
    }

    const int frames = 64;
    std::vector<float> buf(static_cast<size_t>(frames), 0.0f);
    float* planes[1] = {buf.data()};
    e.beginBlock();
    e.render(planes, 1, frames, enclosure, mix);
    double sum = 0.0;
    for (float v : buf) sum += std::abs(v);
    return sum;
  }

  void run() override {
    voicetest::Fixture f;
    static const Corner corners[] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};

    // Each corner on its own carries signal ...
    const double one = energy(f, 0, 0);
    MP_CHECK(one > 0.0, "one enclosure and one mix bus renders something");
    for (const auto& c : corners)
      MP_CHECK(energy(f, c.enclosure, c.mix) > 0.0,
               "every corner renders on its own filter pair");

    // ... and the two filters AND rather than either one winning. If mixBus
    // were ignored, asking for enclosure 0 would return both of its voices —
    // which is exactly what a routing axis that silently does nothing looks
    // like.
    const double bothOfEnclosure0 = energy(f, 0, -1);
    MP_CHECK(bothOfEnclosure0 > one * 1.5,
             "enclosure 0 unfiltered by mix bus carries both its voices");

    // The four corners must partition the whole: no voice rendered twice,
    // none missed.
    double parts = 0.0;
    for (const auto& c : corners) parts += energy(f, c.enclosure, c.mix);
    const double whole = energy(f, -1, -1);
    MP_CHECK(std::abs(parts - whole) < whole * 1e-6,
             "the corners sum to the unfiltered render");

    // A bus nobody is on is silent rather than a fallback to everything — the
    // mistake that would make routing look like it works while every voice
    // played out of every output.
    MP_CHECK(energy(f, 0, 7) == 0.0, "an unused mix bus renders silence");
  }
};

// Voicing. The two levels ADD, which is the decision worth pinning: if a pipe
// override replaced its rank's trim, retuning one sour note would silently
// throw away "this whole Mixture is 3 dB too loud".
class VoicingTest final : public mp::test::Test {
public:
  VoicingTest() : Test("functional.voicing.set", Category::Functional) {}

  void run() override {
    mp::VoicingSet v;
    MP_CHECK(v.empty(), "an unvoiced organ is empty, so note-on skips the lookup");

    mp::PipeVoicing rank;
    rank.gainDb = -3.0f;
    v.setRank(201, rank);
    mp::PipeVoicing one;
    one.tuningCents = -8.0f;
    v.setPipe(301, one);
    MP_CHECK(!v.empty(), "and stops being empty once something is set");

    // --- the two levels add ----------------------------------------------
    const auto eff = v.effective(201, 301);
    MP_CHECK(eff.gainDb == -3.0f, "the rank trim reaches the pipe");
    MP_CHECK(eff.tuningCents == -8.0f, "as does the pipe's own tuning");

    mp::PipeVoicing both;
    both.gainDb = -2.0f;
    v.setPipe(302, both);
    MP_CHECK(v.effective(201, 302).gainDb == -5.0f,
             "a pipe adjustment ADDS to its rank's, it does not replace it");

    // A pipe in a rank with no trim is just itself.
    MP_CHECK(v.effective(999, 302).gainDb == -2.0f,
             "a pipe under an unvoiced rank carries only its own");
    MP_CHECK(v.effective(999, 999).isNeutral(),
             "and an untouched pipe under an untouched rank is neutral");

    // --- putting it back removes it, rather than storing a zero -----------
    // Otherwise a file fills with zero rows and a forgotten experiment looks
    // exactly like a deliberate neutral setting.
    v.setPipe(301, mp::PipeVoicing{});
    MP_CHECK(v.pipes().count(301) == 0, "a neutral setting erases its entry");
    MP_CHECK(v.effective(201, 301).tuningCents == 0.0f, "and stops applying");

    // --- cents are a ratio -----------------------------------------------
    MP_CHECK(mp::centsRatio(0.0) == 1.0, "no detune is exactly unity");
    MP_CHECK(std::abs(mp::centsRatio(1200.0) - 2.0) < 1e-12,
             "an octave up doubles the ratio");
    MP_CHECK(std::abs(mp::centsRatio(-1200.0) - 0.5) < 1e-12,
             "an octave down halves it");
    // The number a player actually types. 10 cents flat is a slow beat, not a
    // wrong note, and the ratio has to be close to 1 for that to be true.
    MP_CHECK(std::abs(mp::centsRatio(-10.0) - 0.9942404) < 1e-6,
             "ten cents flat is 0.9942404");

    // --- A/B ---------------------------------------------------------------
    mp::VoicingAB ab;
    mp::PipeVoicing loud;
    loud.gainDb = 6.0f;
    ab.live().setRank(201, loud);
    MP_CHECK(ab.live().rank(201).gainDb == 6.0f, "A holds what was set");
    ab.swap();
    MP_CHECK(ab.live().empty(), "B starts empty, so the swap is audible");
    ab.swap();
    MP_CHECK(ab.live().rank(201).gainDb == 6.0f, "and swapping back restores A");

    // Copying across is what makes B a variation rather than a comparison
    // against silence.
    ab.copyToOther();
    ab.swap();
    MP_CHECK(ab.live().rank(201).gainDb == 6.0f,
             "the other slot starts from this one");
    mp::PipeVoicing louder;
    louder.gainDb = 9.0f;
    ab.live().setRank(201, louder);
    ab.swap();
    MP_CHECK(ab.live().rank(201).gainDb == 6.0f,
             "and editing B leaves A alone");
  }
};

// Favourites. A slot number is something a thumb piston can be mapped to; a
// path is not. So the numbering is the feature, and the tests are about the
// numbering holding still.
class FavouritesTest final : public mp::test::Test {
public:
  FavouritesTest() : Test("functional.favourites.bank", Category::Functional) {}

  void run() override {
    mp::FavouriteBank bank;
    MP_CHECK(bank.count() == 0, "a new bank is empty");
    MP_CHECK(bank.firstFree() == 1, "and the first free slot is 1, not 0");
    MP_CHECK(bank.at(1).empty(), "an unset slot reads as empty");

    // --- bounds are answered, never crashed through -----------------------
    MP_CHECK(!bank.valid(0) && !bank.valid(65), "slots run 1..64");
    MP_CHECK(bank.at(0).empty() && bank.at(65).empty() && bank.at(-3).empty(),
             "an out-of-range slot reads empty rather than reading memory");
    bank.set(0, {"nowhere", "x"});
    bank.set(99, {"nowhere", "x"});
    MP_CHECK(bank.count() == 0, "and writing out of range stores nothing");

    // --- gaps are part of the layout --------------------------------------
    // Renumbering to close a gap would move every slot a player had learned,
    // which is the one thing a numbered bank must never do.
    bank.set(1, {"Raszczyce", "D:/organs/Raszczyce/x.xml"});
    bank.set(5, {"Cracow", "D:/organs/Cracow/y.xml"});
    MP_CHECK(bank.count() == 2, "two slots used");
    MP_CHECK((bank.used() == std::vector<int>{1, 5}), "reported in slot order");
    MP_CHECK(bank.firstFree() == 2, "the first free slot is the gap");
    bank.clear(1);
    MP_CHECK(bank.at(5).name == "Cracow",
             "clearing slot 1 does not move slot 5");
    MP_CHECK(bank.firstFree() == 1, "and frees slot 1 for reuse");

    // --- the same thing twice is the same thing ---------------------------
    MP_CHECK(bank.slotOf("D:/organs/Cracow/y.xml") == 5,
             "an entry is found by its target");
    MP_CHECK(bank.slotOf("D:/organs/nothing.xml") == 0,
             "and an absent one reports 0, which is not a slot");
    bank.set(9, {"A different name", "D:/organs/Cracow/y.xml"});
    MP_CHECK(bank.slotOf("D:/organs/Cracow/y.xml") == 5,
             "a duplicate target still resolves to the FIRST slot holding it");

    // --- full ---------------------------------------------------------------
    mp::FavouriteBank full;
    for (int i = 1; i <= mp::kFavouriteSlots; ++i)
      full.set(i, {"x", "t" + std::to_string(i)});
    MP_CHECK(full.count() == mp::kFavouriteSlots, "64 slots fill");
    MP_CHECK(full.firstFree() == 0,
             "a full bank reports 0 rather than overwriting slot 1");

    // --- the three banks are separate -------------------------------------
    mp::Favourites f;
    f.bank(mp::FavouriteKind::Organ).set(1, {"organ", "o"});
    f.bank(mp::FavouriteKind::Temperament).set(1, {"temp", "t"});
    MP_CHECK(f.organs.at(1).name == "organ" && f.temperaments.at(1).name == "temp",
             "slot 1 of one bank is not slot 1 of another");
    MP_CHECK(f.combinationSets.at(1).empty(), "and the third is untouched");

    // A key from a newer build must cost one favourite, not the whole file.
    MP_CHECK(mp::Favourites::kindFromKey("temperament") ==
                 mp::FavouriteKind::Temperament, "known keys round-trip");
    MP_CHECK(mp::Favourites::kindFromKey("something-new") ==
                 mp::FavouriteKind::Organ, "an unknown key falls back");
  }
};

class MixerRoutingTest final : public mp::test::Test {
public:
  MixerRoutingTest()
    : Test("functional.mixer.routing", Category::Functional) {}

  static mp::OrganModel threeRanks() {
    mp::OrganModel m;
    for (mp::Id id : {201u, 202u, 203u}) {
      mp::Rank r;
      r.rankId = id;
      m.ranks[id] = r;
    }
    return m;
  }

  void run() override {
    const auto model = threeRanks();

    // --- an unconfigured organ is audible ---------------------------------
    // The point of falling back to simple routing. A rank nobody has routed
    // yet must still speak, or a fresh organ is indistinguishable from a
    // broken engine.
    {
      auto c = mp::MixerConfig::stereoDefault();
      const auto r = c.routingFor(201);
      MP_CHECK(std::holds_alternative<mp::BusId>(r.perspectives[0].dest),
               "an unrouted rank falls back to a direct bus");
      MP_CHECK(std::get<mp::BusId>(r.perspectives[0].dest) == mp::BusId{1},
               "and that bus is the first one");
      const auto d = mp::validateMixer(model, c);
      MP_CHECK(d.unroutedRanks.empty(), "so nothing reports as unrouted");
      MP_CHECK(d.busesWithoutOutput.empty(),
               "and the default bus has device channels");
      MP_CHECK(d.clean(), "a default config is clean");
    }

    // --- bus-unrouted-rank ------------------------------------------------
    {
      auto c = mp::MixerConfig::stereoDefault();
      mp::RankRouting silent;
      silent.rankId = 202;
      silent.perspectives[0].dest = mp::BusId{0}; // deliberately nowhere
      c.rankRoutings[202] = silent;

      const auto d = mp::validateMixer(model, c);
      MP_CHECK(d.unroutedRanks.size() == 1 && d.unroutedRanks[0] == 202,
               "a rank routed to no bus is reported, and only that rank");
      MP_CHECK(!d.clean(), "and the config is not clean");
    }

    // An empty group is the same silence by a different route, and the more
    // likely one: the player made a group and never filled it.
    {
      auto c = mp::MixerConfig::stereoDefault();
      c.groups.push_back(mp::BusGroup{5, {}});
      mp::RankRouting grouped;
      grouped.rankId = 203;
      grouped.perspectives[0].dest = 5;
      c.rankRoutings[203] = grouped;
      MP_CHECK(mp::validateMixer(model, c).unroutedRanks ==
                   std::vector<mp::Id>{203},
               "a rank routed to an empty group is unrouted");
    }

    // --- a stale reference is not the same as no reference ----------------
    // A bus deleted under a routing. Falling back silently would hide the
    // deletion, which is how a rank goes quiet for a reason nobody can see.
    {
      auto c = mp::MixerConfig::stereoDefault();
      mp::RankRouting stale;
      stale.rankId = 201;
      stale.perspectives[0].dest = mp::BusId{97}; // never defined
      c.rankRoutings[201] = stale;

      const auto d = mp::validateMixer(model, c);
      MP_CHECK(d.ranksRoutedToMissingBus == std::vector<mp::Id>{201},
               "a routing to a missing bus is reported as stale");
      MP_CHECK(d.unroutedRanks.empty(),
               "and NOT as unrouted -- they are different faults");
    }

    // --- a bus that reaches no device -------------------------------------
    {
      auto c = mp::MixerConfig::stereoDefault();
      mp::MixerBus orphan;
      orphan.id = mp::BusId{9};
      c.buses.push_back(orphan); // no device channels
      const auto d = mp::validateMixer(model, c);
      MP_CHECK(d.busesWithoutOutput == std::vector<int>{9},
               "a bus with no output is reported");
    }

    // --- reports are stable -----------------------------------------------
    // Ranks live in an unordered_map, whose order is not stable across builds.
    // A diagnostic that reshuffles between runs cannot be diffed.
    {
      auto c = mp::MixerConfig::stereoDefault();
      for (mp::Id id : {201u, 202u, 203u}) {
        mp::RankRouting r;
        r.rankId = id;
        r.perspectives[0].dest = mp::BusId{0};
        c.rankRoutings[id] = r;
      }
      const auto d = mp::validateMixer(model, c);
      MP_CHECK(d.unroutedRanks == (std::vector<mp::Id>{201, 202, 203}),
               "unrouted ranks come back in id order");
    }

    // --- allocation still spreads a chord ---------------------------------
    // Why groups exist: adjacent keys must not pile onto one bus, or a chord
    // loses the separation the multiple outputs were for.
    {
      mp::BusGroup g{5, {mp::BusId{1}, mp::BusId{2}, mp::BusId{3}}};
      const auto a = mp::allocateBus(g, 60, 201, mp::AllocationAlgorithm::StaticChromatic, 0);
      const auto b = mp::allocateBus(g, 61, 201, mp::AllocationAlgorithm::StaticChromatic, 0);
      MP_CHECK(a != b, "adjacent keys land on different buses");
      MP_CHECK(mp::allocateBus(g, 60, 201, mp::AllocationAlgorithm::StaticChromatic, 0) == a,
               "and the same key always lands on the same one");
      MP_CHECK(mp::allocateBus(mp::BusGroup{6, {}}, 60, 201,
                               mp::AllocationAlgorithm::StaticChromatic, 0) == mp::BusId{0},
               "an empty group allocates nothing rather than crashing");
    }
  }
};

class MissingGeneralTest final : public mp::test::Test {
public:
  MissingGeneralTest()
    : Test("functional.odf.missing-general", Category::Functional) {}
  void run() override {
    mp::OdfLoader l;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    mp::OdfLoader::Options o;
    const bool ok = l.loadFromXmlString(
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"Rank\"><Rank><RankID>1</RankID><Name>X</Name></Rank></ObjectList>"
        "</Hauptwerk>",
        "a.Organ_Hauptwerk_xml", o, m, d);
    MP_CHECK(!ok, "full ODF without _General must fail (M1.1 required table)");
    bool found = false;
    for (const auto& e : d.errors)
      if (e.find("_General") != std::string::npos) found = true;
    MP_CHECK(found, "error must name the missing _General table");
  }
};

class FixturePipeworkTest final : public mp::test::Test {
public:
  FixturePipeworkTest()
    : Test("functional.fixtures.pipework", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(loadFixture("minimal.Organ_Hauptwerk_xml", m, d),
             "minimal fixture must load");
    MP_CHECK(d.errors.empty(), "minimal fixture must load error-free");
    MP_CHECK(m.organName == "Masterpiece Test Church", "organ name parsed");
    MP_CHECK(m.uniqueOrganId == 90001, "unique organ ID parsed");
    MP_CHECK(m.basePitchHz > 439.9 && m.basePitchHz < 440.1, "base pitch parsed");
    MP_CHECK(m.samples.size() == 2, "two samples registered");
    MP_CHECK(m.ranks.size() == 1, "one rank registered");
    const auto rankIt = m.ranks.find(201);
    MP_CHECK(rankIt != m.ranks.end(), "rank 201 present");
    MP_CHECK(rankIt->second.name == "Test Principal 8", "rank name parsed");
    MP_CHECK(rankIt->second.pipes.size() == 1, "one pipe linked into rank");
    const mp::Pipe& pipe = rankIt->second.pipes.front();
    MP_CHECK(pipe.pipeId == 301, "pipe ID parsed");
    MP_CHECK(pipe.midiNote == 36, "pipe MIDI note parsed");
    MP_CHECK(pipe.layers.size() == 1, "one layer linked into pipe");
    const mp::PipeLayer& layer = pipe.layers.front();
    MP_CHECK(layer.layerId == 401, "layer ID parsed");
    MP_CHECK(layer.attacks.size() == 1, "one attack linked into layer");
    MP_CHECK(layer.attacks.front().sample.fileName == "001-C.wav",
             "attack sample resolved to registry filename");
    MP_CHECK(layer.attacks.front().velHigh == 127, "attack velocity ceiling parsed");
    MP_CHECK(layer.releases.size() == 1, "one release linked into layer");
    MP_CHECK(layer.releases.front().sample.fileName == "001-C_Trem.wav",
             "release sample resolved to registry filename");
  }
};

// Choosing stops on an organ nobody wrote a preset for.
//
// The names here are not invented: they are the vocabulary actually observed
// across the libraries on hand -- Dutch (Prestant/Holpijp/Mixtuur), French
// (Montre/Bourdon/Plein Jeu), German (Principal/Gedackt/Krummhorn). A
// classifier that only works in the language it was written in does not work.
class RegistrationTest final : public mp::test::Test {
public:
  RegistrationTest() : Test("functional.registration.choose", Category::Functional) {}

  // divisionId 1 = Great, 2 = Swell, 3 = Pedal.
  static void addStop(mp::OrganModel& m, mp::Id id, mp::Id div, const char* name) {
    mp::Stop s;
    s.stopId = id;
    s.divisionId = div;
    s.name = name;
    m.stops[id] = s;
  }

  void run() override {
    using mp::StopFamily;

    // --- the same rank in four languages ----------------------------------
    MP_CHECK(mp::classifyStop("Prestant 8") == StopFamily::Principal, "Dutch principal");
    MP_CHECK(mp::classifyStop("Montre 8") == StopFamily::Principal, "French principal");
    MP_CHECK(mp::classifyStop("Principal 8") == StopFamily::Principal, "German principal");
    MP_CHECK(mp::classifyStop("Open Diapason 8") == StopFamily::Principal, "English principal");
    MP_CHECK(mp::classifyStop("Octaaf 4") == StopFamily::Principal, "an octave is principal-scaled");

    MP_CHECK(mp::classifyStop("Holpijp 8") == StopFamily::Flute, "Dutch stopped flute");
    MP_CHECK(mp::classifyStop("Bourdon 16") == StopFamily::Flute, "French stopped flute");
    MP_CHECK(mp::classifyStop("Gedackt 8") == StopFamily::Flute, "German stopped flute");

    MP_CHECK(mp::classifyStop("Mixtuur IV") == StopFamily::Mixture, "Dutch mixture");
    MP_CHECK(mp::classifyStop("Plein Jeu V") == StopFamily::Mixture, "French mixture");
    MP_CHECK(mp::classifyStop("Scherp III") == StopFamily::Mixture, "and a sharp mixture");

    MP_CHECK(mp::classifyStop("Trompet 8") == StopFamily::Reed, "Dutch reed");
    MP_CHECK(mp::classifyStop("Krummhorn 8") == StopFamily::Reed, "German reed");
    MP_CHECK(mp::classifyStop("Hautbois 8") == StopFamily::Reed, "French reed");
    MP_CHECK(mp::classifyStop("Gamba 8") == StopFamily::String, "a string");

    // --- the ones that look like something they are not -------------------
    // A Cornet is a compound stop, not a reed, however much it sounds like
    // one; a Quintaton is a flute, not a quint.
    MP_CHECK(mp::classifyStop("Cornet V") == StopFamily::Mixture, "a cornet is compound");
    MP_CHECK(mp::classifyStop("Quintaton 16") == StopFamily::Flute, "a quintaton is a flute");
    // A tremulant is a switch that must never be counted as a voice --
    // including in Tutti, which draws literally everything else.
    MP_CHECK(mp::classifyStop("Tremulant") == StopFamily::Effect, "a tremulant is not a voice");
    MP_CHECK(mp::classifyStop("Zimbelstern") == StopFamily::Effect, "nor is a cymbal star");

    // --- accents fold, because builders write them and lists often do not --
    MP_CHECK(mp::classifyStop("Rohrfl\xc3\xb6te 8") == StopFamily::Flute, "accented name");
    MP_CHECK(mp::classifyStop("Rohrfloete 8") == StopFamily::Flute, "and its transliteration");

    // --- pitch ------------------------------------------------------------
    MP_CHECK(mp::stopFootage("Prestant 8") == 8.0, "plain footage");
    MP_CHECK(mp::stopFootage("Subbas 16") == 16.0, "two digits are not read as one");
    MP_CHECK(std::abs(mp::stopFootage("Nasard 2 2/3") - 2.667) < 0.01,
             "a compound pitch is not read as its leading digit");
    MP_CHECK(std::abs(mp::stopFootage("Larigot 1 1/3") - 1.333) < 0.01, "and a larigot");
    MP_CHECK(mp::stopFootage("Mixtuur IV") == 0.0, "a mixture states no pitch");

    // --- a whole organ ----------------------------------------------------
    mp::OrganModel m;
    addStop(m, 10, 1, "Prestant 8");
    addStop(m, 11, 1, "Holpijp 8");
    addStop(m, 12, 1, "Octaaf 4");
    addStop(m, 13, 1, "Octaaf 2");
    addStop(m, 14, 1, "Mixtuur IV");
    addStop(m, 15, 1, "Trompet 8");
    addStop(m, 20, 2, "Gedackt 8");
    addStop(m, 21, 2, "Fluit 4");
    addStop(m, 30, 3, "Subbas 16");
    addStop(m, 31, 3, "Octaafbas 8");
    addStop(m, 40, 1, "Tremulant");

    const auto stops = mp::classifyStops(m);
    MP_CHECK(stops.size() == 11, "every stop is classified, tremulant included");
    // The pedal is found by pitch, not by name: division names are absent
    // here and in several real libraries, and are in the local language when
    // they are present at all.
    MP_CHECK(mp::guessPedalDivision(stops) == 3, "the pedal is the division built lowest");

    auto has = [](const std::vector<mp::Id>& v, mp::Id id) {
      return std::find(v.begin(), v.end(), id) != v.end();
    };

    const auto tutti = mp::chooseRegistration(m, mp::Registration::Tutti);
    MP_CHECK(tutti.size() == 10, "tutti draws everything except the tremulant");
    MP_CHECK(!has(tutti, 40), "and specifically not the tremulant");

    const auto plenum = mp::chooseRegistration(m, mp::Registration::Plenum);
    MP_CHECK(has(plenum, 10) && has(plenum, 12) && has(plenum, 13) && has(plenum, 14),
             "the plenum is the principal chorus 8-4-2 crowned by the mixture");
    MP_CHECK(!has(plenum, 15), "a chorus reed is not part of this plenum");
    MP_CHECK(has(plenum, 30), "with a 16 foot pedal under it");

    const auto chorale = mp::chooseRegistration(m, mp::Registration::Chorale);
    MP_CHECK(chorale.size() == 2, "a chorale prelude is one flute and one pedal stop");
    MP_CHECK(has(chorale, 11) && has(chorale, 30), "the 8 foot flute and the 16 foot pedal");

    const auto trio = mp::chooseRegistration(m, mp::Registration::Trio);
    MP_CHECK(has(trio, 10) && has(trio, 20),
             "a trio contrasts a principal on one manual against a flute on the other");

    const auto solo = mp::chooseRegistration(m, mp::Registration::Solo);
    MP_CHECK(has(solo, 15), "the solo voice is the reed when the organ has one");
    MP_CHECK(has(solo, 20), "accompanied from the second manual");

    // --- an organ that gives the recipes nothing to work with -------------
    // One manual, no reed, no mixture, no pedal. Every recipe must still
    // produce a playable registration: a silent organ and a broken one look
    // exactly alike from the console.
    mp::OrganModel tiny;
    addStop(tiny, 1, 1, "Gedackt 8");
    addStop(tiny, 2, 1, "Tremulant");
    MP_CHECK(mp::guessPedalDivision(mp::classifyStops(tiny)) == 0,
             "an organ with no low division has no pedal to find");
    for (auto style : {mp::Registration::Tutti, mp::Registration::Plenum,
                       mp::Registration::Chorale, mp::Registration::Trio,
                       mp::Registration::Solo}) {
      const auto r = mp::chooseRegistration(tiny, style);
      MP_CHECK(!r.empty(), std::string("a one-stop organ still sounds in ") +
                               mp::registrationName(style));
      MP_CHECK(!has(r, 2), std::string("and never draws the tremulant in ") +
                               mp::registrationName(style));
    }

    // --- the organ that has no 8 foot flute --------------------------------
    // Observed on two of the libraries on hand. Choosing by family first put a
    // Nasard 2 2/3 alone on one and a 4 foot flute alone on the other -- one
    // sour, one an octave high -- when both had an 8 foot principal sitting
    // unused. Pitch is the requirement; family is only the preference.
    MP_CHECK(mp::isMutation(2.667) && mp::isMutation(1.333),
             "a fifth-sounding rank is a mutation");
    MP_CHECK(!mp::isMutation(8.0) && !mp::isMutation(4.0) && !mp::isMutation(16.0),
             "unisons and octaves are not");

    mp::OrganModel odd;
    addStop(odd, 1, 1, "Principal 8");
    addStop(odd, 2, 1, "Nasard 2 2/3");
    addStop(odd, 3, 1, "Flute 4");
    addStop(odd, 4, 3, "Subbass 16");
    const auto ch = mp::chooseRegistration(odd, mp::Registration::Chorale);
    MP_CHECK(has(ch, 1), "with no 8 foot flute, the 8 foot principal carries it");
    MP_CHECK(!has(ch, 2), "and never the mutation");
    MP_CHECK(!has(ch, 3), "nor the 4 foot, which would sound an octave high");

    // --- a stop is never drawn twice --------------------------------------
    // pick() skips what is already chosen; without that, an organ whose only
    // principal is 8 foot would have it selected as the 8, the 4 and the 2.
    mp::OrganModel thin;
    addStop(thin, 1, 1, "Principal 8");
    addStop(thin, 2, 3, "Subbas 16");
    const auto p = mp::chooseRegistration(thin, mp::Registration::Plenum);
    std::vector<mp::Id> uniq = p;
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    MP_CHECK(uniq.size() == p.size(), "no stop appears twice in a registration");
  }
};

static DetectTypeTest g_detect;
static LoaderRejectsUnknownTest g_rejectUnknown;
static LoaderToleranceTest g_tolerance;
static LoaderEmptyTableTest g_emptyTable;
static PalletSwitchTest g_palletSwitch;
#ifdef MP_TEST_HAS_AUDIO
static BmpImageTest g_bmpImage;
#endif
static ConditionSenseTest g_conditionSense;
static EncryptedDetectionTest g_encrypted;
static FixtureCorpusTest g_fixtures;
static CodmCodesTest g_codm;
static CodmDefaultsTest g_codmDefaults;
static VoiceEngineRenderTest g_voiceRender;
static VoiceStopWhileHeldTest g_voiceStopWhileHeld;
static VoiceEngineLifecycleTest g_voiceLifecycle;
static VoiceReleaseTailTest g_voiceReleaseTail;
static VoiceReleaseCrossfadeTest g_voiceReleaseCrossfade;
static VoiceEngineStealingTest g_voiceStealing;
static VoiceRetriggerTest g_voiceRetrigger;
static LoopCrossfadeTest g_loopCrossfade;
static DetuningTest g_detuning;
static RankPitchRatioTest g_rankPitch;
static TremulantVoiceTest g_tremVoice;
static StreamingTest g_streaming;
static CompactStorageTest g_compactStorage;
static VoiceEngineInterpolationTest g_voiceInterp;
static VoiceEngineThreadingTest g_voiceThreading;
static VoiceEngineBusTest g_voiceBuses;
static VoiceEnginePitchTest g_voicePitch;
static TuningTableTest g_tuningTable;
static KeyboardLayoutTest g_keyboardLayout;
static WindSolverTest g_windSolver;
static StepperTest g_stepper;
static DoubleLinkageTest g_doubleLinkage;
static StageSwitchTest g_stageSwitches;
static CombinationTest g_combinationPistons;
static SwitchNetworkTest g_switchNetwork;
static CouplerGraphTest g_couplerGraph;
static DefaultKeyboardTest g_defaultKeyboard;
static KeyboardBindingTest g_keyboardBinding;
static LcdPanelTest g_lcdPanels;
static MidiMapTest g_midiMap;
static ContinuousControlBankTest g_ccBank;
static ExpressionTablesTest g_expressionTables;
static NoiseOptOutTest g_noiseOptOut;
static TemperamentRatioTest g_tempRatio;
static TemperamentLibraryTest g_tempLibrary;
// Declared far above, next to the fixtures they exercise. FixturePipework
// and MissingGeneral were written and never instantiated, so neither had
// ever run.
static AudioOutTrimTest g_audioOutTrim;
static MixerRoutingTest g_mixerRouting;
static VoicingTest g_voicingSet;
static FavouritesTest g_favourites;
static RegistrationTest g_registration;
static MixBusRenderTest g_mixBusRender;
static FixturePipeworkTest g_fixturePipework;
static MissingGeneralTest g_missingGeneral;
#ifdef MP_TEST_HAS_SAMPLER
static DiskTierClassifyTest g_diskTier;
static SampleHandleTest g_sampleHandle;
#endif
#ifdef MP_TEST_HAS_AUDIO
static AudioRecorderTest g_audioRecorder;
static MasterGainPersistenceTest g_masterGainPersistence;
#endif
#ifdef MP_TEST_HAS_DSP
#ifdef MP_TEST_HAS_AUDIO
// The memory settings a fresh installation starts with, and whether the one
// that costs audible quality survives being saved and read back.
//
// Stereo is the default and is meant to stay that way. Mono halves the
// footprint by discarding the recording's stereo image, which is the sort of
// regression nobody reports as a bug: the organ still loads and still plays,
// it is just flat. The same goes for the resident format -- 32-bit float is
// what the samples decode to, and anything else is a decision.
class MemoryDefaultsTest final : public mp::test::Test {
public:
  MemoryDefaultsTest()
    : Test("functional.samples.memory-defaults", Category::Functional) {}

  void run() override {
    mp::SampleLibrary lib;

    // What a fresh installation gets, before any settings file exists.
    MP_CHECK(!lib.loadMono(), "stereo by default");
    MP_CHECK(lib.storage() == mp::SampleStorage::Int24,
             "24-bit by default, which is what the sample sets are");
    MP_CHECK(!lib.streamReleases(), "release tails held by default");

    // Sample data is kept at whatever rate it was recorded at unless asked
    // otherwise. Converting is a real conversion -- nothing above the
    // target's Nyquist survives it -- so it is never the thing that happens
    // because nobody chose.
    MP_CHECK(lib.loadSampleRate() == 0.0, "recorded rate kept by default");
    lib.setLoadSampleRate(48000.0);
    MP_CHECK(lib.loadSampleRate() == 48000.0, "a rate can be asked for");
    lib.setLoadSampleRate(-1.0);
    MP_CHECK(lib.loadSampleRate() == 0.0, "a nonsense rate means 'as recorded'");
    lib.setLoadSampleRate(0.0);

    // And that the channel fold is a real setting rather than one direction
    // only: it was writable long before anything persisted it.
    lib.setLoadMono(true);
    MP_CHECK(lib.loadMono(), "mono can be turned on");
    lib.setLoadMono(false);
    MP_CHECK(!lib.loadMono(), "and back off again");

    // 24-bit residency is only worth defaulting to if it is exact. The
    // loader multiplies the decoded float by 8388608 and rounds, which
    // recovers a 24-bit source integer precisely; this checks the storage
    // type carries it back unchanged, rails included.
    for (int v : {-8388608, -8388607, -65536, -257, -1, 0, 1, 257, 65536,
                  4194304, 8388606, 8388607}) {
      mp::Pcm24 p{};
      p.b[0] = static_cast<unsigned char>(v & 0xFF);
      p.b[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
      p.b[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
      MP_CHECK(static_cast<float>(p) == static_cast<float>(v),
               "24-bit storage round-trips exactly");
    }

    // The enum has exactly the widths that survived measurement. An
    // 8-bit member was built and withdrawn -- it held about 32 dB of signal
    // to quantisation noise against 16-bit's 78 -- so this pins the list
    // rather than letting it quietly grow a third member again.
    MP_CHECK(static_cast<int>(mp::SampleStorage::Float32) == 0 &&
                 static_cast<int>(mp::SampleStorage::Int24) == 1 &&
                 static_cast<int>(mp::SampleStorage::Int16) == 2,
             "three resident formats, in the order the tests assume");
  }
};
#endif

#ifdef MP_TEST_HAS_AUDIO
static SampleLibraryTest g_sampleLibrary;
static MemoryDefaultsTest g_memoryDefaults;
static RecentOrgansTest g_recentOrgans;
static ArchiveFilteringTest g_archiveFiltering;
static OrganNameParsingTest g_organNameParsing;
static UnrarDiscoveryTest g_unrarDiscovery;
static OrganEtaTest g_organEta;
static OrganDiskSpaceFormatTest g_organDiskSpace;
static OrganDetailsAndPackagesTest g_organDetails;
static OrganHidingTest g_organHiding;
static OrganOfflineAudioSettingsTest g_organOfflineAudio;
static OrganRamEstimationTest g_organRamEstimation;
static OrganAsyncStatScanTest g_organAsyncStatScan;
#endif
static DspFastPathTest g_dspFastPath;
static EnclosureResponseTest g_encResponse;
static EnclosureModulationTest g_encModulation;
static TremulantRampTest g_tremRamp;
#endif
static OdfScanThroughputTest g_odfScan;
static VoicePolyphonyThreadedPerfTest g_voicePerfThreaded;
static CompactStoragePerfTest g_compactPerf;
static VoicePolyphonyPerfTest g_voicePerf;
static TemperamentThroughputTest g_tempThroughput;

int main(int argc, char** argv) {
  std::optional<mp::test::Category> filter;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--perf-only")
      filter = mp::test::Category::Perf;
    else if (arg == "--no-perf")
      filter = mp::test::Category::Functional;
  }
  return mp::test::runAll(filter) == 0 ? 0 : 1;
}
