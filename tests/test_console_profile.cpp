// Console profiles (#866): a console's tabs, saved once, matched onto any
// organ by family, footage and keyboard. JUCE-free: the fast loop runs it.
#include "MpTest.h"

#include "../src/mp_control/ConsoleProfile.h"

#include <cmath>
#include <string>
#include <vector>

namespace {

using mp::test::Category;
using mp::ConsoleTab;
using mp::ConsoleTabKind;
using mp::ToneFamily;

ConsoleTab stop(const std::string& label, int keyboard) {
  ConsoleTab t;
  t.label = label;
  t.keyboard = keyboard;
  return t;
}

ConsoleTab coupler(const std::string& label, int from, int onto, int shift = 0) {
  ConsoleTab t;
  t.kind = ConsoleTabKind::Coupler;
  t.label = label;
  t.fromKeyboard = from;
  t.keyboard = onto;
  t.shift = shift;
  return t;
}

ConsoleTab tremulant(const std::string& label, int keyboard) {
  ConsoleTab t;
  t.kind = ConsoleTabKind::Tremulant;
  t.label = label;
  t.keyboard = keyboard;
  return t;
}

class StopNamesTest final : public mp::test::Test {
public:
  StopNamesTest() : Test("functional.profile.stop-names", Category::Functional) {}
  void run() override {
    struct Case {
      const char* name;
      ToneFamily family;
      double footage;
      int ranks;
    };
    const Case cases[] = {
        {"Bourdon 8'", ToneFamily::StoppedFlute, 8, 0},
        {"Gedackt 8", ToneFamily::StoppedFlute, 8, 0},
        {"Bordone 8", ToneFamily::StoppedFlute, 8, 0},
        {"Stopped Diapason 8", ToneFamily::StoppedFlute, 8, 0},
        {"Rohrfl\xc3\xb6te 4'", ToneFamily::StoppedFlute, 4, 0},
        {"Quintadena 8", ToneFamily::StoppedFlute, 8, 0},
        {"Subbass 16'", ToneFamily::StoppedFlute, 16, 0},
        {"Principal 8", ToneFamily::Principal, 8, 0},
        {"Prestant 4", ToneFamily::Principal, 4, 0},
        {"Montre 8", ToneFamily::Principal, 8, 0},
        {"Octave 4'", ToneFamily::Principal, 4, 0},
        {"Flautado 8", ToneFamily::Principal, 8, 0},
        {"Open Diapason 8", ToneFamily::Principal, 8, 0},
        {"Spitzfl\xc3\xb6te 4", ToneFamily::OpenFlute, 4, 0},
        {"Fl\xc3\xbbte harmonique 8", ToneFamily::OpenFlute, 8, 0},
        {"Salicional 8", ToneFamily::String, 8, 0},
        {"Viola da gamba 8", ToneFamily::String, 8, 0},
        {"Dulciana 8", ToneFamily::String, 8, 0},
        {"Voix c\xc3\xa9leste 8", ToneFamily::Celeste, 8, 0},
        {"Unda maris 8", ToneFamily::Celeste, 8, 0},
        {"Quinte 2 2/3", ToneFamily::Mutation, 2.0 + 2.0 / 3, 0},
        {"Nasard 2\xe2\x85\x94", ToneFamily::Mutation, 2.0 + 2.0 / 3, 0},
        {"Larigot 1 1/3'", ToneFamily::Mutation, 1.0 + 1.0 / 3, 0},
        {"Tierce 1 3/5", ToneFamily::Mutation, 1.6, 0},
        {"Mixtur IV", ToneFamily::Mixture, 0, 4},
        {"Mixtur 4 fach 2'", ToneFamily::Mixture, 2, 4},
        {"Fourniture V", ToneFamily::Mixture, 0, 5},
        {"Sesquialtera II", ToneFamily::Cornet, 0, 2},
        {"Trompette 8", ToneFamily::ChorusReed, 8, 0},
        {"Posaune 16", ToneFamily::ChorusReed, 16, 0},
        {"Hautbois 8", ToneFamily::SoloReed, 8, 0},
        {"Krummhorn 8", ToneFamily::SoloReed, 8, 0},
        {"Vox humana 8", ToneFamily::Regal, 8, 0},
    };
    for (const auto& c : cases) {
      const auto t = mp::stopTraits(c.name);
      MP_CHECK(t.family == c.family, std::string(c.name) + ": family " + mp::toneFamilyName(t.family) +
                                         ", expected " + mp::toneFamilyName(c.family));
      MP_CHECK(std::abs(t.footage - c.footage) < 0.01,
               std::string(c.name) + ": footage " + std::to_string(t.footage));
      MP_CHECK(t.ranks == c.ranks, std::string(c.name) + ": ranks " + std::to_string(t.ranks));
    }
  }
};

class ProfileMatchTest final : public mp::test::Test {
public:
  ProfileMatchTest() : Test("functional.profile.match", Category::Functional) {}
  void run() override {
    // An English console: Pedal 0, Great 1, Swell 2, and a Choir the organ
    // has not got.
    const std::vector<ConsoleTab> tabs{
        stop("Bourdon 8", 1),          // 0
        stop("Principal 8", 1),        // 1
        stop("Octave 4", 1),           // 2
        stop("Mixture IV", 1),         // 3
        stop("Flute 8", 1),            // 4
        stop("Voix celeste 8", 2),     // 5
        stop("Trumpet 8", 2),          // 6
        stop("Gamba", 2),              // 7: no footage printed
        stop("Subbass 16", 0),         // 8
        coupler("Swell to Great", 2, 1),  // 9
        tremulant("Tremulant", 2),     // 10
        stop("Cymbale III", 3),        // 11: no third manual on the organ
    };
    // A German organ.
    const std::vector<ConsoleTab> controls{
        stop("Prinzipal 8'", 1),         // 0
        stop("Bordun 16'", 1),           // 1
        stop("Gedackt 8'", 1),           // 2
        stop("Oktave 4'", 1),            // 3
        stop("Mixtur 4fach", 1),         // 4
        stop("Hohlfl\xc3\xb6te 8'", 1),  // 5
        stop("Unda maris 8'", 2),        // 6
        stop("Trompete 8'", 2),          // 7
        stop("Oboe 8'", 2),              // 8
        stop("Viola 8'", 2),             // 9
        stop("Subbass 16'", 0),          // 10
        coupler("II/I", 2, 1),           // 11
        tremulant("Tremulant", 2),       // 12
    };
    const auto matches = mp::matchProfile(tabs, controls);
    auto to = [&](size_t tab) -> int {
      for (const auto& m : matches)
        if (m.tab == tab) return static_cast<int>(m.control);
      return -1;
    };
    auto strong = [&](size_t tab) {
      for (const auto& m : matches)
        if (m.tab == tab) return m.strong;
      return false;
    };
    const int expect[] = {2, 0, 3, 4, 5, 6, 7, 9, 10, 11, 12, -1};
    for (size_t t = 0; t < tabs.size(); ++t)
      MP_CHECK(to(t) == expect[t], tabs[t].label + " goes to control " + std::to_string(expect[t]) + ", got " +
                                       std::to_string(to(t)));
    for (size_t t : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u, 9u, 10u})
      MP_CHECK(strong(t), tabs[t].label + " is a strong match");
    MP_CHECK(!strong(7), "a tab with no footage printed is a weak match, for the player to confirm");
    // Each control once.
    std::vector<int> used(controls.size(), 0);
    for (const auto& m : matches) ++used[m.control];
    for (size_t c = 0; c < used.size(); ++c) MP_CHECK(used[c] <= 1, "control " + std::to_string(c) + " used once");

    // The last pass, by sound: a Diapason with no principal left is offered
    // the Gedackt at its pitch, for the player to confirm; a Trumpet with no
    // reed left is offered nothing.
    {
      const auto near = mp::matchProfile({stop("Open Diapason 8", 1), stop("Trumpet 8", 1)},
                                         {stop("Gedackt 8'", 1), stop("Rohrfl\xc3\xb6te 4'", 1)});
      MP_CHECK(near.size() == 1 && near[0].tab == 0 && near[0].control == 0,
               "the Diapason is offered the Gedackt, got " + std::to_string(near.size()) + " matches");
      MP_CHECK(!near.empty() && near[0].related && !near[0].strong, "as a related match, never a strong one");
      MP_CHECK(mp::matchProfile({stop("Open Diapason 8", 1)}, {stop("Gedackt 4'", 1)}).empty(),
               "never at another footage");
    }

    // Never across footages, keyboards or kinds.
    MP_CHECK(mp::matchScore(stop("Bourdon 8", 1), stop("Bourdon 16", 1)) == 0.0, "a Bourdon 8 never goes to a 16'");
    MP_CHECK(mp::matchScore(stop("Bourdon 8", 1), stop("Bourdon 8", 2)) == 0.0, "nor to another keyboard");
    MP_CHECK(mp::matchScore(coupler("II/I", 2, 1), coupler("II/I 16", 2, 1, -12)) == 0.0,
             "a coupler at another pitch is another coupler");
    MP_CHECK(mp::matchScore(coupler("II/I", 2, 1), coupler("I/P", 1, 0)) == 0.0, "nor between other keyboards");
    MP_CHECK(mp::matchScore(stop("Tremulant", 2), tremulant("Tremulant", 2)) == 0.0, "a stop is not a tremulant");
  }
};

class OrganKeyboardsTest final : public mp::test::Test {
public:
  OrganKeyboardsTest() : Test("functional.profile.organ-keyboards", Category::Functional) {}
  void run() override {
    // Hauptwerk's way: internal keyboards carry the channel convention.
    mp::OrganModel m;
    auto division = [&](mp::Id id, const char* name) {
      mp::Division d;
      d.divisionId = id;
      d.name = name;
      m.divisions[id] = d;
    };
    auto keyboard = [&](mp::Id id, int code, mp::Id div) {
      mp::Keyboard k;
      k.keyboardId = id;
      k.assignmentCode = code;
      k.primaryDivisionHint = div;
      m.keyboards[id] = k;
    };
    division(1, "Pedaal");
    division(2, "Hoofdwerk");
    division(3, "Zwelwerk");
    keyboard(11, 1, 1);
    keyboard(12, 2, 2);
    keyboard(13, 3, 3);
    MP_CHECK(mp::divisionKeyboard(m, 1) == 0, "the pedal is keyboard 0");
    MP_CHECK(mp::divisionKeyboard(m, 2) == 1 && mp::divisionKeyboard(m, 3) == 2, "the manuals by channel");
    // Zwelwerk to Hoofdwerk, an octave up, enabled through a linkage.
    mp::KeyAction a;
    a.sourceKeyboard = 12;
    a.destKeyboard = 13;
    a.destIsKeyboard = true;
    a.conditionSwitchId = 501;
    a.midiIncrement = 12;
    m.keyActions.push_back(a);
    mp::SwitchLinkage l;
    l.sourceSwitchId = 500;
    l.destSwitchId = 501;
    m.switchLinkages.push_back(l);
    int onto = -1, from = -1, shift = 0;
    MP_CHECK(mp::couplerKeyboards(m, 500, onto, from, shift), "the coupler's key action is found");
    MP_CHECK(onto == 1 && from == 2 && shift == 12,
             "II onto I at 4': " + std::to_string(from) + " onto " + std::to_string(onto) + " shift " +
                 std::to_string(shift));

    // A name says the same.
    const std::vector<std::pair<std::string, int>> divs{{"Pedal", 0}, {"Great", 1}, {"Swell", 2}};
    struct Case {
      const char* name;
      int from, onto, shift;
    };
    const Case cases[] = {{"Swell to Great", 2, 1, 0}, {"II/I", 2, 1, 0},       {"I/P", 1, 0, 0},
                          {"Sw-Gt 16", 2, 1, -12},     {"Swell Octave", 2, 2, 12}, {"Great to Pedal", 1, 0, 0}};
    for (const auto& c : cases) {
      onto = from = -1;
      shift = 0;
      const bool ok = mp::couplerKeyboardsFromName(c.name, divs, onto, from, shift);
      MP_CHECK(ok && onto == c.onto && from == c.from && shift == c.shift,
               std::string(c.name) + ": " + std::to_string(from) + " onto " + std::to_string(onto) + " shift " +
                   std::to_string(shift));
    }
    MP_CHECK(mp::keyboardFromName("Tremulant Swell", divs) == 2, "a tremulant named for its division");
    MP_CHECK(mp::keyboardFromName("Tremulant II", divs) == 2, "or numbered");
  }
};

class ProfileFileTest final : public mp::test::Test {
public:
  ProfileFileTest() : Test("functional.profile.file", Category::Functional) {}
  void run() override {
    mp::ConsoleProfile p;
    p.name = "Home console";
    p.tabs = {stop("Bourdon 8", 1), coupler("Swell to Great", 2, 1), tremulant("Tremulant", 2)};
    const int dev = p.messages.devices().claim("My Console");
    auto cc = [&](int n) {
      mp::MidiBinding b;
      b.source.kind = mp::MidiSourceKind::ControlChange;
      b.source.channel = 1;
      b.source.number = n;
      b.source.deviceId = dev;
      b.trigger = mp::MidiTrigger::Toggle;
      return b;
    };
    for (size_t t = 0; t < p.tabs.size(); ++t) p.setBindingsOf(t, {cc(20 + static_cast<int>(t))});

    mp::ConsoleProfile back;
    MP_CHECK(back.fromText(p.toText()), "the profile reads back");
    MP_CHECK(back.name == p.name, "its name");
    MP_CHECK(back.tabs == p.tabs, "its tabs");
    for (size_t t = 0; t < p.tabs.size(); ++t) {
      const auto b = back.bindingsOf(t);
      MP_CHECK(b.size() == 1 && b[0].source.number == 20 + static_cast<int>(t), "tab " + std::to_string(t) + "'s message");
      MP_CHECK(!b.empty() && back.messages.devices().nameFor(b[0].source.deviceId) == "My Console", "and its device");
    }
    MP_CHECK(back.stamp() == p.stamp(), "the same profile, the same stamp");
    back.tabs[0].label = "Bourdon 8'";
    MP_CHECK(back.stamp() != p.stamp(), "a changed tab, another stamp");

    // A tab taken out: the later tabs keep their messages.
    p.removeTab(0);
    MP_CHECK(p.tabs.size() == 2 && p.tabs[0].label == "Swell to Great", "the tab is gone");
    MP_CHECK(p.bindingsOf(0).size() == 1 && p.bindingsOf(0)[0].source.number == 21, "the next one keeps CC 21");
    MP_CHECK(p.bindingsOf(1).size() == 1 && p.bindingsOf(1)[0].source.number == 22, "and the last CC 22");
    MP_CHECK(p.bindingsOf(2).empty(), "nothing left over");
  }
};

StopNamesTest g_stopNames;
ProfileMatchTest g_profileMatch;
OrganKeyboardsTest g_organKeyboards;
ProfileFileTest g_profileFile;

}  // namespace
