// Probes on a real sample set, for what a fixture cannot stand in for: a
// publisher's own wiring of couplers and crescendo. They run only when
// MP_PROBE_ODF names a definition, and pass with nothing to say otherwise:
//
//   MP_PROBE_ODF=/path/Friesach.Organ_Hauptwerk_xml mp_tests --only probe.real-set
//
// Each prints what it found, so a run on a set is a record of it.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

const char* probeDefinition() {
  const char* p = std::getenv("MP_PROBE_ODF");
  return p != nullptr && *p != 0 ? p : nullptr;
}

struct Rig {
  mp::MasterpieceProcessor proc;
  juce::AudioBuffer<float> buf{2, 256};
  void block(const juce::MidiBuffer& m = {}) {
    buf.clear();
    juce::MidiBuffer copy(m);
    proc.processBlock(buf, copy);
  }
  void blocks(int n) {
    for (int i = 0; i < n; ++i) block();
  }
  bool load(const char* path) {
    proc.prepareToPlay(48000.0, 256);
    if (!proc.loadOrgan(juce::File(juce::String::fromUTF8(path)), 0, false).ok) return false;
    blocks(1200);  // the blower and the start-up chains, 6 s
    return true;
  }
  // Tone pipes heard: their voices. A set whose samples are not on this
  // machine sounds nothing, and there the pallets of tone pipes open stand in
  // (on a set whose pallets open with the key whatever is drawn, as
  // Friesach's do, they say nothing).
  struct Heard {
    int voices = 0;
    int pallets = 0;
  };
  Heard tonePipes() const {
    Heard h;
    h.voices = proc.soundingTonePipes();
    for (const auto& [rankId, rank] : proc.organModel().ranks) {
      (void)rankId;
      if (rank.isNoise || rank.noiseEffect) continue;
      for (const auto& pipe : rank.pipes)
        if (pipe.palletSwitchId != 0 && proc.switchEngaged(pipe.palletSwitchId)) ++h.pallets;
    }
    return h;
  }
};

// Every coupler the organ defines from a keyboard to a division sounds that
// division's pipes (#1272: Friesach's sounded only their key noises).
class RealSetCouplersProbe final : public mp::test::Test {
public:
  RealSetCouplersProbe() : Test("probe.real-set.couplers", Category::Functional) {}
  void run() override {
    const char* path = probeDefinition();
    if (path == nullptr) return;
    Rig r;
    MP_CHECK(r.load(path), std::string("the set loads: ") + path);
    const auto& m = r.proc.organModel();
    const auto stops = r.proc.stopList();
    int couplers = 0;
    std::string failed;
    for (const auto& ka : m.keyActions) {
      if (ka.conditionSwitchId == 0 || ka.sourceKeyboard == 0 || ka.destIsKeyboard || ka.destDivision == 0) continue;
      int drawnThere = 0;
      for (const auto& s : stops) {
        const bool there = s.divisionId == ka.destDivision && s.playable && !s.control;
        r.proc.setStopEngaged(s.stopId, there);
        if (there) ++drawnThere;
      }
      if (drawnThere == 0) {
        std::printf("  division %d has no stops to sound: its coupler left out\n", ka.destDivision);
        continue;
      }
      ++couplers;
      r.proc.setSwitchEngaged(r.proc.playerSwitchFor(ka.conditionSwitchId), false);
      r.proc.setSwitchEngaged(ka.conditionSwitchId, false);
      r.proc.clearChannelAssignments();
      r.proc.setKeyboardForChannel(1, static_cast<mp::Id>(ka.sourceKeyboard));
      r.blocks(40);
      const int note = ka.numKeys > 0 ? ka.firstSourceNote + ka.numKeys / 2 : 60;
      const auto sw = m.switches.find(ka.conditionSwitchId);
      const std::string name = sw != m.switches.end() ? sw->second.name : std::to_string(ka.conditionSwitchId);
      const auto press = [&](bool coupled) {
        r.proc.setSwitchEngaged(r.proc.playerSwitchFor(ka.conditionSwitchId), coupled);
        r.proc.setSwitchEngaged(ka.conditionSwitchId, coupled);
        r.blocks(20);
        // The last press's release tails die away first, up to 10 s: they are
        // tone voices too.
        for (int i = 0; i < 1900 && r.proc.soundingTonePipes() > 0; ++i) r.block();
        juce::MidiBuffer down;
        down.addEvent(juce::MidiMessage::noteOn(1, note, 0.8f), 0);
        r.block(down);
        r.blocks(40);
        const Rig::Heard heard = r.tonePipes();
        juce::MidiBuffer up;
        up.addEvent(juce::MidiMessage::noteOff(1, note), 0);
        r.block(up);
        r.blocks(200);
        return heard;
      };
      const Rig::Heard off = press(false);
      const Rig::Heard on = press(true);
      press(false);
      // Voices where the set sounds here; pallets where it is silent.
      const bool audible = off.voices > 0 || on.voices > 0;
      const bool couples = audible ? on.voices > off.voices : on.pallets > off.pallets;
      std::printf("  coupler %s (switch %lld), keyboard %d to division %d (%d stops drawn), key %d: "
                  "tone voices %d without, %d with; tone pallets %d, %d%s\n",
                  name.c_str(), static_cast<long long>(ka.conditionSwitchId), ka.sourceKeyboard, ka.destDivision,
                  drawnThere, note, off.voices, on.voices, off.pallets, on.pallets, couples ? "" : "  <-- silent");
      if (!couples) failed += (failed.empty() ? "" : ", ") + name;
    }
    std::printf("  %d couplers to a division\n", couplers);
    MP_CHECK(failed.empty(), "every coupler sounds the pipes of the division it couples; not: " + failed);
  }
};
static RealSetCouplersProbe g_realSetCouplers;

// The crescendo plays the same registration at a value whichever way the
// shoe came to it (#1274: Düren's stayed on its highest step going back).
class RealSetCrescendoProbe final : public mp::test::Test {
public:
  RealSetCrescendoProbe() : Test("probe.real-set.crescendo", Category::Functional) {}
  void run() override {
    const char* path = probeDefinition();
    if (path == nullptr) return;
    Rig r;
    MP_CHECK(r.load(path), std::string("the set loads: ") + path);
    const mp::Id cres = r.proc.stageSwitches().crescendoControl();
    if (cres == 0) {
      std::printf("  no crescendo\n");
      return;
    }
    const auto stops = r.proc.stopList();
    const auto drawn = [&] {
      std::set<mp::Id> on;
      for (const auto& s : stops)
        if (r.proc.stopEngaged(s.stopId)) on.insert(s.stopId);
      return on;
    };
    std::vector<std::set<mp::Id>> up(128), down(128);
    r.proc.setControlValue(cres, 0);
    r.blocks(4);
    for (int v = 0; v <= 127; ++v) {
      r.proc.setControlValue(cres, v);
      r.blocks(2);
      up[static_cast<size_t>(v)] = drawn();
    }
    for (int v = 127; v >= 0; --v) {
      r.proc.setControlValue(cres, v);
      r.blocks(2);
      down[static_cast<size_t>(v)] = drawn();
    }
    int differ = 0, steps = 0;
    for (int v = 0; v <= 127; ++v) {
      if (v > 0 && up[static_cast<size_t>(v)] != up[static_cast<size_t>(v) - 1]) ++steps;
      if (up[static_cast<size_t>(v)] == down[static_cast<size_t>(v)]) continue;
      ++differ;
      if (differ <= 5)
        std::printf("  at %d: %zu stops drawn on the way up, %zu on the way down\n", v,
                    up[static_cast<size_t>(v)].size(), down[static_cast<size_t>(v)].size());
    }
    std::printf("  crescendo control %lld: %zu stage rows, %d changes of registration on the way up, "
                "%zu stops at the top\n",
                static_cast<long long>(cres), r.proc.stageSwitches().stepCount(cres), steps, up[127].size());
    MP_CHECK(differ == 0, "the same registration at each value both ways: " + std::to_string(differ) + " values differ");
    MP_CHECK(down[0] == up[0], "back at the start, the registration it started with");
  }
};
static RealSetCrescendoProbe g_realSetCrescendo;

}  // namespace

#endif
