// The player's bass and melody couplers (#722), for an organ played from one
// keyboard: with the extra couplers on, each manual gets a bass coupler from
// the pedal and a melody coupler from each other manual. Combinations know
// them as bass and melody couplers, for the option that keeps the pistons
// from storing and recalling them.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <map>
#include <string>

namespace {

using mp::test::Category;

class ProgramBassMelodyTest final : public mp::test::Test {
public:
  ProgramBassMelodyTest()
      : Test("functional.couplers.program-bass-melody", Category::Functional) {}
  void run() override {
    const std::string organ =
        "[Organ]\r\nChurchName=Bass and melody\r\nHasPedals=Y\r\nNumberOfManuals=2\r\n"
        "[Manual000]\r\nName=Pedal\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=3\r\n"
        "[Manual001]\r\nName=Great\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=1\r\n"
        "[Manual002]\r\nName=Swell\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=2\r\n"
        "[Stop001]\r\nName=Principal 8\r\nNumberOfLogicalPipes=1\r\nNumberOfAccessiblePipes=1\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nPipe001=P8\\036.wav\r\n"
        "[Stop002]\r\nName=Viola 8\r\nNumberOfLogicalPipes=1\r\nNumberOfAccessiblePipes=1\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nPipe001=V8\\036.wav\r\n"
        "[Stop003]\r\nName=Subbass 16\r\nNumberOfLogicalPipes=1\r\nNumberOfAccessiblePipes=1\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nPipe001=S16\\036.wav\r\n";
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-program-bass-melody");
    dir.deleteRecursively();
    dir.createDirectory();
    const juce::File file = dir.getChildFile("bass-melody.organ");
    file.replaceWithText(juce::String::fromUTF8(organ.c_str()));
    {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(file);
      proc.setProgramCouplers(true);
      MP_CHECK(proc.loadOrgan(file, 0, true).ok, "the organ loads");

      std::map<std::string, mp::Id> byName;
      std::map<std::string, int> extremeOf;
      for (const auto& pc : proc.programCouplerList()) {
        const auto sw = proc.organModel().switches.find(pc.switchId);
        if (sw == proc.organModel().switches.end()) continue;
        byName[sw->second.name] = pc.switchId;
        extremeOf[sw->second.name] = pc.extreme;
      }
      for (const char* name : {"Bass Pedal to Great", "Bass Pedal to Swell"})
        MP_CHECK(extremeOf.count(name) != 0 && extremeOf[name] == mp::CouplerMatrix::kLowestHeld,
                 std::string(name) + " is there, on the lowest key");
      for (const char* name : {"Melody Swell to Great", "Melody Great to Swell"})
        MP_CHECK(extremeOf.count(name) != 0 && extremeOf[name] == mp::CouplerMatrix::kHighestHeld,
                 std::string(name) + " is there, on the highest key");
      MP_CHECK(byName.count("Melody Pedal to Great") == 0 && byName.count("Bass Pedal to Pedal") == 0,
               "the pedal gets none of its own, and is never a melody");
      MP_CHECK(byName.count("Great to Swell 8'") != 0, "the 8' couplers are still there");

      const mp::Id bass = byName["Bass Pedal to Great"];
      bool flagged = false;
      for (const auto& e : proc.playerCombinations().elements())
        if (e.kind == mp::PlayerCombinations::ElementKind::Switch && e.id == bass) flagged = e.bassOrMelody;
      MP_CHECK(flagged, "the combinations know it as a bass coupler");
      MP_CHECK(proc.hasBassOrMelodyCoupler(), "so the option to leave it is offered");

      proc.setSwitchEngaged(bass, true);
      MP_CHECK(proc.switchEngaged(bass), "the bass coupler is drawn");
      proc.pressGeneralCancel();
      MP_CHECK(!proc.switchEngaged(bass), "General Cancel puts it in, as it does every coupler");
      settings.deleteFile();
    }
    dir.deleteRecursively();
  }
};
static ProgramBassMelodyTest g_programBassMelody;

}  // namespace

#endif
