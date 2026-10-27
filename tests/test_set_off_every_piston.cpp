// "Set turns off after storing" holds for every piston (#591): the console's
// own pistons, the player's pistons pressed over MIDI or the control API, the
// sequencer's stepper, as well as those clicked in the combinations window
// (#130).
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"

using mp::test::Category;

class SetOffEveryPistonTest final : public mp::test::Test {
public:
  SetOffEveryPistonTest() : Test("functional.control.set-off-every-piston", Category::Functional) {}

  void run() override {
    const juce::File fixture(juce::String(MP_TEST_FIXTURES_DIR) + "/minimal.Organ_Hauptwerk_xml");
    juce::String xml = fixture.loadFileAsString();
    // The organ's own Set (12) and a general piston (101) that stores stop 1101.
    xml = xml.replaceFirstOccurrenceOf(
        "<ObjectList ObjectType=\"Switch\">",
        "<ObjectList ObjectType=\"Switch\">"
        "<Switch><SwitchID>12</SwitchID><Name>Comb: Capture mode</Name><Latching>Y</Latching>"
        "<Clickable>Y</Clickable><DefaultInputOutputSwitchAsgnCode>12</DefaultInputOutputSwitchAsgnCode></Switch>"
        "<Switch><SwitchID>101</SwitchID><Name>General 1</Name><Latching>N</Latching>"
        "<Clickable>Y</Clickable></Switch>");
    xml = xml.replaceFirstOccurrenceOf(
        "<ObjectList ObjectType=\"Combination\">",
        "<ObjectList ObjectType=\"Combination\"><Combination><CombinationID>1203</CombinationID>"
        "<Name>General 1</Name><CombinationTypeCode>2</CombinationTypeCode>"
        "<ActivatingSwitchID>101</ActivatingSwitchID><AllowsCapture>Y</AllowsCapture></Combination>");
    xml = xml.replaceFirstOccurrenceOf(
        "<ObjectList ObjectType=\"CombinationElement\">",
        "<ObjectList ObjectType=\"CombinationElement\"><CombinationElement>"
        "<CombinationElementID>1303</CombinationElementID><CombinationID>1203</CombinationID>"
        "<ControlledSwitchID>1101</ControlledSwitchID><CapturedSwitchID>1101</CapturedSwitchID>"
        "<InitialStoredStateIsEngaged>N</InitialStoredStateIsEngaged></CombinationElement>");

    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-set-off");
    dir.deleteRecursively();
    dir.createDirectory();
    const auto odf = dir.getChildFile("setoff.Organ_Hauptwerk_xml");
    odf.replaceWithText(xml);

    mp::MasterpieceProcessor proc;
    const juce::File global = proc.globalSettingsFile();
    const bool had = global.existsAsFile();
    const juce::String before = had ? global.loadFileAsString() : juce::String();
    MP_CHECK(proc.loadOrgan(odf, 0, true).ok, "the organ loads");
    {
      const auto& combos = proc.organModel().combinations;
      const auto c = combos.find(1203);
      MP_CHECK(c != combos.end() && c->second.elements.size() == 1 && c->second.allowsCapture,
               "the piston's combination loads with its element");
    }

    // Off, the default: Set stays down after a store, on the console and over MIDI.
    proc.setSetOffAfterStore(false);
    proc.setSwitchEngaged(1101, true);
    proc.setCaptureMode(true);
    proc.setSwitchEngaged(101, true);
    MP_CHECK(proc.captureMode() && proc.switchEngaged(12), "with the option off, Set stays down");
    proc.pressGeneral(1);
    MP_CHECK(proc.captureMode(), "with the option off, Set stays down after a player piston");
    proc.setCaptureMode(false);
    // The paired check: the console piston stored, and recalls with Set up.
    proc.setSwitchEngaged(1101, false);
    proc.setSwitchEngaged(101, true);
    MP_CHECK(proc.switchEngaged(1101), "the console piston recalls what it stored");

    proc.setSetOffAfterStore(true);

    // The console's own piston stores, and Set lets go, its switch with it.
    proc.setSwitchEngaged(1101, true);
    proc.setCaptureMode(true);
    MP_CHECK(proc.switchEngaged(12), "Set is down");
    proc.setSwitchEngaged(101, true);
    MP_CHECK(!proc.captureMode(), "a console piston that stored turns Set off");
    MP_CHECK(!proc.switchEngaged(12), "and the organ's Set switch with it");
    MP_CHECK(!proc.switchEngaged(101), "the piston lets itself out as before");
    proc.setSwitchEngaged(1101, false);
    proc.setSwitchEngaged(101, true);
    MP_CHECK(proc.switchEngaged(1101), "the next press recalls what was stored");

    // A player's general, as MIDI and the control API press it.
    proc.setCaptureMode(true);
    proc.pressGeneral(1);
    MP_CHECK(!proc.captureMode(), "a player general that stored turns Set off");

    // A player's divisional.
    const auto& divisions = proc.playerCombinations().divisions();
    if (!divisions.empty()) {
      proc.setCaptureMode(true);
      proc.pressDivisional(divisions.front().divisionId, 1);
      MP_CHECK(!proc.captureMode(), "a player divisional that stored turns Set off");
    }

    // The sequencer's stepper stores a frame, so with the option on it lets Set
    // go at that step: a step taken by mistake does not leave Set down for the
    // next piston to overwrite (#591).
    proc.setCaptureMode(true);
    MP_CHECK(proc.stepperNext(), "a step stores a frame");
    MP_CHECK(!proc.captureMode(), "a step that stored turns Set off");

    // Off, as the player who builds a sequence wants it: Set stays down and
    // every step stores, frame after frame.
    proc.setSetOffAfterStore(false);
    proc.setCaptureMode(true);
    proc.stepperNext();
    proc.stepperNext();
    MP_CHECK(proc.captureMode(), "with the option off, stepping keeps Set down");
    proc.setCaptureMode(false);

    proc.settingsFileFor(odf).deleteFile();
    proc.combinationFileFor(odf).deleteFile();
    if (had) global.replaceWithText(before);
    else global.deleteFile();
    dir.deleteRecursively();
  }
};
static SetOffEveryPistonTest g_setOffEveryPiston;

#endif  // MP_TEST_HAS_AUDIO
