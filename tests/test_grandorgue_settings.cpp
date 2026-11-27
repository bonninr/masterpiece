// A GrandOrgue settings file beside its MIDI (#1012): the registrations, the
// voicing and the organ's own settings come over, and the report counts what
// Masterpiece has no place for. A small organ is converted from its .organ,
// a .cmb is fed through each part, and the result is read back.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_control/Combinations.h"
#include "../src/mp_control/GrandOrgueSettings.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

// Two manuals. The Great: a Principal from [Rank001] on windchest group 1,
// an Octave carrying its own pipes, the Swell to Great, a tremulant and a
// switch; one general and one divisional. The Swell: a Gedackt. One swell.
const char* const kOrgan =
    "[Organ]\r\nChurchName=Settings import\r\nHasPedals=N\r\nNumberOfManuals=2\r\n"
    "NumberOfWindchestGroups=1\r\nNumberOfRanks=1\r\nNumberOfEnclosures=1\r\nNumberOfTremulants=1\r\n"
    "NumberOfSwitches=1\r\nNumberOfGenerals=1\r\n"
    "[WindchestGroup001]\r\nName=Main\r\nNumberOfEnclosures=1\r\nEnclosure001=001\r\nGain=-2\r\n"
    "[Enclosure001]\r\nName=Swell\r\n"
    "[Tremulant001]\r\nName=Tremulant\r\n"
    "[Switch001]\r\nName=Ventil\r\n"
    "[Rank001]\r\nName=Principal 8\r\nFirstMidiNoteNumber=36\r\nNumberOfLogicalPipes=3\r\nWindchestGroup=001\r\n"
    "AmplitudeLevel=80\r\nPipe001=P8\\036.wav\r\nPipe002=P8\\037.wav\r\nPipe003=P8\\038.wav\r\n"
    "[Manual001]\r\nName=Great\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
    "NumberOfAccessibleKeys=3\r\nNumberOfStops=2\r\nStop001=001\r\nStop002=002\r\n"
    "NumberOfCouplers=1\r\nCoupler001=001\r\nNumberOfTremulants=1\r\nTremulant001=001\r\n"
    "NumberOfSwitches=1\r\nSwitch001=001\r\nNumberOfDivisionals=1\r\nDivisional001=001\r\n"
    "[Manual002]\r\nName=Swell\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
    "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=003\r\n"
    "[Stop001]\r\nName=Principal 8\r\nNumberOfRanks=1\r\nRank001=001\r\n"
    "FirstAccessiblePipeLogicalKeyNumber=1\r\nNumberOfAccessiblePipes=3\r\n"
    "[Stop002]\r\nName=Octave 4\r\nNumberOfLogicalPipes=3\r\nNumberOfAccessiblePipes=3\r\n"
    "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nPipe001=O4\\036.wav\r\nPipe002=O4\\037.wav\r\n"
    "Pipe003=O4\\038.wav\r\n"
    "[Stop003]\r\nName=Gedackt 8\r\nNumberOfLogicalPipes=3\r\nNumberOfAccessiblePipes=3\r\n"
    "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nPipe001=G8\\036.wav\r\nPipe002=G8\\037.wav\r\n"
    "Pipe003=G8\\038.wav\r\n"
    "[Coupler001]\r\nName=II/I\r\nDestinationManual=2\r\n"
    "[General001]\r\nName=Tutti\r\nNumberOfStops=2\r\nStopManual001=1\r\nStopNumber001=1\r\n"
    "StopManual002=1\r\nStopNumber002=2\r\n"
    "[Divisional001]\r\nName=1\r\nNumberOfStops=1\r\nStop001=1\r\n";

// What GrandOrgue saved for it.
const char* const kCmb =
    "[Organ]\r\nChurchName=Settings import\r\nVolume=-9\r\nTemperament=Werckmeister III\r\n"
    // The organ's general, changed: the Octave off, the Gedackt on, the coupler on.
    "[General001]\r\nNumberOfStops=2\r\nStopManual001=1\r\nStopNumber001=-2\r\nStopManual002=2\r\n"
    "StopNumber002=1\r\nNumberOfCouplers=1\r\nCouplerManual001=1\r\nCouplerNumber001=1\r\n"
    "NumberOfTremulants=1\r\nTremulantNumber001=1\r\nNumberOfDivisionalCouplers=1\r\n"
    "DivisionalCouplerNumber001=1\r\n"
    // The organ's divisional, by its place on the Great: the Octave, and an
    // entry naming a stop the Great does not have.
    "[Divisional001]\r\nNumberOfStops=2\r\nStop001=2\r\nStop002=9\r\n"
    // The setter: general 1 kept, general 101 beyond Masterpiece's hundred.
    "[SetterGeneral001]\r\nNumberOfStops=1\r\nStopManual001=1\r\nStopNumber001=1\r\nIsFull=Y\r\n"
    "[SetterGeneral101]\r\nNumberOfStops=1\r\nStopManual001=1\r\nStopNumber001=1\r\n"
    // A sequencer frame, and the Great's divisionals 1 and 21.
    "[FrameGeneral002]\r\nNumberOfStops=1\r\nStopManual001=2\r\nStopNumber001=1\r\n"
    "[Setter001Divisional000]\r\nNumberOfStops=1\r\nStop001=2\r\nNumberOfSwitches=1\r\nSwitch001=1\r\n"
    "[Setter001Divisional020]\r\nNumberOfStops=1\r\nStop001=1\r\n"
    // The crescendo's bank A, step 1: the player's crescendo (#1221).
    "[SetterCrescendo1_001]\r\nNumberOfStops=1\r\nStopManual001=1\r\nStopNumber001=1\r\n"
    // The voicing: the windchest 3 dB down from its own -2, the rank at half
    // its own 80 per cent and 10 cents up, darker, and grouped; its second
    // pipe 2 dB up, and a delay.
    "[WindchestGroup001]\r\nUserGain=-5\r\n"
    "[Rank001]\r\nAmplitude=40\r\nUserGain=0\r\nManualTuning=10\r\nToneBalance=-30\r\nAudioGroup=Gallery\r\n"
    "Pipe002UserGain=2\r\nPipe002Delay=5\r\n"
    // The swell half open, the Principal drawn, the metronome.
    "[Enclosure001]\r\nValue=64\r\n"
    "[Stop001]\r\nDefaultToEngaged=Y\r\n"
    "[Stop002]\r\nDefaultToEngaged=N\r\n"
    "[Metronome]\r\nBPM=100\r\nMeasureLength=3\r\n";

bool near(double a, double b, double tolerance = 0.05) { return std::fabs(a - b) <= tolerance; }

class GrandOrgueSettingsTest final : public mp::test::Test {
public:
  GrandOrgueSettingsTest() : Test("functional.midi.grandorgue-settings", Category::Functional) {}
  void run() override {
    juce::MessageManager::getInstance();
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-go-settings");
    root.deleteRecursively();
    root.createDirectory();
    const juce::File before = mp::dataFolder();
    mp::setDataFolder(root.getChildFile("data"));
    const juce::File file = root.getChildFile("settings.organ");
    file.replaceWithText(kOrgan);
    {
      mp::MasterpieceProcessor proc;
      MP_CHECK(proc.loadOrgan(file, 0, true).ok, "the organ loads");
      const auto& m = proc.organModel();
      const auto sw = [&](const char* key) {
        const auto it = m.grandOrgueObjects.find(key);
        return it == m.grandOrgueObjects.end() ? mp::Id{0} : it->second.id;
      };
      const mp::Id principal = sw("manual001/stop001"), octave = sw("manual001/stop002");
      const mp::Id gedackt = sw("manual002/stop001"), coupler = sw("manual001/coupler001");
      const mp::Id tremulant = sw("tremulant001"), ventil = sw("manual001/switch001");
      MP_CHECK(principal != 0 && octave != 0 && gedackt != 0 && coupler != 0 && tremulant != 0 && ventil != 0,
               "each manual's own list is recorded by place");
      MP_CHECK(m.grandOrgueRanks.count("rank001") == 1 && m.grandOrgueRanks.count("stop002") == 1,
               "the ranks are recorded by section, a stop carrying its pipes too");

      mp::GrandOrgueSettingsReport rep;
      const auto s = mp::readGrandOrgueSettings(kCmb, m, rep);
      MP_CHECK(rep.ok, "a GrandOrgue settings file");

      // ---- registrations.
      MP_CHECK(rep.organPistons == 2, "the organ's general and divisional: " + std::to_string(rep.organPistons));
      const mp::Id generalPiston = sw("general001"), divisionalPiston = sw("divisional001");
      const auto& g = s.organPistons.at(generalPiston).switches;
      const auto has = [](const std::vector<std::pair<mp::Id, bool>>& v, mp::Id id, bool on) {
        for (const auto& [x, o] : v)
          if (x == id) return o == on;
        return false;
      };
      MP_CHECK(has(g, octave, false) && has(g, gedackt, true) && has(g, coupler, true) && has(g, tremulant, true),
               "the general: the Octave off, the Gedackt, the coupler and the tremulant on");
      MP_CHECK(has(s.organPistons.at(divisionalPiston).switches, octave, true),
               "the divisional names the Octave by its place on the Great");
      MP_CHECK(rep.unknownElements == 1, "the stop the Great does not have is counted: " +
                                             std::to_string(rep.unknownElements));
      MP_CHECK(rep.divisionalCouplers == 1, "GrandOrgue's divisional coupler is counted");
      MP_CHECK(rep.generals == 1 && rep.generalsLeftOut == 1, "setter general 1 kept, 101 left out");
      MP_CHECK(s.generals.at(1).full, "IsFull is read");
      MP_CHECK(rep.frames == 1 && s.frames.count(2) == 1, "the sequencer frame 2");
      MP_CHECK(rep.divisionals == 1 && rep.divisionalsLeftOut == 1, "the Great's divisional 1 kept, 21 left out");
      MP_CHECK(rep.crescendoSteps == 1 && rep.crescendoStages == 0 && s.crescendo.count(1) == 1,
               "the crescendo's bank A step 1, for the player's crescendo (#1221)");

      // The organ's piston recalls what the file names, the definition's
      // own element and the others alike.
      {
        mp::CombinationSystem cs;
        cs.reset(m);
        const mp::Id combo = cs.combinationForSwitch(generalPiston);
        MP_CHECK(combo != 0 && cs.store(combo, g), "the general's registration is stored");
        std::vector<mp::CombinationSystem::Change> out;
        cs.recall(combo, out);
        bool gedacktOn = false, octaveOff = false;
        for (const auto& c : out) {
          if (c.switchId == gedackt && c.engage) gedacktOn = true;
          if (c.switchId == octave && !c.engage) octaveOff = true;
        }
        MP_CHECK(gedacktOn && octaveOff, "recall draws the Gedackt, which the definition's general did not name");
      }

      // ---- voicing.
      const mp::Id rank = m.grandOrgueRanks.at("rank001").front().rankId;
      MP_CHECK(s.ranks.count(rank) == 1, "the Principal's rank is voiced");
      const auto& rv = s.ranks.at(rank);
      double bright = 0.0, bass = 0.0;
      mp::toneBalanceShelves(-30, bright, bass);
      MP_CHECK(near(rv.gainDb, -3.0 + 20.0 * std::log10(0.5)), "the level: the chest's -3 dB and the rank's half: " +
                                                                   std::to_string(rv.gainDb));
      MP_CHECK(near(rv.tuningCents, 10.0), "the tuning: " + std::to_string(rv.tuningCents));
      MP_CHECK(near(rv.brightnessDb, bright) && bright < -8.0 && bright > -11.0,
               "ToneBalance -30 darkens the treble: " + std::to_string(rv.brightnessDb));
      MP_CHECK(s.audioGroups.count(rank) == 1 && s.audioGroups.at(rank) == "Gallery", "the audio group");
      MP_CHECK(rep.voicedPipes == 1 && rep.delays == 1, "one pipe voiced, one delay counted");
      const mp::Id octaveRank = m.grandOrgueRanks.at("stop002").front().rankId;
      MP_CHECK(s.ranks.count(octaveRank) == 1 && near(s.ranks.at(octaveRank).gainDb, -3.0),
               "the Octave on the same chest takes the chest's change");
      double b2 = 0.0, bass2 = 0.0;
      mp::toneBalanceShelves(50, b2, bass2);
      MP_CHECK(b2 == 0.0 && bass2 < -6.0, "a positive ToneBalance thins the bass: " + std::to_string(bass2));

      // ---- the organ.
      MP_CHECK(s.volumeChangeDb && near(*s.volumeChangeDb, 6.0), "the volume, 6 dB over GrandOrgue's -15");
      MP_CHECK(s.temperament == "Werckmeister III", "the temperament");
      MP_CHECK(s.swells.size() == 1 && s.swells.begin()->second == 64, "the swell half open");
      MP_CHECK(s.drawn.count(principal) == 1 && s.drawn.at(principal) && s.drawn.count(octave) == 1 &&
                   !s.drawn.at(octave),
               "the drawstops as GrandOrgue left them");
      MP_CHECK(s.metronomeBpm == 100 && s.metronomeBeats == 3, "the metronome");
      MP_CHECK(mp::temperamentFromGrandOrgue("Original temperament") == std::string(),
               "the organ's own temperament");
      MP_CHECK(!mp::temperamentFromGrandOrgue("Rameau bemols").has_value(), "a temperament Masterpiece lacks");

      // ---- applied to the organ loaded.
      proc.applyGrandOrgueSettings(s, rep);
      std::vector<mp::PlayerCombinations::Change> recall;
      proc.playerCombinations().recallGeneral(1, recall);
      bool principalOn = false, octaveOff = false;
      for (const auto& c : recall) {
        const auto st = m.stops.find(c.id);
        const mp::Id cs = st == m.stops.end() ? c.id : st->second.controllingSwitchId;
        if (cs == principal && c.engage) principalOn = true;
        if (cs == octave && !c.engage) octaveOff = true;
      }
      MP_CHECK(principalOn && octaveOff, "the player's general 1: the Principal, and the rest retired");
      MP_CHECK(proc.playerCombinations().frameSet(2), "the sequencer's step 2 is set");
      // No crescendo in the organ: the player's, with GrandOrgue's step 1.
      MP_CHECK(proc.playerCrescendoControl() != 0 && proc.playerCrescendoStepSet(1),
               "the player's crescendo, its step 1 from the file (#1221)");
      MP_CHECK(near(proc.voicing().live().rank(rank).gainDb, rv.gainDb) &&
                   near(proc.voicing().live().rank(rank).tuningCents, 10.0),
               "the rank's voicing is on the Voicing page");
      MP_CHECK(proc.temperamentChoice() == "Werckmeister III" && rep.temperamentTaken, "the temperament is set");
      MP_CHECK(near(proc.metronome().tempo(), 100.0) && proc.metronome().beatsPerBar() == 3, "the metronome is set");
      MP_CHECK(proc.switchEngaged(principal) && !proc.switchEngaged(octave), "the Principal is drawn");
      MP_CHECK(rep.routedRanks == 0 && rep.groupsWithoutOutput.size() == 1,
               "no output is called Gallery, and the report says so");
    }
    mp::setDataFolder(before);
    root.deleteRecursively();
  }
};
GrandOrgueSettingsTest g_grandOrgueSettings;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
