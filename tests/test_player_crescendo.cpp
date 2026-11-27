// The player's crescendo (JPIL04, #1221): an organ with no crescendo of its
// own, as every GrandOrgue set, gets a pedal of 32 steps. A step adds to what
// the hands drew and going back retires only what it added; a step never set
// takes the nearest set one below; the steps are saved with the combinations.
#include "MpTest.h"

#include "../src/mp_control/PlayerCombinations.h"

#ifdef MP_TEST_HAS_AUDIO
#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#endif

#include <set>
#include <string>

namespace {

using mp::test::Category;
using PC = mp::PlayerCombinations;

class PlayerCrescendoTest final : public mp::test::Test {
public:
  PlayerCrescendoTest() : Test("functional.combinations.player-crescendo", Category::Functional) {}

  void run() override {
    std::vector<PC::Element> els;
    for (mp::Id id = 1; id <= 4; ++id) {
      PC::Element e;
      e.id = id;
      e.divisionId = 1;
      e.name = "Stop " + std::to_string(id);
      els.push_back(e);
    }
    PC pc;
    pc.reset(els, {{1, "Great"}});

    std::set<mp::Id> drawn;
    const auto read = [&](const PC::Element& e) { return drawn.count(e.id) != 0; };
    const auto move = [&](int step) {
      std::vector<PC::Change> out;
      pc.moveCrescendo(step, read, out);
      for (const auto& c : out) {
        if (c.engage) drawn.insert(c.id);
        else drawn.erase(c.id);
      }
    };

    MP_CHECK(PC::crescendoStepOf(0) == 0 && PC::crescendoStepOf(1) == 1 && PC::crescendoStepOf(127) == 32 &&
                 PC::crescendoStepOf(64) == 17,
             "the pedal's travel in 32 steps above closed");

    // Step 1: stop 1; step 8: stops 1 and 2; step 16: 1, 2 and 3.
    drawn = {1};
    pc.storeCrescendo(1, read);
    drawn = {1, 2};
    pc.storeCrescendo(8, read);
    drawn = {1, 2, 3};
    pc.storeCrescendo(16, read);
    MP_CHECK(pc.crescendoSetCount() == 3, "three steps set");

    // The hands draw stop 4, and stop 1.
    drawn = {1, 4};
    move(5);
    MP_CHECK(drawn == std::set<mp::Id>({1, 4}), "step 5 takes step 1's, which the hands already drew");
    move(12);
    MP_CHECK(drawn == std::set<mp::Id>({1, 2, 4}), "step 12 takes step 8's: stop 2 added");
    move(32);
    MP_CHECK(drawn == std::set<mp::Id>({1, 2, 3, 4}), "step 32 takes step 16's");
    move(8);
    MP_CHECK(drawn == std::set<mp::Id>({1, 2, 4}), "back to 8: stop 3 retired");
    move(0);
    MP_CHECK(drawn == std::set<mp::Id>({1, 4}), "closed: the hands' registration, stop 1 kept as they drew it");

    // Set on a step: what it holds then belongs to the crescendo.
    move(20);
    drawn.insert(4);
    MP_CHECK(pc.captureCrescendo(20, read), "a step stored with Set");
    move(0);
    MP_CHECK(drawn.empty(), "closing retires what the step held, stop 4 with it");

    // Saved and read back.
    const std::string text = pc.toText();
    MP_CHECK(text.find("crescendo 8 ") != std::string::npos, "saved as lines of their own:\n" + text);
    PC back;
    back.reset(els, {{1, "Great"}});
    MP_CHECK(back.fromText(text), "the file reads back");
    MP_CHECK(back.crescendoSet(1) && back.crescendoSet(8) && back.crescendoSet(16) && back.crescendoSet(20) &&
                 !back.crescendoSet(2),
             "the same steps set");
    back.clearCrescendo(8);
    MP_CHECK(!back.crescendoSet(8), "a step emptied");

    // Filled from the ready-made levels: every step set, each holding the one
    // before, the last holding them all.
    PC fill;
    fill.reset(els, {{1, "Great"}});
    PC::Suggestion soft, loud;
    soft.drawn = {1, 0, 0, 0};
    loud.drawn = {1, 1, 1, 1};
    MP_CHECK(fill.fillCrescendo({soft, loud}), "filled");
    MP_CHECK(fill.crescendoSetCount() == PC::kCrescendoSteps, "every step set");
    MP_CHECK(!fill.fillCrescendo({soft, loud}), "not filled twice over what is set");
    drawn.clear();
    pc = fill;
    move(1);
    MP_CHECK(drawn == std::set<mp::Id>({1}), "step 1: the softest stop");
    move(32);
    MP_CHECK(drawn.size() == 4, "step 32: everything");

#ifdef MP_TEST_HAS_AUDIO
    runOnOrgan();
#endif
  }

#ifdef MP_TEST_HAS_AUDIO
  // A GrandOrgue organ: two stops, no crescendo of its own.
  void runOnOrgan() {
    juce::MessageManager::getInstance();
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-player-crescendo");
    root.deleteRecursively();
    root.createDirectory();
    const juce::File before = mp::dataFolder();
    mp::setDataFolder(root.getChildFile("data"));
    const juce::File file = root.getChildFile("crescendo.organ");
    file.replaceWithText(
        "[Organ]\r\nChurchName=Player crescendo\r\nHasPedals=N\r\nNumberOfManuals=1\r\n"
        "NumberOfWindchestGroups=1\r\n"
        "[WindchestGroup001]\r\nName=Main\r\n"
        "[Manual001]\r\nName=Great\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=2\r\nStop001=001\r\nStop002=002\r\n"
        "[Stop001]\r\nName=Principal 8\r\nNumberOfLogicalPipes=3\r\nNumberOfAccessiblePipes=3\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nPipe001=P\\036.wav\r\n"
        "Pipe002=P\\037.wav\r\nPipe003=P\\038.wav\r\n"
        "[Stop002]\r\nName=Octave 4\r\nNumberOfLogicalPipes=3\r\nNumberOfAccessiblePipes=3\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nPipe001=O\\036.wav\r\n"
        "Pipe002=O\\037.wav\r\nPipe003=O\\038.wav\r\n");
    {
      mp::MasterpieceProcessor proc;
      MP_CHECK(proc.loadOrgan(file, 0, true).ok, "the organ loads");
      const mp::Id pedal = proc.playerCrescendoControl();
      MP_CHECK(pedal == mp::MasterpieceProcessor::kPlayerCrescendoId &&
                   proc.organModel().continuousControls.count(pedal) == 1,
               "an organ without a crescendo gets the player's pedal");
      mp::Id principal = 0, octave = 0;
      for (const auto& [id, s] : proc.organModel().stops) {
        if (s.name.find("Principal") != std::string::npos) principal = id;
        if (s.name.find("Octave") != std::string::npos) octave = id;
      }
      MP_CHECK(principal != 0 && octave != 0, "both stops");

      // Step 10 holds the Octave, set with the pedal there and Set.
      proc.gotoCrescendoStep(10);
      MP_CHECK(proc.playerCrescendoStep() == 10, "the window's buttons move the pedal to a step");
      proc.setStopEngaged(octave, true);
      MP_CHECK(proc.storeIntoTarget(), "Set stores into the step the pedal is on");
      proc.setStopEngaged(octave, false);

      proc.setStopEngaged(principal, true);
      proc.setContinuousControl(pedal, 0);
      proc.setContinuousControl(pedal, 127);
      MP_CHECK(proc.stopEngaged(octave) && proc.stopEngaged(principal), "open: the Octave added to the hands'");
      MP_CHECK(proc.lcdState().crescendoStep == 32, "the display reads the step");
      proc.setContinuousControl(pedal, 0);
      MP_CHECK(!proc.stopEngaged(octave) && proc.stopEngaged(principal), "closed: the Octave retired, the Principal kept");
      MP_CHECK(proc.saveCombinations(), "saved");
    }
    {
      mp::MasterpieceProcessor proc;
      MP_CHECK(proc.loadOrgan(file, 0, true).ok, "the organ loads again");
      proc.readCombinations();
      MP_CHECK(proc.playerCrescendoStepSet(10) && !proc.playerCrescendoStepSet(9), "step 10 comes back");
    }
    mp::setDataFolder(before);
    root.deleteRecursively();
  }
#endif
};

PlayerCrescendoTest g_playerCrescendo;

}  // namespace
