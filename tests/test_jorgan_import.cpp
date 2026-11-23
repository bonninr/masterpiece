// jOrgan organs: a disposition and its SoundFont, converted and loaded.
//
// The SoundFont is built here: two samples, a preset that splits the keys
// between them, its tremulant twin forty programs below, and a percussive
// preset without a loop. The disposition has a pedal and a manual, a unit
// stop played at two pitches, a coupler, a swell filter that sends the
// expression controller, and a tremulant filter that switches its ranks'
// program as the Freedom theatre organ's do.
#include "MpTest.h"


#include "../src/mp_core/OdfLoader.h"
#include "../src/mp_jorgan/JOrganDisposition.h"
#include "../src/mp_jorgan/JOrganImport.h"
#include "../src/mp_jorgan/SoundFont2.h"

#ifdef MP_TEST_HAS_AUDIO
#include <juce_graphics/juce_graphics.h>
#endif

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
    <stop id="25">
      <name>Gedeckt</name>
      <references><reference id="12"/></references>
      <messages/>
      <action>0</action><from>0</from><to>127</to><transpose>0</transpose><velocity>0</velocity>
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
      <references><reference id="20"/><reference id="21"/><reference id="25"/><reference id="31"/></references>
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
    auto has = [&](const std::string& s) { return rep.xml.find(s) != std::string::npos; };
    bool leftOut = false;
    for (const auto& n : rep.notes) leftOut = leftOut || n.find("combination") != std::string::npos;
    MP_CHECK(leftOut, "the combination is reported as left out");
    MP_CHECK(has("<Text>Flute Harmonic</Text>"), "a knob shows the description's stop= line, as jOrgan's skins do");
    MP_CHECK(has("<Text>Flute 4'</Text>"), "else the stop's name");
    MP_CHECK(has("<FiltParamWhenClsd_OverallAttnDb>11.9"),
             "the swell closes to the expression it sends at 0: 64 of 127, squared, 11.9 dB down");
    MP_CHECK(has("<MasterpiecePercussive>Y</MasterpiecePercussive>"), "the drum, without a loop, is percussive");

    // It loads as an organ.
    mp::OdfLoader loader;
    mp::OrganModel m;
    mp::OdfDiagnostics diag;
    mp::OdfLoader::Options opts;
    MP_CHECK(loader.loadFromXmlString(rep.xml, (dir / "Test.disposition").string(), opts, m, diag),
             "the translated organ loads");
    MP_CHECK(m.organName == "Test Organ", "the console names the organ: " + m.organName);
    MP_CHECK(m.keyboards.size() == 2, "two keyboards, got " + std::to_string(m.keyboards.size()));
    const mp::Keyboard* pedal = nullptr;
    const mp::Keyboard* great = nullptr;
    for (const auto& [id, k] : m.keyboards) (k.name == "Pedal" ? pedal : great) = &k;
    MP_CHECK(pedal && great && pedal->keyboardId == 1 && great->name == "Great", "the pedal first, then the great");
    auto rankNamed = [&](const std::string& name) -> const mp::Rank* {
      for (const auto& [id, r] : m.ranks)
        if (r.name == name) return &r;
      return nullptr;
    };
    auto pipeAt = [](const mp::Rank* r, int midi) -> const mp::Pipe* {
      if (!r) return nullptr;
      for (const auto& p : r->pipes)
        if (p.midiNote == midi) return &p;
      return nullptr;
    };
    auto stopNamed = [&](const std::string& name) -> const mp::Stop* {
      for (const auto& [id, s] : m.stops)
        if (s.name == name) return &s;
      return nullptr;
    };
    const mp::Rank* fluteRank = rankNamed("Flute");
    const mp::Pipe* c4 = pipeAt(fluteRank, 60);
    const mp::Pipe* c5 = pipeAt(fluteRank, 72);
    MP_CHECK(c4 && std::abs(c4->baseTuningDeviationCents - 1218.75) < 0.01,
             "a key an octave above its region's root is retuned an octave, plus the tuning knob's 18.75 cents "
             "(70 of 128 in a two-semitone bend)");
    MP_CHECK(c5 && std::abs(c5->baseTuningDeviationCents - 13.75) < 0.01,
             "the high region at its root: its fine tune and correction, -5 cents, and the knob");
    MP_CHECK(c5 && !c5->layers.empty() && std::abs(c5->layers[0].gainDb + 1.2) < 0.01,
             "30 cB of attenuation, as FluidSynth scales it, is 1.2 dB");
    MP_CHECK(c4 && !c4->layers.empty() && !c4->layers[0].attacks.empty() &&
                 c4->layers[0].attacks[0].sample.fileName.find("low") != std::string::npos,
             "the low region's sample, written as a WAV file");
    MP_CHECK(rankNamed("Flute (tremulant)") != nullptr, "the flute has its tremulant takes");
    const mp::Stop* flute4 = stopNamed("Flute 4'");
    MP_CHECK(flute4 && flute4->ranks.size() == 1 && flute4->ranks[0].midiIncrement == 12 &&
                 flute4->ranks[0].firstMappedDivisionNote == 36,
             "the 4' stop reaches its rank an octave up");
    MP_CHECK(flute4 && flute4->ranks[0].alternateRankId != 0 && flute4->ranks[0].retriggerOnAlternate,
             "and the wave tremulant swaps in the tremulant takes");
    const mp::Stop* flute16 = stopNamed("Flute 16'");
    MP_CHECK(flute16 && flute16->ranks[0].midiIncrement == -24,
             "the pedal's 16' an octave below its key, and the pedal's own octave down again");
    const mp::Rank* drumRank = rankNamed("Drum (41)");
    const mp::Pipe* drumPipe = pipeAt(drumRank, 50);
    MP_CHECK(drumPipe && std::abs(drumPipe->baseTuningDeviationCents + 1900.0) < 0.01,
             "the constant-pitch drum plays note 41 (60 - 19) from every key");
    MP_CHECK(drumPipe && std::abs(drumPipe->layers[0].gainDb + 5.04) < 0.05,
             "a level filter nothing moves is its rank's level: volume 95 of 127, squared, is -5 dB");
    MP_CHECK(m.enclosures.size() == 1, "one swell shoe");
    MP_CHECK(m.tremulants.size() == 2, "two tremulants");
    const mp::Tremulant* lfo = nullptr;
    for (const auto& [id, t] : m.tremulants)
      if (t.name == "Tremulant Gedeckt") lfo = &t;
    MP_CHECK(lfo && std::abs(lfo->engagedHz - 5.9) < 0.05, "the LFO tremulant at its LFO's 5.9 Hz");
    const mp::Pipe* gedecktPipe = pipeAt(rankNamed("Gedeckt"), 60);
    const auto mod = gedecktPipe ? m.tremulantPipes.find(gedecktPipe->pipeId) : m.tremulantPipes.end();
    MP_CHECK(mod != m.tremulantPipes.end() && std::abs(mod->second.ampDepthDb - 5.79) < 0.05,
             "its 3 dB swing: 29% of level, 5.8 dB over the waveform's 15%");
    bool coupled = false, unisonOff = false;
    for (const auto& ka : m.keyActions) {
      if (ka.conditionSwitchId != 0 && ka.conditionWhenEngaged && ka.midiIncrement == -12) coupled = true;
      if (ka.conditionSwitchId != 0 && !ka.conditionWhenEngaged) unisonOff = true;
    }
    MP_CHECK(coupled, "the coupler on the pedal reaches the great, with the pedal's octave down");
    MP_CHECK(unisonOff, "the great's coupler played while off is its unison off");
    MP_CHECK(stopNamed("Flute 2'") && stopNamed("Flute 2'")->divisionId == great->keyboardId,
             "the stop only that coupler reaches is the great's");
    size_t sounding = 0;
    for (const auto& [id, s] : m.stops) sounding += s.ranks.empty() ? 0 : 1;
    MP_CHECK(sounding == 6, "six stops that sound, got " + std::to_string(sounding));
    MP_CHECK(!m.displayPages.empty(), "a console page");
    MP_CHECK(m.samples.size() > 0 && std::all_of(m.samples.begin(), m.samples.end(), [&](const auto& s) {
               return fs::exists(work / fs::path(s.second.fileName));
             }), "every sample it names was written in the work folder");
    MP_CHECK(mp::deriveOrganRoot((dir / "Test.disposition").string()) ==
                 mp::jorganWorkFolder((dir / "Test.disposition").string()),
             "the organ root of a disposition is its work folder");

    // The looped sample: its loop, then the release tail, marked by a cue.
    bool checkedWav = false;
    for (const auto& entry : fs::recursive_directory_iterator(work / "samples")) {
      if (entry.path().filename().string().find("low.wav") == std::string::npos) continue;
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

    // A second conversion finds the work folder current; a header-only one
    // converts nothing.
    const mp::JOrganImportReport again = mp::convertJOrgan((dir / "Test.disposition").string(), work.string());
    MP_CHECK(again.ok && again.reused, "a second load reuses the converted samples");
    MP_CHECK(again.xml == rep.xml, "and gives the same definition");
    MP_CHECK(again.notes == rep.notes, "and the same notes");
    const mp::JOrganImportReport header =
        mp::convertJOrgan((dir / "Test.disposition").string(), (dir / "w4").string(), true);
    MP_CHECK(header.ok && header.name == "Test Organ" && header.id == rep.id && !fs::exists(dir / "w4"),
             "a header-only load names the organ and writes nothing");

    skinned(dir);

    // A SoundFont packed with sfArk, damaged: its ranks are silent, the
    // organ opens.
    {
      std::string text = kDisposition;
      text.replace(text.find("TEST.sf2"), 8, "Packed.sf2");
      std::ofstream f(dir / "Packed.disposition", std::ios::binary);
      f << text;
      std::ofstream p(dir / "packed.sfArk", std::ios::binary);
      p << "not an sfArk file, not at all, only words";
    }
    const mp::JOrganImportReport packed = mp::convertJOrgan((dir / "Packed.disposition").string(), (dir / "w5").string());
    bool packedNote = false;
    for (const auto& n : packed.notes) packedNote = packedNote || n.find("packed.sfArk") != std::string::npos;
    MP_CHECK(packed.ok && packedNote, "a damaged sfArk file is named in the notes and the organ opens");

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

  // A console drawn by its skin: a folder beside the disposition with
  // skin.xml and its pictures, the console at half size.
  void skinned(const fs::path& dir) {
#ifdef MP_TEST_HAS_AUDIO
    const fs::path skin = dir / "TestSkin";
    fs::create_directories(skin);
    auto png = [&](const char* name, int w, int h, juce::Colour c) {
      juce::Image img(juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
      img.clear(img.getBounds(), c);
      juce::File f(juce::String((skin / name).string()));
      juce::FileOutputStream out(f);
      juce::PNGImageFormat().writeImageToStream(img, out);
    };
    png("wood.png", 64, 64, juce::Colour(0xff804020));
    png("off.png", 40, 30, juce::Colour(0xffff0000));
    png("on.png", 40, 30, juce::Colour(0xff00ff00));
    for (int i = 0; i < 4; ++i)
      png(("s" + std::to_string(i) + ".png").c_str(), 20, 50, juce::Colour::greyLevel(i / 3.0f));
    {
      std::ofstream f(skin / "skin.xml", std::ios::binary);
      f << R"(<skin><name>test</name><styles>
<style><name>console</name><layers><image><file>wood.png</file></image></layers></style>
<style><name>stop</name><layers>
  <button><binding>active</binding><fill>BOTH</fill><layers>
    <image><file>off.png</file></image><image><file>ON.png</file></image></layers></button>
  <text><binding>stop</binding><width>40</width><height>30</height><font><name>Serif</name><style>1</style><size>12</size></font>
    <color><red>255</red><green>255</green><blue>255</blue></color></text>
</layers></style>
<style><name>shoe</name><layers><slider><binding>value</binding><layers>
  <image><file>s0.png</file></image><image><file>s1.png</file></image>
  <image><file>s2.png</file></image><image><file>s3.png</file></image></layers></slider></layers></style>
<style><name>label</name><layers>
  <image><file>off.png</file><fill>BOTH</fill><border><top>2</top><left>2</left><bottom>2</bottom><right>2</right></border></image>
  <text><binding>name</binding><padding><top>4</top><left>30</left><bottom>4</bottom><right>30</right></padding>
    <font><name>SansSerif</name><style>0</style><size>12</size></font></text>
</layers></style>
</styles></skin>)";
    }
    std::string text = kDisposition;
    const auto at = [&](const std::string& s) { return text.find(s); };
    text.replace(at("<console-locationReference id=\"20\"><x>10</x><y>20</y></console-locationReference>"),
                 std::string("<console-locationReference id=\"20\"><x>10</x><y>20</y></console-locationReference>").size(),
                 "<console-locationReference id=\"20\"><x>10</x><y>20</y></console-locationReference>"
                 "<console-locationReference id=\"2\"><x>100</x><y>20</y></console-locationReference>"
                 "<console-locationReference id=\"50\"><x>200</x><y>20</y></console-locationReference>");
    text.insert(at("</console>"), "<style>console</style><zoom>0.5</zoom><skin>testskin</skin>");
    text.insert(at("<active>true</active>"), "<style>stop</style>");
    text.insert(at("<value>1.0</value>"), "<style>shoe</style>");
    text.insert(at("<references/>\n      <messages/>\n    </combination>"), "<style>label</style>");
    {
      std::ofstream f(dir / "Skinned.disposition", std::ios::binary);
      f << text;
    }
    const fs::path work = dir / "skinned-work";
    const mp::JOrganImportReport rep = mp::convertJOrgan((dir / "Skinned.disposition").string(), work.string());
    MP_CHECK(rep.ok, "the skinned disposition converts: " + rep.error);
    if (!rep.ok) return;
    auto has = [&](const std::string& s) { return rep.xml.find(s) != std::string::npos; };
    MP_CHECK(has("skin/c1-e20-0.png") && has("skin/c1-e20-1.png"), "the stop drawn off and on");
    MP_CHECK(has("skin/c1-background.png"), "the console's style as its background");
    MP_CHECK(has("<Name>Stock console</Name>"), "the stock page after the skin's");
    auto image = [&](const std::string& rel) {
      return juce::ImageFileFormat::loadFrom(juce::File(juce::String((work / rel).string())));
    };
    const juce::Image off = image("skin/c1-e20-0.png"), on = image("skin/c1-e20-1.png");
    MP_CHECK(off.getWidth() == 20 && off.getHeight() == 15, "the stop's picture at the console's half size");
    MP_CHECK(off.isValid() && on.isValid() && off.getPixelAt(1, 1).getRed() > 200 && on.getPixelAt(1, 1).getGreen() > 200,
             "the button's first child off, its second on, its file name in any case");
    const juce::Image label = image("skin/c1-e50.png");
    MP_CHECK(label.getWidth() >= 30, "the label stretched past its picture by its text's padding, got " +
                                         std::to_string(label.getWidth()));
    MP_CHECK(fs::exists(work / "skin/c1-e2-3.png") && !fs::exists(work / "skin/c1-e2-4.png"), "the shoe's four frames");

    mp::OdfLoader loader;
    mp::OrganModel m;
    mp::OdfDiagnostics diag;
    mp::OdfLoader::Options opts;
    MP_CHECK(loader.loadFromXmlString(rep.xml, (dir / "Skinned.disposition").string(), opts, m, diag),
             "the skinned organ loads");
    MP_CHECK(m.displayPages.size() == 2, "two pages: the skin's console and the stock one");
    const mp::Switch* knob = nullptr;
    for (const auto& [id, s] : m.switches)
      if (s.name == "M2.Flute 8' (Test Organ)") knob = &s;
    MP_CHECK(knob && knob->dispInstanceId != 0 && knob->clickable && knob->defaultEngaged,
             "the stop's picture is a knob of its own, drawn as the stop stands");
    bool forth = false, back = false;
    for (const auto& l : m.switchLinkages) {
      if (knob && l.sourceSwitchId == knob->switchId) forth = true;
      if (knob && l.destSwitchId == knob->switchId) back = true;
    }
    MP_CHECK(forth && back, "wired both ways to the stop's switch");
    const mp::ContinuousControl* shoe = nullptr;
    for (const auto& [id, c] : m.continuousControls)
      if (c.name == "Swell") shoe = &c;
    MP_CHECK(shoe && shoe->imageSetInstanceId != 0, "the swell moves its picture on the skin's page");

    const mp::JOrganImportReport again = mp::convertJOrgan((dir / "Skinned.disposition").string(), work.string());
    MP_CHECK(again.reused, "a second load reuses the drawn pictures");
#else
    (void)dir;
#endif
  }
};

static JOrganImportTest g_jorganImport;
