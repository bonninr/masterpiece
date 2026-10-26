// "Set turns off after storing" is the player's choice and outlives the
// program: it is written when the box is ticked and read again with the next
// organ (#591).
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

using mp::test::Category;

namespace {

juce::File writeOrgan(const juce::String& name) {
  const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-set-off");
  dir.deleteRecursively();
  dir.createDirectory();
  const auto odf = dir.getChildFile(name + ".Organ_Hauptwerk_xml");
  odf.replaceWithText(R"(<?xml version="1.0"?><Hauptwerk FileFormat="Organ">)"
                      R"(<ObjectList ObjectType="_General"><_General>)"
                      R"(<Identification_Name>Set</Identification_Name>)"
                      R"(<Identification_UniqueOrganID>79</Identification_UniqueOrganID>)"
                      R"(</_General></ObjectList></Hauptwerk>)");
  return odf;
}

}  // namespace

class SetOffAfterStorePersistsTest final : public mp::test::Test {
public:
  SetOffAfterStorePersistsTest()
      : Test("functional.settings.set-off-after-store-persists", Category::Functional) {}

  void run() override {
    const auto odf = writeOrgan("set");
    // The program's own settings file is not this test's to change: the
    // defaults are read and written in a folder of their own.
    const auto data = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-set-off-data");
    data.deleteRecursively();
    data.createDirectory();
    const auto previous = mp::dataFolder();
    mp::setDataFolder(data);

    {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
      MP_CHECK(!proc.setOffAfterStore(), "it starts off");
      proc.setSetOffAfterStore(true);
      MP_CHECK(proc.setOffAfterStore(), "the player ticked the box");
    }
    {
      // The next time the program starts: a new engine, the same settings.
      mp::MasterpieceProcessor again;
      again.prepareToPlay(48000.0, 256);
      MP_CHECK(again.loadOrgan(odf, 0, false).ok, "the organ loads again");
      MP_CHECK(again.setOffAfterStore(), "the choice outlived the program");
      again.setSetOffAfterStore(false);
      MP_CHECK(!again.setOffAfterStore(), "and it can be unticked");
    }
    {
      mp::MasterpieceProcessor third;
      third.prepareToPlay(48000.0, 256);
      MP_CHECK(third.loadOrgan(odf, 0, false).ok, "the organ loads a third time");
      MP_CHECK(!third.setOffAfterStore(), "the box stays unticked");
    }

    mp::setDataFolder(previous);
    data.deleteRecursively();
  }
};
static SetOffAfterStorePersistsTest g_setOffAfterStorePersists;

#endif  // MP_TEST_HAS_AUDIO
