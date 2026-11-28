// Refinements asked for before 1.0 (#1285, #1286, #1271): the favourites
// question on Quit is asked once per organ, the last tab used is kept per
// organ, and the tooltip over a drawn manual names its keyboard and channel.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

using mp::test::Category;

namespace {
juce::File writeOrgan(const juce::File& folder, const juce::String& name) {
  const juce::File file = folder.getChildFile(name + ".organ");
  file.replaceWithText(
      "[Organ]\r\nChurchName=" + name +
      "\r\nHasPedals=Y\r\nNumberOfManuals=1\r\nNumberOfWindchestGroups=1\r\n"
      "[WindchestGroup001]\r\nName=Main\r\n"
      "[Manual000]\r\nName=Pedal\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
      "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=002\r\n"
      "[Manual001]\r\nName=Great\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
      "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=001\r\n"
      "[Stop001]\r\nName=Principal 8\r\nNumberOfLogicalPipes=3\r\nNumberOfAccessiblePipes=3\r\n"
      "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nPipe001=P\\036.wav\r\n"
      "Pipe002=P\\037.wav\r\nPipe003=P\\038.wav\r\n"
      "[Stop002]\r\nName=Subbass 16\r\nNumberOfLogicalPipes=3\r\nNumberOfAccessiblePipes=3\r\n"
      "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nPipe001=S\\036.wav\r\n"
      "Pipe002=S\\037.wav\r\nPipe003=S\\038.wav\r\n");
  return file;
}
}  // namespace

class AhallRefinementsTest final : public mp::test::Test {
public:
  AhallRefinementsTest() : Test("functional.settings.quit-favourite-last-tab", Category::Functional) {}

  void run() override {
    juce::MessageManager::getInstance();
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-ahall-refinements");
    root.deleteRecursively();
    root.createDirectory();
    const juce::File before = mp::dataFolder();
    mp::setDataFolder(root.getChildFile("data"));
    const juce::File first = writeOrgan(root, "First");
    const juce::File second = writeOrgan(root, "Second");

    {
      mp::MasterpieceProcessor proc;
      proc.loadGlobalDefaults();
      MP_CHECK(!proc.shouldOfferFavourite(), "nothing loaded: nothing to offer");
      MP_CHECK(proc.loadOrgan(first, 0, true).ok, "the first organ loads");
      MP_CHECK(proc.shouldOfferFavourite(), "an organ on no slot is offered on Quit");
      proc.markFavouriteOffered();
      MP_CHECK(!proc.shouldOfferFavourite(), "offered once: not again");
      MP_CHECK(proc.lastTab().isEmpty(), "no tab kept yet");
      proc.setLastTab("Panel 1");

      mp::Id great = 0;
      for (const auto& [id, kb] : proc.organModel().keyboards)
        if (kb.name == "Great") great = id;
      MP_CHECK(great != 0, "the Great is a keyboard");
      const auto text = proc.keyboardSourceText(great);
      MP_CHECK(text.startsWith("Great: ") && text.contains("channel "),
               "over the manual: its name and channel: " + text.toStdString());
      MP_CHECK(proc.keyboardSourceText(0).isEmpty(), "no keyboard, nothing said");
      MP_CHECK(proc.lastDeviceOnChannel(1) == 0 && proc.lastDeviceOnChannel(0) == 0 &&
                   proc.lastDeviceOnChannel(17) == 0,
               "no device has played yet; channels out of range are none");

      MP_CHECK(proc.loadOrgan(second, 0, true).ok, "the second organ loads");
      MP_CHECK(proc.shouldOfferFavourite(), "the second organ is offered");
      MP_CHECK(proc.lastTab().isEmpty(), "the tab kept is the first organ's, not this one's");
      MP_CHECK(proc.addCurrentOrganToFavourites() != 0, "added to the favourites");
      MP_CHECK(!proc.shouldOfferFavourite(), "a favourite is not offered");
    }
    {
      mp::MasterpieceProcessor proc;
      proc.loadGlobalDefaults();
      MP_CHECK(proc.loadOrgan(first, 0, true).ok, "the first organ loads again");
      MP_CHECK(!proc.shouldOfferFavourite(), "the answer is kept: not offered again");
      MP_CHECK(proc.lastTab() == "Panel 1", "the last tab comes back: " + proc.lastTab().toStdString());
    }
    mp::setDataFolder(before);
    root.deleteRecursively();
  }
};
static AhallRefinementsTest g_ahallRefinements;

#endif
