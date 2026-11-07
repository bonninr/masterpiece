// A mixture drawn as one drawstop over a stop per rank, as Hill's Great
// Mixture III is (#832). The drawstop is wired into the node of each rank's
// stop; the second and third rank also carry a clickable switch drawn as a
// one-pixel image, which no player can hit. The drawstop is the knob of all
// three, so drawing any rank draws it, and General Cancel, which knows only
// the drawstop, puts all three in. Each rank's crescendo path waits on
// Crescendo Plus, a drawn knob, which is no rank's knob. A Tutti over two
// divisions stays an aid.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <string>

namespace {

using mp::test::Category;

std::string sw(int id, const char* name, bool latching, bool clickable, int image) {
  return "<Switch><SwitchID>" + std::to_string(id) + "</SwitchID><Name>" + name + "</Name><Latching>" +
         (latching ? "Y" : "N") + "</Latching><Clickable>" + (clickable ? "Y" : "N") + "</Clickable>" +
         (image ? "<Disp_ImageSetInstanceID>" + std::to_string(image) + "</Disp_ImageSetInstanceID>" : std::string()) +
         "</Switch>";
}

std::string link(int from, int to, int condition = 0) {
  return "<SwitchLinkage><SourceSwitchID>" + std::to_string(from) + "</SourceSwitchID><DestSwitchID>" +
         std::to_string(to) + "</DestSwitchID>" +
         (condition ? "<ConditionSwitchID>" + std::to_string(condition) +
                          "</ConditionSwitchID><ConditionSwitchLinkIfEngaged>Y</ConditionSwitchLinkIfEngaged>"
                    : std::string()) +
         "<SourceSwitchLinkIfEngaged>Y</SourceSwitchLinkIfEngaged>"
         "<EngageLinkActionCode>1</EngageLinkActionCode>"
         "<DisengageLinkActionCode>2</DisengageLinkActionCode></SwitchLinkage>";
}

// Hill's crescendo path into a rank's node: the crescendo step's switch,
// passed on only while Crescendo Plus is drawn.
std::string crescendoPath(int rank) {
  return link(14000 + rank, 15000 + rank, 13038) + link(15000 + rank, 19000 + rank);
}

std::string crescendoSwitches(int rank) {
  const std::string n = std::to_string(rank);
  return "<Switch><SwitchID>140" + n + "</SwitchID><Name>CrescendoCombSwitch " + n +
         "</Name><Latching>Y</Latching></Switch><Switch><SwitchID>150" + n + "</SwitchID><Name>CrescendoIfOnSwitch " +
         n + "</Name><Latching>N</Latching></Switch>";
}

std::string imageSet(int id, int right, int bottom) {
  return "<ImageSet><ImageSetID>" + std::to_string(id) + "</ImageSetID><Name>Set " + std::to_string(id) +
         "</Name><ClickableAreaLeftRelativeXPosPixels>0</ClickableAreaLeftRelativeXPosPixels>"
         "<ClickableAreaRightRelativeXPosPixels>" +
         std::to_string(right) +
         "</ClickableAreaRightRelativeXPosPixels>"
         "<ClickableAreaTopRelativeYPosPixels>0</ClickableAreaTopRelativeYPosPixels>"
         "<ClickableAreaBottomRelativeYPosPixels>" +
         std::to_string(bottom) + "</ClickableAreaBottomRelativeYPosPixels></ImageSet>";
}

std::string instance(int id, int set, int left) {
  return "<ImageSetInstance><ImageSetInstanceID>" + std::to_string(id) + "</ImageSetInstanceID><Name>Instance " +
         std::to_string(id) + "</Name><ImageSetID>" + std::to_string(set) +
         "</ImageSetID><DisplayPageID>1</DisplayPageID><LeftXPosPixels>" + std::to_string(left) +
         "</LeftXPosPixels><TopYPosPixels>10</TopYPosPixels></ImageSetInstance>";
}

std::string stop(int id, const char* name, int division, int node) {
  return "<Stop><StopID>" + std::to_string(id) + "</StopID><Name>" + name + "</Name><DivisionID>" +
         std::to_string(division) + "</DivisionID><ControllingSwitchID>" + std::to_string(node) +
         "</ControllingSwitchID></Stop>";
}

std::string organText() {
  return "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
         "<ObjectList ObjectType=\"_General\"><_General>"
         "<Identification_UniqueOrganID>832</Identification_UniqueOrganID>"
         "<Identification_Name>One knob over three ranks</Identification_Name>"
         "</_General></ObjectList>"
         "<ObjectList ObjectType=\"Division\"><Division><DivisionID>3</DivisionID><Name>Great</Name></Division>"
         "<Division><DivisionID>4</DivisionID><Name>Swell</Name></Division></ObjectList>"
         "<ObjectList ObjectType=\"DisplayPage\"><DisplayPage><PageID>1</PageID><Name>Console</Name>"
         "</DisplayPage></ObjectList>"
         "<ObjectList ObjectType=\"ImageSet\">" +
         imageSet(1, 200, 190) + imageSet(2, 1, 1) +
         "</ObjectList><ObjectList ObjectType=\"ImageSetInstance\">" + instance(1, 1, 10) + instance(2, 2, 0) +
         instance(3, 2, 0) + instance(4, 1, 300) + instance(5, 1, 500) +
         "</ObjectList><ObjectList ObjectType=\"Switch\">" + sw(13059, "GT. Mixture", true, true, 1) +
         sw(13085, "GT. Mixture rank 2", true, true, 2) + sw(13086, "GT. Mixture rank 3", true, true, 3) +
         sw(19059, "Slider rank 1", false, false, 0) + sw(19085, "Slider rank 2", false, false, 0) +
         sw(19086, "Slider rank 3", false, false, 0) + sw(130, "Tutti", true, true, 4) +
         sw(19003, "Slider GT. Flute", false, false, 0) + sw(19004, "Slider SW. Oboe", false, false, 0) +
         sw(1448, "Gen. Cancel", false, false, 0) + sw(13038, "Crescendo Plus", true, true, 5) +
         crescendoSwitches(59) + crescendoSwitches(85) + crescendoSwitches(86) +
         "</ObjectList><ObjectList ObjectType=\"SwitchLinkage\">" + link(13059, 19059) + link(13059, 19085) +
         link(13059, 19086) + link(13085, 19085) + link(13086, 19086) + link(130, 19003) + link(130, 19004) +
         crescendoPath(59) + crescendoPath(85) + crescendoPath(86) +
         "</ObjectList><ObjectList ObjectType=\"Stop\">" + stop(59, "GT. Mixture rank 1", 3, 19059) +
         stop(85, "GT. Mixture rank 2", 3, 19085) + stop(86, "GT. Mixture rank 3", 3, 19086) +
         stop(3, "GT. Flute", 3, 19003) + stop(4, "SW. Oboe", 4, 19004) +
         "</ObjectList><ObjectList ObjectType=\"Combination\">"
         "<Combination><CombinationID>1448</CombinationID><Name>General cancel</Name>"
         "<CombinationTypeCode>6</CombinationTypeCode><ActivatingSwitchID>1448</ActivatingSwitchID>"
         "<CanEngageControlledSwitches>N</CanEngageControlledSwitches>"
         "<AllowsCapture>N</AllowsCapture></Combination>"
         "</ObjectList><ObjectList ObjectType=\"CombinationElement\"><CombinationElement>"
         "<CombinationElementID>1</CombinationElementID><CombinationID>1448</CombinationID>"
         "<ControlledSwitchID>13059</ControlledSwitchID>"
         "<InitialStoredStateIsEngaged>N</InitialStoredStateIsEngaged></CombinationElement>"
         "</ObjectList></Hauptwerk>";
}

class MixtureKnobTest final : public mp::test::Test {
public:
  MixtureKnobTest() : Test("functional.control.mixture-one-knob", Category::Functional) {}
  void run() override {
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-mixture-knob");
    dir.deleteRecursively();
    dir.createDirectory();
    const juce::File file = dir.getChildFile("mixture.Organ_Hauptwerk_xml");
    file.replaceWithText(juce::String::fromUTF8(organText().c_str()));

    {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(file);
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(file, 0, true).ok, "the organ loads");
      juce::AudioBuffer<float> buf(2, 256);
      juce::MidiBuffer none;
      proc.processBlock(buf, none);

      const auto& m = proc.organModel();
      MP_CHECK(m.switches.at(13059).clickable, "the drawstop can be clicked");
      MP_CHECK(!m.switches.at(13085).clickable && !m.switches.at(13086).clickable,
               "a switch drawn one pixel wide cannot");
      MP_CHECK(proc.playerSwitchFor(19059) == 13059 && proc.playerSwitchFor(19085) == 13059 &&
                   proc.playerSwitchFor(19086) == 13059,
               "each rank leads to the mixture's drawstop");
      MP_CHECK(proc.playerSwitchFor(19003) != 130 && proc.playerSwitchFor(19004) != 130,
               "a Tutti over two divisions is no stop's knob");

      proc.setStopEngaged(85, true);
      proc.processBlock(buf, none);
      MP_CHECK(proc.switchEngaged(13059), "drawing a rank moves the drawstop");
      MP_CHECK(proc.stopEngaged(59) && proc.stopEngaged(85) && proc.stopEngaged(86), "and draws all three ranks");

      proc.setSwitchEngaged(1448, true);
      proc.setSwitchEngaged(1448, false);
      proc.processBlock(buf, none);
      MP_CHECK(!proc.switchEngaged(13059), "General Cancel puts the drawstop in");
      MP_CHECK(!proc.stopEngaged(59) && !proc.stopEngaged(85) && !proc.stopEngaged(86),
               "and all three ranks with it");

      proc.setSwitchEngaged(130, true);
      proc.processBlock(buf, none);
      MP_CHECK(proc.stopEngaged(3) && proc.stopEngaged(4), "the Tutti still draws its stops");
      settings.deleteFile();
      proc.combinationFileFor(file).deleteFile();
    }
    dir.deleteRecursively();
  }
};
MixtureKnobTest g_mixtureKnob;

}  // namespace

#endif
