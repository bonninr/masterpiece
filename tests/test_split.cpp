// The split keyboard (#358): zones, layers, bass from the hands, a split that
// follows the hands, presets and learned controls, and the files. The module
// is JUCE-free, so all of it runs in the fast loop.
#include <string>
#include <vector>

#include "MpTest.h"
#include "mp_control/MidiMap.h"
#include "mp_split/SplitKeyboard.h"
#include "mp_split/SplitText.h"

namespace {

using mp::test::Category;
using mp::split::Layout;
using mp::split::Role;
using mp::split::SplitKeyboard;

constexpr int kSource = 2, kOwn = 9;

// One note as the split sent it: channel, note, on.
struct Out {
  int channel, note;
  bool on;
  bool operator==(const Out&) const = default;
};

std::string show(const std::vector<Out>& v) {
  std::string s;
  for (const auto& o : v)
    s += (s.empty() ? "" : " ") + std::string(o.on ? "on" : "off") + "(" + std::to_string(o.channel) + "," +
         std::to_string(o.note) + ")";
  return s.empty() ? "nothing" : s;
}

std::vector<Out> drain(SplitKeyboard& sk) {
  int n = 0;
  const auto* m = sk.output(n);
  std::vector<Out> v;
  for (int i = 0; i < n; ++i)
    v.push_back({(m[i].status & 0x0F) + 1, m[i].data1, (m[i].status & 0xF0) == 0x90 && m[i].data2 > 0});
  sk.clearOutput();
  return v;
}

struct Rig {
  SplitKeyboard sk;
  double now = 0.0;
  bool taken = false;

  explicit Rig(const Layout& l, bool on = true) {
    sk.setDevices(kSource, kOwn);
    sk.setLayout(l);
    sk.setEnabled(on);
    block();
  }
  std::vector<Out> block(double ms = 10.0) {
    now += ms;
    sk.beginBlock(now);
    sk.endBlock();
    return drain(sk);
  }
  std::vector<Out> send(uint8_t status, int d1, int d2, int device = kSource) {
    const uint8_t data[3] = {status, static_cast<uint8_t>(d1), static_cast<uint8_t>(d2)};
    sk.clearOutput();
    taken = sk.feed(device, data, 3);
    return drain(sk);
  }
  std::vector<Out> on(int note, int device = kSource) { return send(0x90, note, 100, device); }
  std::vector<Out> off(int note, int device = kSource) { return send(0x80, note, 0, device); }
};

const int kLower = mp::split::channelOf(Role::Lower), kUpper = mp::split::channelOf(Role::Upper),
          kSolo = mp::split::channelOf(Role::Solo), kPedal = mp::split::channelOf(Role::Pedal);

Layout twoManuals() {
  Layout l = mp::split::factoryLayout(2);
  return l;
}

class SplitZonesTest final : public mp::test::Test {
public:
  SplitZonesTest() : Test("functional.split.zones", Category::Functional) {}
  void run() override {
    Layout l = twoManuals();
    l.zones[1].octave = -1;
    l.zones[1].transpose = 2;
    l.transpose = 1;
    Rig r(l);
    auto o = r.on(50);
    MP_CHECK(r.taken && o == std::vector<Out>({{kLower, 51, true}}),
             "below the split: the lower manual, with the global transposition: " + show(o));
    o = r.on(70);
    MP_CHECK(o == std::vector<Out>({{kUpper, 70 - 12 + 2 + 1, true}}),
             "above it: the upper manual, an octave down, two semitones up and one more: " + show(o));
    o = r.off(70);
    MP_CHECK(o == std::vector<Out>({{kUpper, 61, false}}), "the release goes where the press went: " + show(o));
    o = r.off(50);
    MP_CHECK(o == std::vector<Out>({{kLower, 51, false}}), show(o));
  }
};
static SplitZonesTest g_splitZones;

class SplitLayersTest final : public mp::test::Test {
public:
  SplitLayersTest() : Test("functional.split.layers-and-shared-notes", Category::Functional) {}
  void run() override {
    Layout l = twoManuals();
    l.zones[1].layer = Role::Solo;
    Rig r(l);
    auto o = r.on(72);
    MP_CHECK(o == std::vector<Out>({{kUpper, 72, true}, {kSolo, 72, true}}), "a layer plays two roles: " + show(o));
    o = r.off(72);
    MP_CHECK(o == std::vector<Out>({{kUpper, 72, false}, {kSolo, 72, false}}), show(o));

    // Two keys sending the same note to one role: one note-on, and the note
    // ends with the last key.
    l = twoManuals();
    l.zones[0].octave = 1;
    l.zones[1].role = Role::Lower;
    Rig s(l);
    o = s.on(48);
    MP_CHECK(o == std::vector<Out>({{kLower, 60, true}}), show(o));
    o = s.on(60);
    MP_CHECK(o.empty(), "already sounding: " + show(o));
    o = s.off(48);
    MP_CHECK(o.empty(), "still held by the other key: " + show(o));
    o = s.off(60);
    MP_CHECK(o == std::vector<Out>({{kLower, 60, false}}), show(o));
  }
};
static SplitLayersTest g_splitLayers;

class SplitBassTest final : public mp::test::Test {
public:
  SplitBassTest() : Test("functional.split.bass-from-the-hands", Category::Functional) {}
  void run() override {
    Rig r(mp::split::factoryLayout(1));  // two manuals and bass, up to B3
    auto o = r.on(48);
    MP_CHECK(o == std::vector<Out>({{kLower, 48, true}, {kPedal, 48, true}}),
             "the lowest left-hand key plays the pedal too: " + show(o));
    o = r.on(55);
    MP_CHECK(o == std::vector<Out>({{kLower, 55, true}}), "a key above it does not: " + show(o));
    o = r.on(72);
    MP_CHECK(o == std::vector<Out>({{kUpper, 72, true}}), "nor a right-hand key: " + show(o));
    o = r.on(45);
    MP_CHECK(o == std::vector<Out>({{kLower, 45, true}, {kPedal, 48, false}, {kPedal, 45, true}}),
             "a lower key takes the pedal, one note at a time: " + show(o));
    o = r.off(45);
    MP_CHECK(o == std::vector<Out>({{kLower, 45, false}, {kPedal, 45, false}, {kPedal, 48, true}}),
             "and gives it back to the lowest still held: " + show(o));
    o = r.off(55);
    MP_CHECK(o == std::vector<Out>({{kLower, 55, false}}), "the lowest unchanged, the pedal is not struck: " + show(o));
    o = r.off(48);
    MP_CHECK(o == std::vector<Out>({{kLower, 48, false}, {kPedal, 48, false}}), "no sustain: it ends: " + show(o));

    // Only in the pedal.
    Layout l = mp::split::factoryLayout(1);
    l.bass.alsoOnManual = false;
    Rig p(l);
    o = p.on(48);
    MP_CHECK(o == std::vector<Out>({{kPedal, 48, true}}), "the bass key plays only the pedal: " + show(o));
    o = p.on(52);
    MP_CHECK(o == std::vector<Out>({{kLower, 52, true}}), "the keys above it play the manual: " + show(o));
    o = p.on(43);
    MP_CHECK(o == std::vector<Out>({{kLower, 48, true}, {kPedal, 48, false}, {kPedal, 43, true}}),
             "a lower bass: the former bass key, still held, plays its manual: " + show(o));
  }
};
static SplitBassTest g_splitBass;

class SplitSustainTest final : public mp::test::Test {
public:
  SplitSustainTest() : Test("functional.split.pedal-sustain", Category::Functional) {}
  void run() override {
    Layout l = mp::split::factoryLayout(1);
    l.bass.sustain = mp::split::Sustain::UntilNext;
    Rig r(l);
    r.on(48);
    auto o = r.off(48);
    MP_CHECK(o == std::vector<Out>({{kLower, 48, false}}), "the pedal holds after the hand lifts: " + show(o));
    o = r.block(5000);
    MP_CHECK(o.empty(), "for as long as it takes: " + show(o));
    o = r.on(50);
    MP_CHECK(o == std::vector<Out>({{kLower, 50, true}, {kPedal, 48, false}, {kPedal, 50, true}}),
             "until the next bass note: " + show(o));

    l.bass.sustain = mp::split::Sustain::Timed;
    l.bass.sustainMs = 1500;
    Rig t(l);
    t.on(48);
    t.off(48);
    o = t.block(1000);
    MP_CHECK(o.empty(), "a timed sustain holds within its time: " + show(o));
    o = t.block(600);
    MP_CHECK(o == std::vector<Out>({{kPedal, 48, false}}), "and ends after it: " + show(o));
  }
};
static SplitSustainTest g_splitSustain;

class SplitFollowTest final : public mp::test::Test {
public:
  SplitFollowTest() : Test("functional.split.follows-the-hands", Category::Functional) {}
  void run() override {
    Layout l = twoManuals();  // split at 60
    l.followHands = true;
    Rig r(l);
    r.on(60);
    r.on(64);
    auto o = r.on(80);
    MP_CHECK(o == std::vector<Out>({{kUpper, 80, true}}), show(o));
    // The gap 64..80 moved the split to 73: 66 is now a left-hand key.
    o = r.on(66);
    MP_CHECK(o == std::vector<Out>({{kLower, 66, true}}), "a key in the gap goes to the left hand: " + show(o));
    o = r.off(60);
    MP_CHECK(o == std::vector<Out>({{kUpper, 60, false}}),
             "a key keeps the zone it was pressed in, before the split moved: " + show(o));
    r.off(64);
    r.off(66);
    r.off(80);
    r.block(1000);
    o = r.on(70);
    MP_CHECK(o == std::vector<Out>({{kLower, 70, true}}), "the moved split stays after the release: " + show(o));
    r.off(70);
    r.block(3500);
    o = r.on(70);
    MP_CHECK(o == std::vector<Out>({{kUpper, 70, true}}), "and returns a few seconds later: " + show(o));
  }
};
static SplitFollowTest g_splitFollow;

class SplitOnOffTest final : public mp::test::Test {
public:
  SplitOnOffTest() : Test("functional.split.on-off-and-inputs", Category::Functional) {}
  void run() override {
    Rig r(twoManuals(), false);
    auto o = r.on(50);
    MP_CHECK(!r.taken && o.empty(), "off: the keyboard plays as it is");
    r.sk.setEnabled(true);
    r.block();
    o = r.off(50);
    MP_CHECK(!r.taken, "a key pressed before the split came on is released the way it was pressed");
    o = r.on(50);
    MP_CHECK(r.taken && o == std::vector<Out>({{kLower, 50, true}}), show(o));
    r.on(50, 3);
    MP_CHECK(!r.taken, "another input is left alone");
    r.on(50, 0);
    MP_CHECK(!r.taken, "the host and a file playing are never split");
    r.on(50, kOwn);
    MP_CHECK(!r.taken, "nor the split's own input");
    r.send(0xB0, 7, 100);
    MP_CHECK(!r.taken, "the keyboard's controllers pass through");
    r.on(72);
    r.sk.setEnabled(false);
    o = r.block();
    MP_CHECK(o == std::vector<Out>({{kLower, 50, false}, {kUpper, 72, false}}),
             "switched off: everything it sounded is released: " + show(o));
  }
};
static SplitOnOffTest g_splitOnOff;

class SplitPresetsTest final : public mp::test::Test {
public:
  SplitPresetsTest() : Test("functional.split.presets-and-controls", Category::Functional) {}
  void run() override {
    Rig r(twoManuals());
    SplitKeyboard::Presets p;
    p.layouts[4] = twoManuals();
    p.layouts[4].splits[0] = 48;
    p.used[4] = true;
    r.sk.setPresets(p);
    r.sk.learn(static_cast<int>(mp::split::Control::Next));
    r.block();
    auto o = r.on(96, 5);
    MP_CHECK(r.taken && o.empty(), "a press on any input is learned and plays nothing");
    r.off(96, 5);
    MP_CHECK(r.taken, "nor does its release");

    r.on(50);
    o = r.on(96, 5);
    MP_CHECK(o == std::vector<Out>({{kLower, 50, false}, {kPedal, 50, true}}),
             "next: preset 1, two manuals and a pedal zone; the held key sounds in its new place: " + show(o));
    r.off(50);
    r.on(96, 5);
    r.on(96, 5);
    r.on(96, 5);
    r.block();
    mp::split::State st;
    MP_CHECK(r.sk.state(st) && st.preset == 4, "next skips the empty slot 4 to the player's preset 5: " +
                                                    std::to_string(st.preset));
    o = r.on(50);
    MP_CHECK(o == std::vector<Out>({{kUpper, 50, true}}), "and plays it: the split at 48: " + show(o));
    r.on(96, 5);
    r.block();
    MP_CHECK(r.sk.state(st) && st.preset == 0, "and wraps to the first");
    MP_CHECK(st.layoutChanges == 5, "five changes made from MIDI, to be saved: " + std::to_string(st.layoutChanges));

    r.sk.learn(static_cast<int>(mp::split::Control::Toggle));
    r.block();
    r.send(0xB0, 64, 127, 5);
    o = r.send(0xB0, 64, 127, 5);
    MP_CHECK(r.taken && !r.sk.enabled(), "a learned controller switches the split off");
  }
};
static SplitPresetsTest g_splitPresets;

class SplitTextTest final : public mp::test::Test {
public:
  SplitTextTest() : Test("functional.split.files", Category::Functional) {}
  void run() override {
    Layout l = mp::split::factoryLayout(0);
    l.zones[1].layer = Role::Solo;
    l.zones[2].transpose = -3;
    l.transpose = 2;
    l.followHands = true;
    l.bass = {true, 57, false, mp::split::Sustain::Timed, 900};
    Layout back;
    int preset = -1;
    mp::split::organFromText(mp::split::organToText(l, 2), back, preset);
    MP_CHECK(back == l && preset == 2, "an organ's layout reads back whole:\n" + mp::split::organToText(l, 2));

    mp::split::Shared s;
    s.on = true;
    s.source = "Keystation 61 MK3";
    s.presets.layouts[5] = l;
    s.presets.used[5] = true;
    s.names[5] = "Trio, left hand bass";
    s.triggers[1] = {mp::split::Trigger::Note, 1, 96, 7};
    const auto text = mp::split::sharedToText(s, [](int id) { return id == 7 ? std::string("Pedal Unit") : ""; });
    const auto r = mp::split::sharedFromText(text, [](const std::string& n) { return n == "Pedal Unit" ? 3 : 0; });
    MP_CHECK(r.on && r.source == s.source && r.presets.used[5] && r.presets.layouts[5] == l &&
                 r.names[5] == s.names[5] && !r.presets.used[6],
             "the shared presets read back:\n" + text);
    MP_CHECK(r.triggers[1].kind == mp::split::Trigger::Note && r.triggers[1].number == 96 && r.triggers[1].device == 3,
             "a control keeps its input by name");
    MP_CHECK(r.presets.used[0] && r.presets.layouts[0] == mp::split::factoryLayout(0), "the factory presets are there");

    Layout odd;
    mp::split::layoutFromText("splits 3 90 40 40\nzone 1 upper upper 9 -40\nfuture-line 1 2 3\n", odd);
    MP_CHECK(odd.splits[0] == 40 && odd.splits[1] == 41 && odd.splits[2] == 90 && odd.zones[1].layer == Role::None &&
                 odd.zones[1].octave == 3 && odd.zones[1].transpose == -12,
             "a file out of range is brought into it");
  }
};
static SplitTextTest g_splitText;

// The split's input is taken only by mappings that name it: a manual mapped
// from any input on any channel would otherwise play every role's notes too.
class SplitExclusiveInputTest final : public mp::test::Test {
public:
  SplitExclusiveInputTest() : Test("functional.split.exclusive-input", Category::Functional) {}
  void run() override {
    mp::MidiMap map;
    map.setExclusiveDevice(kOwn);
    mp::MidiMap::KeyboardBinding any;
    any.keyboardId = 1;  // any input, any channel: the Great
    map.addKeyboardBinding(any);
    mp::MidiMap::KeyboardBinding pedal;
    pedal.deviceId = kOwn;
    pedal.channel = kPedal;
    pedal.keyboardId = 4;
    map.addKeyboardBinding(pedal);

    std::vector<mp::MidiMap::KeyHit> hits;
    map.matchKeyboards(kOwn, kPedal, 36, 100, 0.0, hits);
    MP_CHECK(hits.size() == 1 && hits[0].keyboardId == 4, "the split's pedal channel plays the pedal alone");
    hits.clear();
    map.matchKeyboards(kOwn, kLower, 60, 100, 0.0, hits);
    MP_CHECK(hits.empty(), "a role mapped nowhere plays nothing, whatever 'any input' maps");
    hits.clear();
    map.matchKeyboards(kSource, 1, 60, 100, 0.0, hits);
    MP_CHECK(hits.size() == 1 && hits[0].keyboardId == 1, "every other input is matched as before");
    MP_CHECK(!map.hasChannelBinding(kOwn, kLower) && map.hasChannelBinding(kOwn, kPedal),
             "only the split's own mappings claim its channels");

    mp::MidiBinding piston;
    piston.source.kind = mp::MidiSourceKind::Note;
    piston.source.number = 60;
    piston.targetKind = mp::MidiTargetKind::Switch;
    piston.targetId = 77;
    map.bind(piston);
    mp::MidiSource fromSplit;
    fromSplit.kind = mp::MidiSourceKind::Note;
    fromSplit.number = 60;
    fromSplit.channel = kLower;
    fromSplit.deviceId = kOwn;
    MP_CHECK(!map.actionFor(fromSplit, 100).valid(), "nor does a control mapped from any input");
    fromSplit.deviceId = kSource;
    MP_CHECK(map.actionFor(fromSplit, 100).valid(), "which every other input still reaches");
  }
};
static SplitExclusiveInputTest g_splitExclusiveInput;

}  // namespace
