// The stop list a screen reader can use.
//
// Every stop in the plain list carries the name a screen reader reads (the
// stop and the division it is in), reports whether it is drawn, and can be
// drawn and retired by the accessibility layer itself. The state follows the
// engine, so a stop drawn by a piston shows drawn in the list too.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"

#include "../src/mp_ui/Ui.h"

using mp::test::Category;

namespace {

// A small organ: two divisions, three stops, one of them with no pipes in the
// set, and a real sample so the stops that have pipes are playable.
struct TestOrgan {
  juce::File root;
  juce::File odf;

  TestOrgan() {
    root = juce::File::getSpecialLocation(juce::File::tempDirectory)
               .getChildFile("mp-stop-accessibility");
    root.deleteRecursively();
    const auto defs = root.getChildFile("OrganDefinitions");
    const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
    defs.createDirectory();
    pkg.createDirectory();
    {
      juce::WavAudioFormat fmt;
      std::unique_ptr<juce::FileOutputStream> os(pkg.getChildFile("tone.wav").createOutputStream());
      juce::StringPairArray meta;
      meta.set("NumSampleLoops", "1");
      meta.set("Loop0Start", "1000");
      meta.set("Loop0End", "47000");
      std::unique_ptr<juce::AudioFormatWriter> w(
          fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
      juce::AudioBuffer<float> tone(1, 48000);
      for (int i = 0; i < 48000; ++i)
        tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 130.81 * i / 48000.0)));
      w->writeFromAudioSampleBuffer(tone, 0, 48000);
    }
    odf = defs.getChildFile("stops.Organ_Hauptwerk_xml");
    odf.replaceWithText(
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Stops</Identification_Name>"
        "<Identification_UniqueOrganID>77</Identification_UniqueOrganID></_General></ObjectList>"
        "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
        "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage>"
        "<RequiredInstallationPackage>"
        "<InstallationPackageID>2</InstallationPackageID><Name>Q</Name></RequiredInstallationPackage></ObjectList>"
        "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
        "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
        "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample>"
        // The file this one names is in a package of its own, and that package
        // is not here: the stop that uses it has no pipes, as on a demo set.
        "<Sample><SampleID>102</SampleID><InstallationPackageID>2</InstallationPackageID>"
        "<SampleFilename>absent.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
        "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
        "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank>"
        "<Rank><RankID>202</RankID><Name>B</Name></Rank>"
        "<Rank><RankID>203</RankID><Name>C</Name></Rank></ObjectList>"
        "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
        "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
        "<Pipe_SoundEngine01><PipeID>302</PipeID><RankID>202</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
        "<Pipe_SoundEngine01><PipeID>303</PipeID><RankID>203</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
        "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
        "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
        "<Pipe_SoundEngine01_Layer><LayerID>402</LayerID><PipeID>302</PipeID></Pipe_SoundEngine01_Layer>"
        "<Pipe_SoundEngine01_Layer><LayerID>403</LayerID><PipeID>303</PipeID></Pipe_SoundEngine01_Layer>"
        "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
        "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
        "<Pipe_SoundEngine01_AttackSample><UniqueID>502</UniqueID><LayerID>402</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
        "<Pipe_SoundEngine01_AttackSample><UniqueID>503</UniqueID><LayerID>403</LayerID><SampleID>102</SampleID></Pipe_SoundEngine01_AttackSample>"
        "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
        "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
        "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
        "<Keyboard><KeyboardID>702</KeyboardID><Name>II</Name><DefaultInputOutputKeyboardAsgnCode>2</DefaultInputOutputKeyboardAsgnCode>"
        "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
        "</ObjectList><ObjectList ObjectType=\"Division\">"
        "<Division><DivisionID>801</DivisionID><Name>Great</Name></Division>"
        "<Division><DivisionID>802</DivisionID><Name>Swell</Name></Division></ObjectList>"
        "<ObjectList ObjectType=\"KeyAction\">"
        "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
        "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
        "<KeyAction><SourceKeyboardID>702</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>802</DestDivisionID>"
        "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
        "</ObjectList><ObjectList ObjectType=\"Stop\">"
        "<Stop><StopID>901</StopID><Name>Principal 8</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
        "<Stop><StopID>902</StopID><Name>Gedackt 8</Name><DivisionID>801</DivisionID><ControllingSwitchID>1102</ControllingSwitchID></Stop>"
        "<Stop><StopID>903</StopID><Name>Flute 4</Name><DivisionID>802</DivisionID><ControllingSwitchID>1103</ControllingSwitchID></Stop>"
        "</ObjectList><ObjectList ObjectType=\"StopRank\">"
        "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
        "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
        "<StopRank><StopID>902</StopID><RankID>202</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
        "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
        "<StopRank><StopID>903</StopID><RankID>203</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
        "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
        "</ObjectList><ObjectList ObjectType=\"Switch\">"
        "<Switch><SwitchID>1101</SwitchID><Name>Principal 8</Name><Latching>Y</Latching>"
        "<Disp_ImageSetInstanceID>20</Disp_ImageSetInstanceID>"
        "<Disp_ImageSetIndexEngaged>2</Disp_ImageSetIndexEngaged>"
        "<Disp_ImageSetIndexDisengaged>1</Disp_ImageSetIndexDisengaged></Switch>"
        "<Switch><SwitchID>1102</SwitchID><Name>Gedackt 8</Name><Latching>Y</Latching></Switch>"
        "<Switch><SwitchID>1103</SwitchID><Name>Flute 4</Name><Latching>Y</Latching></Switch>"
        "</ObjectList>"
        // A console with one drawstop on it: the knob a screen reader finds.
        "<ObjectList ObjectType=\"DisplayPage\">"
        "<DisplayPage><PageID>1</PageID><Name>Console</Name></DisplayPage></ObjectList>"
        "<ObjectList ObjectType=\"ImageSet\">"
        "<ImageSet><ImageSetID>10</ImageSetID><Name>Knob</Name><InstallationPackageID>1</InstallationPackageID>"
        "<ImageWidthPixels>40</ImageWidthPixels><ImageHeightPixels>60</ImageHeightPixels></ImageSet></ObjectList>"
        "<ObjectList ObjectType=\"ImageSetInstance\">"
        "<ImageSetInstance><ImageSetInstanceID>20</ImageSetInstanceID><Name>Principal knob</Name>"
        "<ImageSetID>10</ImageSetID><DefaultImageIndexWithinSet>1</DefaultImageIndexWithinSet>"
        "<DisplayPageID>1</DisplayPageID><ScreenLayerNumber>5</ScreenLayerNumber>"
        "<LeftXPosPixels>100</LeftXPosPixels><TopYPosPixels>200</TopYPosPixels></ImageSetInstance>"
        "</ObjectList></Hauptwerk>");
  }

  ~TestOrgan() { root.deleteRecursively(); }
};

// The window's copy of the engine's state is published on the next block.
void runOneBlock(mp::MasterpieceProcessor& proc) {
  juce::AudioBuffer<float> buf(2, 256);
  juce::MidiBuffer none;
  proc.processBlock(buf, none);
}

} // namespace

// Every stop in the list names itself and its division, reports whether it is
// drawn, and can be drawn and retired from the accessibility layer.
class StopListAccessibilityTest final : public mp::test::Test {
public:
  StopListAccessibilityTest()
      : Test("functional.ui.stop-list-accessibility", Category::Functional) {}

  void run() override {
    TestOrgan organ;
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    const auto loaded = proc.loadOrgan(organ.odf, 0, false);
    MP_CHECK(loaded.ok, "the organ loads");

    mp::ui::StopJamb jamb(proc);
    MP_CHECK(jamb.stopCount() == 3,
             "the list holds every stop, got " + std::to_string(jamb.stopCount()));

    // The names a screen reader reads: the stop and the division it is in.
    const char* const titles[3] = {"Principal 8, Great", "Gedackt 8, Great", "Flute 4, Swell"};
    std::vector<std::unique_ptr<juce::AccessibilityHandler>> handlers;
    for (int i = 0; i < 3; ++i) {
      auto* button = jamb.stopButtonAt(i);
      MP_CHECK(button != nullptr, "the stop is in the list");
      auto handler = button->makeAccessibilityHandler();
      MP_CHECK(handler != nullptr, "the stop has an accessibility handler");
      MP_CHECK(handler->getTitle() == titles[i],
               "the title names the stop and its division, got \"" +
                   handler->getTitle().toStdString() + "\"");
      MP_CHECK(handler->getRole() == juce::AccessibilityRole::toggleButton,
               "the stop is a toggle button to the accessibility layer");
      MP_CHECK(handler->getCurrentState().isCheckable(),
               "the state says the stop can be drawn or retired");
      MP_CHECK(!handler->getCurrentState().isChecked(), "and it is not drawn yet");
      handlers.push_back(std::move(handler));
    }

    // A stop whose ranks ship no pipes says so: a screen reader reads no
    // tooltip.
    MP_CHECK(jamb.stopButtonAt(2)->getDescription() == juce::String(TRANS("Not available")),
             "the stop with no pipes says it is not available, got \"" +
                 jamb.stopButtonAt(2)->getDescription().toStdString() + "\"");
    MP_CHECK(jamb.stopButtonAt(0)->getDescription().isEmpty(),
             "a stop that can sound says nothing about itself");

    // A stop drawn from the engine shows drawn in the list.
    proc.setStopEngaged(901, true);
    runOneBlock(proc);
    jamb.refresh();
    auto* handler = handlers[0].get();
    MP_CHECK(handler->getCurrentState().isChecked(),
             "a stop drawn from the engine shows drawn");

    // And the accessibility layer can draw and retire the stop itself.
    MP_CHECK(handler->getActions().contains(juce::AccessibilityActionType::toggle),
             "the stop can be toggled from the accessibility layer");
    handler->getActions().invoke(juce::AccessibilityActionType::toggle);
    MP_CHECK(!proc.stopEngaged(901), "the toggle action retires the stop");
    MP_CHECK(!handler->getCurrentState().isChecked(),
             "and the list shows it retired");

    handler->getActions().invoke(juce::AccessibilityActionType::toggle);
    MP_CHECK(proc.stopEngaged(901), "and drawing it again works the other way");
    MP_CHECK(handler->getCurrentState().isChecked(), "and the list shows it drawn");

    // The keyboard: space draws and retires the stop, as a click does.
    auto* button = jamb.stopButtonAt(1);
    MP_CHECK(button->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)),
             "the space bar belongs to the stop");
    MP_CHECK(proc.stopEngaged(902), "the space bar draws the stop");
    MP_CHECK(button->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)),
             "and again");
    MP_CHECK(!proc.stopEngaged(902), "the space bar retires it");

    // The arrow keys move through the list.
    MP_CHECK(button->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)),
             "the right arrow belongs to the stop");
    MP_CHECK(button->keyPressed(juce::KeyPress(juce::KeyPress::leftKey)),
             "and the left arrow");
    MP_CHECK(button->keyPressed(juce::KeyPress(juce::KeyPress::downKey)),
             "and the down arrow");
    MP_CHECK(button->keyPressed(juce::KeyPress(juce::KeyPress::upKey)),
             "and the up arrow");
  }
};

// The drawn console: an invisible component over each drawn control, so a
// screen reader finds the stop the console draws.
class ConsoleAccessibilityTest final : public mp::test::Test {
public:
  ConsoleAccessibilityTest()
      : Test("functional.ui.console-accessibility", Category::Functional) {}

  void run() override {
    TestOrgan organ;
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    const auto loaded = proc.loadOrgan(organ.odf, 0, false);
    MP_CHECK(loaded.ok, "the organ loads");

    mp::ui::ConsoleView console(proc);
    MP_CHECK(console.hasArtwork(), "the console is drawn");

    // The controls the screen reader finds, over the drawn ones.
    std::vector<mp::ui::ConsoleControl*> controls;
    for (auto* child : console.getChildren())
      if (auto* control = dynamic_cast<mp::ui::ConsoleControl*>(child))
        controls.push_back(control);
    MP_CHECK(controls.size() == 1,
             "the console has one control over its drawstop, got " +
                 std::to_string(controls.size()));
    if (controls.empty()) return;

    auto* control = controls.front();
    // It takes no clicks: the console paints and clicks as it always did.
    bool takesClicks = true, takesChildClicks = true;
    control->getInterceptsMouseClicks(takesClicks, takesChildClicks);
    MP_CHECK(!takesClicks && !takesChildClicks,
             "the control over the knob takes no clicks");
    MP_CHECK(control->getBounds() == juce::Rectangle<int>(100, 200, 40, 60),
             "the control is where the knob is drawn, got " +
                 control->getBounds().toString().toStdString());

    auto handler = control->makeAccessibilityHandler();
    MP_CHECK(handler->getTitle() == "Principal 8, Great",
             "the console names the stop and its division, got \"" +
                 handler->getTitle().toStdString() + "\"");
    MP_CHECK(handler->getRole() == juce::AccessibilityRole::toggleButton,
             "the drawn stop is a toggle button to the accessibility layer");
    MP_CHECK(handler->getCurrentState().isCheckable(),
             "the state says the stop can be drawn or retired");
    MP_CHECK(!handler->getCurrentState().isChecked(), "and it is not drawn yet");

    // The accessibility layer draws the stop, and the console follows.
    handler->getActions().invoke(juce::AccessibilityActionType::toggle);
    runOneBlock(proc);
    MP_CHECK(proc.stopEngaged(901), "the toggle action draws the stop");
    console.refreshControlStates();
    MP_CHECK(handler->getCurrentState().isChecked(),
             "and the console's control shows the stop drawn");

    // And the state follows the engine, so a piston is announced too.
    proc.setStopEngaged(901, false);
    runOneBlock(proc);
    console.refreshControlStates();
    MP_CHECK(!handler->getCurrentState().isChecked(),
             "the console's control shows the stop retired");
  }
};

static StopListAccessibilityTest g_stopListAccessibility;
static ConsoleAccessibilityTest g_consoleAccessibility;

#endif  // MP_TEST_HAS_AUDIO
