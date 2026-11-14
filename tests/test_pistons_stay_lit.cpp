// Pistons that stay lit (#1117). By default a stop moved by hand puts the lit
// piston out, since it no longer describes the console (#979). With "Pistons
// stay lit until another is pressed" the lamp shows the general in use until
// another piston, a cancel or the sequencer moves the registration.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

namespace {

using mp::test::Category;

class PistonsStayLitTest final : public mp::test::Test {
public:
  PistonsStayLitTest() : Test("functional.control.pistons-stay-lit", Category::Functional) {}
  void run() override {
    const juce::File odf(juce::String(MP_TEST_FIXTURES_DIR) + "/minimal.Organ_Hauptwerk_xml");
    mp::MasterpieceProcessor proc;
    const juce::File combinations = proc.combinationFileFor(odf);
    const bool hadCombinations = combinations.existsAsFile();
    const juce::String keptCombinations = hadCombinations ? combinations.loadFileAsString() : juce::String();
    combinations.deleteFile();
    const juce::File settings = proc.settingsFileFor(odf);
    const bool hadSettings = settings.existsAsFile();
    const juce::String keptSettings = hadSettings ? settings.loadFileAsString() : juce::String();
    const bool before = proc.pistonsStayLit();

    MP_CHECK(proc.loadOrgan(odf, 0, true).ok, "the fixture loads");
    const auto& pc = proc.playerCombinations();
    proc.setStopEngaged(901, true);
    proc.setCaptureMode(true);
    proc.pressGeneral(1);
    proc.pressDivisional(801, 2);
    proc.setCaptureMode(false);
    proc.setCaptureMode(true);
    proc.pressGeneralCancel();
    proc.pressGeneral(2);
    proc.setCaptureMode(false);
    MP_CHECK(pc.generalSet(1) && pc.generalSet(2) && pc.divisionalSet(801, 2), "Set stores the pistons");

    proc.setPistonsStayLit(false);
    proc.pressGeneral(1);
    MP_CHECK(pc.litGeneral() == 1, "the general lights");
    proc.setStopEngaged(901, false);
    MP_CHECK(pc.litGeneral() == 0, "by default a stop moved by hand puts it out");

    proc.setPistonsStayLit(true);
    proc.pressGeneral(1);
    MP_CHECK(pc.litGeneral() == 1, "with the option, the general lights");
    proc.setStopEngaged(901, false);
    MP_CHECK(!proc.stopEngaged(901) && pc.litGeneral() == 1, "and stays lit with a stop pushed in by hand");
    proc.setStopEngaged(901, true);
    MP_CHECK(pc.litGeneral() == 1, "and drawn again");
    proc.pressGeneral(2);
    MP_CHECK(pc.litGeneral() == 2, "another general takes the light");
    proc.pressDivisional(801, 2);
    MP_CHECK(pc.litGeneral() == 0 && pc.litDivisional(801) == 2, "a divisional takes it from the general");
    proc.setStopEngaged(901, false);
    MP_CHECK(pc.litDivisional(801) == 2, "the divisional stays lit through a hand move too");
    proc.pressGeneral(1);
    proc.pressGeneralCancel();
    MP_CHECK(pc.litGeneral() == 0 && pc.litDivisional(801) == 0, "the general cancel puts every lamp out");

    proc.setPistonsStayLit(before);
    combinations.deleteFile();
    if (hadCombinations) combinations.replaceWithText(keptCombinations);
    settings.deleteFile();
    if (hadSettings) settings.replaceWithText(keptSettings);
  }
};
static PistonsStayLitTest g_pistonsStayLit;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
