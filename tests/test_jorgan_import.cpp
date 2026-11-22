// jOrgan organs: a disposition and its SoundFont, converted and loaded.
//
// The SoundFont is built here: two samples, a preset that splits the keys
// between them, its tremulant twin forty programs below, and a percussive
// preset without a loop. The disposition has a pedal and a manual, a unit
// stop played at two pitches, a coupler, a swell filter that sends the
// expression controller, and a tremulant filter that switches its ranks'
// program as the Freedom theatre organ's do.
#include "MpTest.h"

#include "../src/mp_core/GrandOrgueImport.h"
#include "../src/mp_core/OdfLoader.h"
#include "../src/mp_jorgan/JOrganDisposition.h"
#include "../src/mp_jorgan/JOrganImport.h"
#include "../src/mp_jorgan/SoundFont2.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using mp::test::Category;
namespace fs = std::filesystem;

namespace {

std::vector<int16_t> sine(int frames, double hz, double rate = 44100.0) {
  std::vector<int16_t> out(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i)
    out[static_cast<size_t>(i)] = static_cast<int16_t>(12000.0 * std::sin(2.0 * 3.141592653589793 * hz * i / rate));
  return out;
}

std::string fileBytes(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

uint32_t le32(const std::string& s, size_t at) {
  return static_cast<uint32_t>(static_cast<unsigned char>(s[at])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(s[at + 1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(s[at + 2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(s[at + 3])) << 24);
}

// The offset of a RIFF chunk's body in a WAV file, or 0.
size_t chunkAt(const std::string& wav, const char* id) {
  for (size_t at = 12; at + 8 <= wav.size();) {
    const uint32_t size = le32(wav, at + 4);
    if (wav.compare(at, 4, id) == 0) return at + 8;
    at += 8 + size + (size & 1);
  }
  return 0;
}

const char* kDisposition = R"(<?xml version="1.0" encoding="UTF-8" ?>
<organ version="3.20">
  <elements>
    <fluidsynth.fluidsynthSound id="1">
      <name>Pipes</name>
      <references/>
      <messages/>
      <soundfont>C:\Organ Sounds\TEST.sf2</soundfont>
      <gain>0.2</gain>
    </fluidsynth.fluidsynthSound>
    <continuousFilter id="2">
      <name>Swell</name>
      <references/>
      <messages>
        <continuous-change>equal 176, equal 11, div 127 | get value</continuous-change>
        <continuousFilter-engaging>set 176, set 11, set value | div 2 | add 0.5 | mult volume 127</continuousFilter-engaging>
      </messages>
      <value>1.0</value>
    </continuousFilter>
    <switchFilter id="3">
      <name>Tremulant</name>
      <references/>
      <messages>
        <switchFilter-intercept>equal 192, get program, </switchFilter-intercept>
        <switchFilter-engaged>set 192, set program | sub 40, </switchFilter-engaged>
        <switchFilter-disengaged>set 192, set program 0, </switchFilter-disengaged>
      </messages>
      <active>false</active>
    </switchFilter>
    <rank id="10">
      <name>Flute</name>
      <references><reference id="1"/><reference id="2"/><reference id="3"/><reference id="5"/></references>
      <messages>
        <rank-engaged>set 176, set 0, set 0</rank-engaged>
        <rank-engaged>set 192, set 50, </rank-engaged>
        <rank-notePlayed>set 144, set pitch, set 127</rank-notePlayed>
        <rank-noteMuted>set 128, set pitch, </rank-noteMuted>
      </messages>
      <channel>greaterEqual 0</channel>
      <delay>0</delay>
    </rank>
    <continuousFilter id="5">
      <name>Tuning</name>
      <references/>
      <messages>
        <continuousFilter-engaging>set 224, set 0, set value | mult 25 | add 51</continuousFilter-engaging>
      </messages>
      <value>0.75</value>
    </continuousFilter>
    <continuousFilter id="4">
      <name>Drum Amplitude</name>
      <references/>
      <messages>
        <continuousFilter-engaging>set 176, set 7, set value | div 2 | add 0.5 | mult volume 127</continuousFilter-engaging>
      </messages>
      <value>0.5</value>
    </continuousFilter>
    <switchFilter id="6">
      <name>Tremulant Gedeckt</name>
      <references/>
      <messages>
        <switchFilter-intercept>equal 192, get program, </switchFilter-intercept>
        <switchFilter-engaged>set 192, set program | add 1, </switchFilter-engaged>
        <switchFilter-disengaged>set 192, set program 0, </switchFilter-disengaged>
      </messages>
    </switchFilter>
    <rank id="12">
      <name>Gedeckt</name>
      <references><reference id="1"/><reference id="6"/></references>
      <messages>
        <rank-engaged>set 176, set 0, set 0</rank-engaged>
        <rank-engaged>set 192, set 60, </rank-engaged>
        <rank-notePlayed>set 144, set pitch, set 127</rank-notePlayed>
      </messages>
    </rank>
    <rank id="11">
      <name>Drum</name>
      <references><reference id="1"/><reference id="4"/></references>
      <messages>
        <rank-engaged>set 176, set 0, set 0</rank-engaged>
        <rank-engaged>set 192, set 3, </rank-engaged>
        <rank-notePlayed>set 144, set pitch, set 127</rank-notePlayed>
      </messages>
    </rank>
    <stop id="20">
      <name>M2.Flute 8'</name>
      <description>GREAT - main
rank = 8  FLUTE
stop=  Flute   Harmonic</description>
      <references><reference id="10"/></references>
      <messages/>
      <active>true</active>
      <action>0</action><from>0</from><to>127</to><transpose>0</transpose><velocity>0</velocity>
    </stop>
    <stop id="21">
      <name>Flute 4'</name>
      <references><reference id="10"/></references>
      <messages/>
      <active>false</active>
      <action>0</action><from>0</from><to>127</to><transpose>12</transpose><velocity>0</velocity>
    </stop>
    <stop id="22">
      <name>Flute 16'</name>
      <references><reference id="10"/></references>
      <messages/>
      <action>0</action><from>0</from><to>127</to><transpose>-12</transpose><velocity>0</velocity>
    </stop>
    <stop id="23">
      <name>Kettle Drum</name>
      <references><reference id="11"/></references>
      <messages/>
      <action>1</action><from>0</from><to>127</to><transpose>-19</transpose><velocity>0</velocity>
    </stop>
    <stop id="24">
      <name>Flute 2'</name>
      <references><reference id="10"/></references>
      <messages/>
      <action>0</action><from>0</from><to>127</to><transpose>24</transpose><velocity>0</velocity>
    </stop>
    <coupler id="31">
      <name>Great Unison Off</name>
      <references><reference id="24"/></references>
      <messages/>
      <active>false</active>
      <action>6</action><from>0</from><to>127</to><transpose>0</transpose><velocity>0</velocity>
    </coupler>
    <coupler id="30">
      <name>Great to Pedal</name>
      <references><reference id="20"/><reference id="21"/></references>
      <messages/>
      <action>0</action><from>0</from><to>127</to><transpose>0</transpose><velocity>0</velocity>
    </coupler>
    <keyboard id="40">
      <name>Great</name>
      <references><reference id="20"/><reference id="21"/><reference id="31"/></references>
      <messages>
        <keyboard-pressKey>equal 145, get pitch, greater 0 | get velocity</keyboard-pressKey>
        <keyboard-releaseKey>equal 129, get pitch, </keyboard-releaseKey>
      </messages>
    </keyboard>
    <keyboard id="41">
      <name>Pedal</name>
      <references><reference id="22"/><reference id="23"/><reference id="30"/></references>
      <messages>
        <keyboard-pressKey>equal 144, sub 12 | get pitch, greater 0 | get velocity</keyboard-pressKey>
      </messages>
    </keyboard>
    <combination id="50">
      <name>Piston 1</name>
      <references/>
      <messages/>
    </combination>
    <console id="60">
      <name>Test Organ</name>
      <references>
        <console-locationReference id="20"><x>10</x><y>20</y></console-locationReference>
      </references>
      <messages/>
    </console>
  </elements>
</organ>
)";

}  // namespace

class JOrganImportTest final : public mp::test::Test {
public:
  JOrganImportTest() : Test("functional.jorgan.import", Category::Functional) {}

  void run() override {
    // ---- the message language -------------------------------------------
    mp::MplVariables vars{{"program", 50.0}};
    MP_CHECK(mp::evaluateMpl("set program | sub 40", 0, vars) == 10.0, "a variable less a number");
    mp::MplVariables none;
    MP_CHECK(std::isnan(mp::evaluateMpl("set value | div 2 | add 0.5 | mult volume 127", 0, none)),
             "an unset variable stops the chain");
    mp::MplVariables value0{{"value", 0.0}};
    MP_CHECK(mp::evaluateMpl("set value | div 2 | add 0.5 | mult volume 127", 0, value0) == 63.5,
             "a named number falls back to its value when the name is not set");
    MP_CHECK(std::isnan(mp::evaluateMpl("equal 144", 145, none)), "a condition that fails stops the chain");
    MP_CHECK(mp::mplMatchedStatus("equal 147, get pitch, greater 0 | get velocity") == 147,
             "a keyboard's status byte");
    const std::vector<int> bytes = mp::mplBytes("set 176, set 121, ");
    MP_CHECK(bytes.size() == 3 && bytes[0] == 176 && bytes[1] == 121 && bytes[2] == 0,
             "an empty command is a zero byte, as jOrgan sends it");

    // ---- the SoundFont ----------------------------------------------------
    const fs::path dir = fs::temp_directory_path() / "mp_jorgan_import";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    mp::SoundFont2::Builder b;
    b.samples.push_back({"low", sine(8000, 130.81), 2000, 6000, 44100, 48, 0});
    b.samples.push_back({"high", sine(8000, 523.25), 2000, 6000, 44100, 72, -10});
    b.samples.push_back({"drum", sine(3000, 80.0), 0, 0, 44100, 60, 0});
    b.samples.push_back({"low trem", sine(8000, 131.5), 2000, 6000, 44100, 48, 0});
    mp::SoundFont2::Builder::Preset flute{"Flute", 0, 50, {}};
    flute.zones.push_back({0, 63, 0, -1, 0, 1, 0, -6000});
    flute.zones.push_back({64, 127, 1, -1, 5, 1, 30, -6000});
    b.presets.push_back(flute);
    mp::SoundFont2::Builder::Preset trem{"Flute trem", 0, 10, {}};
    // Tuned down a tone and back up by the modulation envelope, as the
    // Piteå reeds are: in tune while held.
    trem.zones.push_back({0, 127, 3, -1, 0, 1, 0, -6000, -2, 200});
    b.presets.push_back(trem);
    mp::SoundFont2::Builder::Preset drum{"Drum", 0, 3, {}};
    drum.zones.push_back({0, 127, 2, -1, 0, 0, 0, -12000});
    b.presets.push_back(drum);
    // A tremulant preset on the same sample, swung by the modulation LFO:
    // 3 dB at 5.9 Hz, as the Freedom's are.
    mp::SoundFont2::Builder::Preset gedeckt{"Gedeckt", 0, 60, {}};
    gedeckt.zones.push_back({0, 127, 0, -1, 0, 1, 0, -6000});
    b.presets.push_back(gedeckt);
    mp::SoundFont2::Builder::Preset gedecktTrem{"Gedeckt trem", 0, 61, {}};
    gedecktTrem.zones.push_back({0, 127, 0, -1, 0, 1, 0, -6000, 0, 0, 30, -565});
    b.presets.push_back(gedecktTrem);
    {
      std::ofstream f(dir / "test.sf2", std::ios::binary);
      const std::string s = b.bytes();
      f.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
    mp::SoundFont2 sf;
    std::string err;
    MP_CHECK(sf.open((dir / "test.sf2").string(), err), "the SoundFont opens: " + err);
    MP_CHECK(sf.presets().size() == 5 && sf.samples().size() == 4, "five presets, four samples");
    const int p = sf.findPreset(0, 50);
    MP_CHECK(p == 0, "the flute is bank 0 program 50");
    const auto regions = sf.regions(p);
    MP_CHECK(regions.size() == 2, "two regions, got " + std::to_string(regions.size()));
    if (regions.size() == 2) {
      MP_CHECK(regions[0].keyLo == 0 && regions[0].keyHi == 63 && regions[0].rootKey == 48,
               "the low region from its sample's pitch");
      MP_CHECK(regions[1].rootKey == 72 && regions[1].tuneCents == -5.0 && regions[1].attenuationCb == 30.0,
               "the high region: fine tune plus the sample's correction, and its attenuation");
      MP_CHECK(regions[0].loopMode == 1 && std::abs(regions[0].releaseSec - 0.03125) < 1e-6,
               "looped, with a release of -6000 timecents");
    }
    const auto tremRegions = sf.regions(sf.findPreset(0, 10));
    MP_CHECK(tremRegions.size() == 1 && tremRegions[0].tuneCents == 0.0,
             "coarse tune -2 and the modulation envelope's +200 cents at full sustain hold the pitch");
    std::vector<float> frames;
    MP_CHECK(sf.readFrames(sf.samples()[0].start, sf.samples()[0].start + 10, frames) && frames.size() == 10 &&
                 std::abs(frames[5] - 12000.0 * std::sin(2.0 * 3.141592653589793 * 130.81 * 5 / 44100.0) / 32768.0) <
                     1e-4,
             "the frames read back");

    // ---- the conversion ---------------------------------------------------
    {
      std::ofstream f(dir / "Test.disposition", std::ios::binary);
      f << kDisposition;
    }
    const fs::path work = dir / "work";
    const mp::JOrganImportReport rep = mp::convertJOrgan((dir / "Test.disposition").string(), work.string());
    MP_CHECK(rep.ok, "the disposition converts: " + rep.error);
    if (!rep.ok) return;
    const std::string& t = rep.organText;
    auto has = [&](const std::string& s) { return t.find(s) != std::string::npos; };
    MP_CHECK(has("ChurchName=Test Organ"), "the console names the organ");
    MP_CHECK(has("HasPedals=Y") && has("NumberOfManuals=1"), "a pedal and one manual");
    MP_CHECK(has("[Manual000]\nName=Pedal") && has("[Manual001]\nName=Great"), "the pedal is manual 0");
    MP_CHECK(has("TremulantType=Wave"), "the program-switching filter is a wave tremulant");
    MP_CHECK(has("Name=Tremulant Gedeckt\nDispLabelText=Tremulant Gedeckt\nDisplayed=Y\nDefaultToEngaged=N\n") &&
                 has("TremulantType=Synth\nPeriod=169.") && has("AmpModDepth=29.2"),
             "a tremulant preset on the same samples is a level tremulant at its LFO: 5.9 Hz, 3 dB");
    MP_CHECK(has("AmpMinimumLevel=25"), "the swell closes to the expression it sends at 0: 64 of 127, squared");
    // Key 61 of the flute (MIDI 60) plays the low sample, rooted at 48.
    MP_CHECK(has("Pipe061PitchTuning=1200"), "a key an octave above its region's root is retuned an octave");
    MP_CHECK(has("Pipe073PitchTuning=-5"), "the high region at its root: its fine tune and correction");
    MP_CHECK(has("Pipe073Gain=-1.2"), "30 cB of attenuation, as FluidSynth scales it, is 1.2 dB");
    MP_CHECK(has("Pipe061Attack001IsTremulant=1"), "each flute pipe has its tremulant take");
    MP_CHECK(has("Rank001FirstPipeNumber=49"), "the 4' stop reaches its rank an octave up");
    MP_CHECK(has("Percussive=Y"), "the drum, without a loop, is percussive");
    MP_CHECK(has("Name=Drum (41)"), "the constant-pitch drum is a rank of its own at 60 - 19");
    MP_CHECK(has("Name=Drum (41)\nFirstMidiNoteNumber=0\nNumberOfLogicalPipes=128\nWindchestGroup=2\n"
                 "AmplitudeLevel=100\nPitchTuning=0\nGain=-5.0"),
             "a level filter nothing moves is its rank's level: volume 95 of 127, squared, is -5 dB");
    MP_CHECK(has("NumberOfEnclosures=1"), "and no swell shoe");
    MP_CHECK(has("Name=Flute\nFirstMidiNoteNumber=0\nNumberOfLogicalPipes=128\nWindchestGroup=1\n"
                 "AmplitudeLevel=100\nPitchTuning=18.75\n"),
             "a tuning knob tunes its ranks by the pitch bend it holds: 70 of 128, two semitones' range");
    MP_CHECK(has("DestinationManual=1\nDestinationKeyshift=-12\n"),
             "the coupler on the pedal reaches the great, with the pedal's octave down");
    MP_CHECK(has("Rank001FirstPipeNumber=13\n"), "the pedal's 16' an octave under it again: key 36 plays MIDI note 12");
    MP_CHECK(has("Name=M2.Flute 8'\nDispLabelText=Flute Harmonic\n"),
             "a knob shows the description's stop= line, as jOrgan's skins do");
    MP_CHECK(has("Name=Flute 4'\nDispLabelText=Flute 4'\n"), "else the stop's name");
    MP_CHECK(has("Name=Great Unison Off\nDispLabelText=Great Unison Off\nDisplayed=Y\nDefaultToEngaged=N") &&
                 has("UnisonOff=Y"),
             "the great's coupler played while off is its unison off");
    MP_CHECK(has("[Manual001]\nName=Great") && has("NumberOfStops=3"),
             "the stop only that coupler reaches is the great's");
    bool leftOut = false;
    for (const auto& n : rep.notes) leftOut = leftOut || n.find("combination") != std::string::npos;
    MP_CHECK(leftOut, "the combination is reported as left out");

    // The looped sample: its loop, then the release tail, marked by a cue.
    bool checkedWav = false;
    for (const auto& entry : fs::recursive_directory_iterator(work / "samples")) {
      if (entry.path().filename().string().find("low") == std::string::npos) continue;
      const std::string wav = fileBytes(entry.path());
      const size_t smpl = chunkAt(wav, "smpl"), cue = chunkAt(wav, "cue "), data = chunkAt(wav, "data");
      MP_CHECK(smpl && cue && data, "the WAV has its data, smpl and cue chunks");
      if (!smpl || !cue || !data) break;
      MP_CHECK(le32(wav, smpl + 12) == 48, "its unity note is the sample's");
      MP_CHECK(le32(wav, smpl + 28) == 1 && le32(wav, smpl + 44) == 2000 && le32(wav, smpl + 48) == 5999,
               "its loop, the last point inclusive");
      MP_CHECK(le32(wav, cue + 4 + 20) == 6000, "the release begins where the loop ends");
      const uint32_t frames16 = le32(wav, data - 4) / 2;
      MP_CHECK(frames16 > 6000 + 441, "a release tail follows, " + std::to_string(frames16) + " frames in all");
      checkedWav = true;
      break;
    }
    MP_CHECK(checkedWav, "the low sample was written");

    // The organ file is there, and the GrandOrgue import reads it as an organ.
    MP_CHECK(fs::exists(fs::path(rep.organFile)) && fileBytes(fs::path(rep.organFile)) == rep.organText,
             "organ.organ holds the definition");
    const mp::GrandOrgueImportReport go = mp::convertGrandOrgue(rep.organFile);
    MP_CHECK(go.ok, "the GrandOrgue import reads it: " + go.error);
    mp::OdfLoader loader;
    mp::OrganModel m;
    mp::OdfDiagnostics diag;
    mp::OdfLoader::Options opts;
    MP_CHECK(loader.loadFromXmlString(go.xml, rep.organFile, opts, m, diag), "the converted organ loads");
    MP_CHECK(m.keyboards.size() == 2, "two keyboards, got " + std::to_string(m.keyboards.size()));
    // The coupler is a listed control too, with no ranks of its own.
    size_t sounding = 0;
    for (const auto& [id, s] : m.stops) sounding += s.ranks.empty() ? 0 : 1;
    MP_CHECK(sounding == 5, "five stops that sound, got " + std::to_string(sounding));
    MP_CHECK(m.enclosures.size() == 1, "one swell");
    MP_CHECK(m.tremulants.size() == 1, "one tremulant");
    MP_CHECK(mp::deriveOrganRoot(rep.organFile) == work.string(),
             "its samples are found beside it, in the work folder");
    MP_CHECK(m.samples.size() > 0 && std::all_of(m.samples.begin(), m.samples.end(), [&](const auto& s) {
               return fs::exists(work / fs::path(s.second.fileName));
             }), "every sample it names was written");

    // A second conversion finds the work folder current.
    const mp::JOrganImportReport again = mp::convertJOrgan((dir / "Test.disposition").string(), work.string());
    MP_CHECK(again.ok && again.reused, "a second load reuses the converted samples");
    MP_CHECK(again.organText == rep.organText && again.organFile == rep.organFile, "and gives the same definition");
    MP_CHECK(again.notes == rep.notes, "and the same notes");

    // Damaged files fail to open; they never take the program down.
    {
      std::ofstream f(dir / "Broken.disposition", std::ios::binary);
      f << "<organ><elements><stop id=\"1\"><name>x</name></stop>";
    }
    const mp::JOrganImportReport broken = mp::convertJOrgan((dir / "Broken.disposition").string(), (dir / "w2").string());
    MP_CHECK(!broken.ok && !broken.error.empty(), "a disposition that is not XML is refused: " + broken.error);
    {
      const std::string whole = fileBytes(dir / "test.sf2");
      std::ofstream f(dir / "test.sf2", std::ios::binary | std::ios::trunc);
      f.write(whole.data(), static_cast<std::streamsize>(whole.size() / 3));
    }
    const mp::JOrganImportReport cut = mp::convertJOrgan((dir / "Test.disposition").string(), (dir / "w3").string());
    MP_CHECK(cut.ok, "a truncated SoundFont leaves its ranks silent and the organ opens: " + cut.error);
    fs::remove_all(dir, ec);
  }
};

static JOrganImportTest g_jorganImport;
