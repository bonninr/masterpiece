// The Open window's lists (#1259): versions of one organ in a Hauptwerk
// organs folder, and no organ listed as installed from Masterpiece's own
// folder.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <string>

namespace {

using mp::test::Category;

void writeDefinition(const juce::File& f, const std::string& packages) {
  f.replaceWithText("<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                    "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>" +
                    f.getFileNameWithoutExtension() +
                    "</Identification_Name></_General></ObjectList>"
                    "<ObjectList ObjectType=\"RequiredInstallationPackage\">" +
                    juce::String(packages) + "</ObjectList></Hauptwerk>");
}

std::string full(int id) {
  return "<RequiredInstallationPackage><InstallationPackageID>" + std::to_string(id) +
         "</InstallationPackageID></RequiredInstallationPackage>";
}

class OrganListsTest final : public mp::test::Test {
public:
  OrganListsTest() : Test("functional.ui.organ-lists", Category::Functional) {}
  void run() override {
    const juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-organ-lists");
    root.deleteRecursively();
    const juce::File defs = root.getChildFile("OrganDefinitions");
    defs.createDirectory();
    const auto surround = defs.getChildFile("Rotterdam Hoofdorgel Surround.Organ_Hauptwerk_xml");
    const auto wet = defs.getChildFile("Rotterdam Hoofdorgel Wet.Organ_Hauptwerk_xml");
    const auto caen = defs.getChildFile("Caen St. Etienne.Organ_Hauptwerk_xml");
    const auto compact = defs.getChildFile("Rotterdam Hoofdorgel Dry.Organ_Hauptwerk_xml");
    writeDefinition(surround, full(10) + full(11));
    writeDefinition(wet, full(11));
    writeDefinition(caen, full(20));
    writeDefinition(compact, "<o><a>10</a><b>P</b></o>");

    const auto v = mp::MasterpieceProcessor::organVersions(surround);
    MP_CHECK(v.contains(surround) && v.contains(wet) && v.contains(compact),
             "the perspectives of the same packages are versions: " + std::to_string(v.size()));
    MP_CHECK(!v.contains(caen), "another organ in the same OrganDefinitions is not");
    const auto c = mp::MasterpieceProcessor::organVersions(caen);
    MP_CHECK(c.size() == 1 && c.contains(caen), "an organ with no other perspective has itself only");

    // Masterpiece's own folder: where organs opened from their RAR packages
    // are unpacked. Never a library, so never listed as installed.
    MP_CHECK(mp::MasterpieceProcessor::isOwnDataFolder(mp::dataFolder()), "the data folder is its own");
    MP_CHECK(mp::MasterpieceProcessor::isOwnDataFolder(mp::dataFolder().getChildFile("Packaged").getChildFile("x")),
             "and every folder in it");
    MP_CHECK(!mp::MasterpieceProcessor::isOwnDataFolder(root), "an organs folder elsewhere is not");
    root.deleteRecursively();
  }
};
static OrganListsTest g_organLists;

}  // namespace

#endif
