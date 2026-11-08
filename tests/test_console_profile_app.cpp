// A console profile on a loaded organ (#866): its tabs matched to the
// organ's unmapped controls, the strong matches mapped, the player's own
// mapping left alone, the mapped tabs working, and an organ mapped by hand
// made into a profile.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_ui/Ui.h"

#include <cmath>
#include <memory>
#include <string>

namespace {

using mp::test::Category;

mp::MidiBinding cc(int number, int device, mp::Id target) {
  mp::MidiBinding b;
  b.source.kind = mp::MidiSourceKind::ControlChange;
  b.source.channel = 1;
  b.source.number = number;
  b.source.deviceId = device;
  b.targetKind = mp::MidiTargetKind::Switch;
  b.targetId = target;
  b.trigger = mp::MidiTrigger::Toggle;
  return b;
}

class ConsoleProfileAppTest final : public mp::test::Test {
public:
  ConsoleProfileAppTest() : Test("functional.ui.console-profile", Category::Functional) {}
  void run() override {
    juce::MessageManager::getInstance();
    // A German organ: Principal on the Great, Gedackt and Trompete on the
    // Swell, and the Swell to Great.
    const std::string organ =
        "[Organ]\r\nChurchName=Console profile\r\nHasPedals=N\r\nNumberOfManuals=2\r\n"
        "NumberOfWindchestGroups=1\r\n[WindchestGroup001]\r\nName=G\r\n"
        "[Manual001]\r\nName=Hauptwerk\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=001\r\n"
        "NumberOfCouplers=1\r\nCoupler001=001\r\n"
        "[Manual002]\r\nName=Schwellwerk\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=2\r\nStop001=002\r\nStop002=003\r\n"
        "[Stop001]\r\nName=Principal 8\r\nNumberOfLogicalPipes=3\r\nNumberOfAccessiblePipes=3\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nPipe001=P8\\036.wav\r\nPipe002=P8\\036.wav\r\nPipe003=P8\\036.wav\r\n"
        "[Stop002]\r\nName=Gedackt 8\r\nNumberOfLogicalPipes=3\r\nNumberOfAccessiblePipes=3\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nPipe001=G8\\036.wav\r\nPipe002=G8\\036.wav\r\nPipe003=G8\\036.wav\r\n"
        "[Stop003]\r\nName=Trompete 8\r\nNumberOfLogicalPipes=3\r\nNumberOfAccessiblePipes=3\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nWindchestGroup=001\r\nPipe001=T8\\036.wav\r\nPipe002=T8\\036.wav\r\nPipe003=T8\\036.wav\r\n"
        "[Coupler001]\r\nName=II/I\r\nDestinationManual=2\r\n";
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-console-profile");
    root.deleteRecursively();
    root.createDirectory();
    const juce::File before = mp::dataFolder();
    mp::setDataFolder(root.getChildFile("data"));
    for (const char* rank : {"P8", "G8", "T8"}) {
      const juce::File wav = root.getChildFile(rank).getChildFile("036.wav");
      wav.getParentDirectory().createDirectory();
      juce::WavAudioFormat fmt;
      std::unique_ptr<juce::FileOutputStream> os(wav.createOutputStream());
      std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, {}, 0));
      juce::AudioBuffer<float> tone(1, 4800);
      for (int i = 0; i < 4800; ++i)
        tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 65.41 * i / 48000.0)));
      w->writeFromAudioSampleBuffer(tone, 0, 4800);
    }
    const juce::File file = root.getChildFile("profile.organ");
    file.replaceWithText(juce::String::fromUTF8(organ.c_str()));
    {
      mp::MasterpieceProcessor proc;
      MP_CHECK(proc.loadOrgan(file, 0, false).ok, "the organ loads");
      proc.prepareToPlay(48000.0, 256);
      const int console = proc.registerMidiDevice("Console");
      juce::AudioBuffer<float> buf(2, 256);
      auto press = [&](int number) {
        proc.pushMidi(console, juce::MidiMessage::controllerEvent(1, number, 127));
        juce::MidiBuffer m;
        buf.clear();
        proc.processBlock(buf, m);
      };

      const auto controls = mp::ui::organControls(proc);
      auto find = [&](const char* name) -> const mp::ui::OrganControl* {
        for (const auto& c : controls)
          if (c.tab.label.find(name) != std::string::npos) return &c;
        return nullptr;
      };
      const auto* principal = find("Principal");
      const auto* gedackt = find("Gedackt");
      const auto* trompete = find("Trompete");
      const auto* coupler = find("II/I");
      MP_CHECK(controls.size() == 4, "three stops and a coupler, got " + std::to_string(controls.size()));
      if (principal == nullptr || gedackt == nullptr || trompete == nullptr || coupler == nullptr) return;
      MP_CHECK(principal->tab.keyboard == 1 && gedackt->tab.keyboard == 2, "the stops know their keyboards");
      MP_CHECK(coupler->tab.kind == mp::ConsoleTabKind::Coupler && coupler->tab.keyboard == 1 &&
                   coupler->tab.fromKeyboard == 2,
               "the coupler joins II onto I, got onto " + std::to_string(coupler->tab.keyboard) + " from " +
                   std::to_string(coupler->tab.fromKeyboard));

      // The player's own mapping for the Trompete.
      proc.setObjectBindings(mp::MidiTargetKind::Switch, trompete->tab.switchId,
                             {cc(40, console, trompete->tab.switchId)});

      // The player's console, in English, sending controllers 20 to 24.
      mp::ConsoleProfile profile;
      profile.name = "My console";
      auto tab = [](mp::ConsoleTabKind kind, const char* label, int keyboard, int from = -1) {
        mp::ConsoleTab t;
        t.kind = kind;
        t.label = label;
        t.keyboard = keyboard;
        t.fromKeyboard = from;
        return t;
      };
      profile.tabs = {tab(mp::ConsoleTabKind::Stop, "Open Diapason 8", 1),
                      tab(mp::ConsoleTabKind::Stop, "Stopped Diapason 8", 2),
                      tab(mp::ConsoleTabKind::Stop, "Trumpet 8", 2),
                      tab(mp::ConsoleTabKind::Coupler, "Swell to Great", 1, 2),
                      tab(mp::ConsoleTabKind::Stop, "Bourdon 16", 2)};
      const int dev = profile.messages.devices().claim("Console");
      for (size_t i = 0; i < profile.tabs.size(); ++i)
        profile.messages.bind(cc(20 + static_cast<int>(i), dev, mp::ConsoleProfile::targetOf(i)));
      const juce::File profileFile = mp::ui::newConsoleProfileFile("My console");
      MP_CHECK(mp::ui::saveConsoleProfile(profileFile, profile), "the profile is saved");
      mp::ui::setActiveConsoleProfile(profileFile);
      MP_CHECK(mp::ui::activeConsoleProfile() == profileFile, "the profile is the one in use");

      mp::ui::ProfileMatchPanel panel(proc);
      panel.setSize(600, 400);
      MP_CHECK(panel.refresh(), "the panel reads the profile");
      std::string offered;
      for (int i = 0; i < panel.matchCount(); ++i)
        offered += (panel.tabText(i) + " -> " + panel.controlText(i)).toStdString() +
                   (panel.match(i).strong ? " strong|" : " weak|");
      bool trumpetOffered = false, bourdonOffered = false;
      for (int i = 0; i < panel.matchCount(); ++i) {
        trumpetOffered |= panel.match(i).tab == 2;
        bourdonOffered |= panel.match(i).tab == 4;
      }
      MP_CHECK(!trumpetOffered, "the Trompete has the player's own mapping: not offered, got " + offered);
      MP_CHECK(!bourdonOffered, "a Bourdon 16 never goes to an 8', got " + offered);

      const int mapped = panel.applyStrong();
      MP_CHECK(mapped == 3, "three strong matches mapped, got " + std::to_string(mapped) + " from " + offered);
      auto number = [&](mp::Id sw) {
        const auto b = proc.midiMap().bindingsFor(mp::MidiTargetKind::Switch, sw);
        return b.size() == 1 ? b.front()->source.number : -static_cast<int>(b.size());
      };
      MP_CHECK(number(principal->tab.switchId) == 20, "Open Diapason to the Principal");
      MP_CHECK(number(gedackt->tab.switchId) == 21, "Stopped Diapason to the Gedackt");
      MP_CHECK(number(coupler->tab.switchId) == 23, "Swell to Great to II/I");
      MP_CHECK(number(trompete->tab.switchId) == 40, "the Trompete keeps the player's own mapping");
      MP_CHECK(panel.matchCount() == 0, "nothing left to offer, got " + std::to_string(panel.matchCount()));

      press(21);
      MP_CHECK(proc.shownSwitchEngaged(gedackt->tab.switchId), "the console's CC 21 draws the Gedackt");
      press(23);
      MP_CHECK(proc.shownSwitchEngaged(coupler->tab.switchId), "CC 23 draws the coupler");

      // The organ keeps the stamp it was mapped from.
      mp::ConsoleProfile reread;
      MP_CHECK(mp::ui::loadConsoleProfile(profileFile, reread), "the profile reads back");
      const auto stamp = mp::ui::profileStampFor(profileFile, reread);
      mp::ui::setOrganProfileStamp(proc, stamp);
      MP_CHECK(mp::ui::organProfileStamp(proc) == stamp, "the organ remembers the profile's stamp");

      // An organ mapped by hand made into a profile.
      mp::ConsoleProfile made;
      const int added = mp::ui::addOrganToProfile(proc, made);
      MP_CHECK(added == 4, "every mapped control becomes a tab, got " + std::to_string(added));
      bool trompeteTab = false;
      for (size_t i = 0; i < made.tabs.size(); ++i)
        if (made.tabs[i].label.find("Trompete") != std::string::npos) {
          const auto rows = made.bindingsOf(i);
          trompeteTab = rows.size() == 1 && rows.front().source.number == 40 &&
                        made.messages.devices().nameFor(rows.front().source.deviceId) == "Console";
        }
      MP_CHECK(trompeteTab, "the Trompete tab carries CC 40 from the Console");

      // The profile window: a new profile from this organ, in use at once.
      mp::ui::ConsoleProfilePanel window(proc);
      window.setSize(700, 500);
      MP_CHECK(window.newProfile("Second", true), "a new profile");
      MP_CHECK(window.file().existsAsFile() && mp::ui::activeConsoleProfile() == window.file(),
               "the new profile is saved and in use");
      MP_CHECK(window.profile().tabs.size() == 4, "with this organ's four mapped controls, got " +
                                                      std::to_string(window.profile().tabs.size()));
      window.addTab();
      MP_CHECK(window.profile().tabs.size() == 5, "a tab added");
      window.setLabel(4, "Sw-Gt 16");
      window.setKind(4, mp::ConsoleTabKind::Coupler);
      window.setLabel(4, "Sw-Gt 16");
      MP_CHECK(window.profile().tabs[4].shift == -12, "a coupler named 16 sounds an octave down, got " +
                                                          std::to_string(window.profile().tabs[4].shift));
      window.removeTab(4);
      MP_CHECK(window.profile().tabs.size() == 4, "and removed");
      mp::ConsoleProfile onDisk;
      MP_CHECK(mp::ui::loadConsoleProfile(window.file(), onDisk) && onDisk.tabs.size() == 4,
               "the window saves each change");
    }
    mp::setDataFolder(before);
    root.deleteRecursively();
  }
};

static ConsoleProfileAppTest g_consoleProfileApp;

}  // namespace

#endif
