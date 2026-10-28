// Two manuals on one channel (#637, point 18). Choosing the console for one of
// them keeps the other on the channel, and both stay through a reload: the
// device column released every other manual on the channel. A manual whose
// default channel another manual has taken is named, so the row's "Default"
// is not taken for a manual that plays.
#include "MpTest.h"

#if defined(MP_TEST_HAS_AUDIO)

#include "../src/mp_audio/MasterpieceProcessor.h"

#include <algorithm>
#include <string>
#include <vector>

using mp::test::Category;

class SharedChannelReloadTest final : public mp::test::Test {
public:
  SharedChannelReloadTest() : Test("functional.midi.shared-channel-survives-reload", Category::Functional) {}

  // The manuals one note reaches through the player's bindings.
  static std::vector<mp::Id> reached(mp::MasterpieceProcessor& proc, int device, int channel) {
    std::vector<mp::MidiMap::KeyHit> hits;
    proc.midiMap().matchKeyboards(device, channel, 60, 100, 0.0, hits);
    std::vector<mp::Id> out;
    for (const auto& h : hits) out.push_back(h.keyboardId);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  void run() override {
    // Manual I (701) on channel 2 and the Pedal (702) on channel 1 by default.
    const juce::File odf(juce::String(MP_TEST_FIXTURES_DIR) + "/recorder-keyboards.Organ_Hauptwerk_xml");
    mp::MasterpieceProcessor proc;
    const juce::File settings = proc.settingsFileFor(odf);
    const juce::File map = proc.midiMapFileFor(odf);
    const bool hadSettings = settings.existsAsFile(), hadMap = map.existsAsFile();
    const juce::String keptSettings = hadSettings ? settings.loadFileAsString() : juce::String();
    const juce::String keptMap = hadMap ? map.loadFileAsString() : juce::String();
    settings.deleteFile();
    map.deleteFile();

    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the fixture loads");
    proc.clearChannelAssignments();

    // The Pedal put on channel 2, the way the MIDI tab's channel column does.
    proc.keepRouting();
    proc.setKeyboardForChannel(2, 702, 0, false);
    MP_CHECK(proc.defaultChannelTakenBy(701) == 702,
             "Manual I, on channel 2 by default, is named as taken over by the Pedal");
    MP_CHECK(proc.defaultChannelTakenBy(702) == 0, "the Pedal, on a channel of its own choosing, is not");

    // Channel 2 chosen for Manual I as well: both play from it.
    proc.setKeyboardForChannel(2, 701, 0, false);
    const std::vector<mp::Id> both{701, 702};
    MP_CHECK(reached(proc, 0, 2) == both, "a note on channel 2 plays both manuals");
    MP_CHECK(proc.defaultChannelTakenBy(701) == 0, "and Manual I is no longer named");

    // A console chosen for the Pedal leaves Manual I on the channel.
    // Any console but one the settings play exclusively, which by design
    // answers only to the bindings that name it.
    const int device = proc.midiMap().exclusiveDevice() == 1 ? 2 : 1;
    proc.setKeyboardDevice(702, device);
    MP_CHECK(reached(proc, device, 2) == both, "with a console chosen for the Pedal, its notes play both manuals");
    proc.saveMidiMap();

    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads again");
    MP_CHECK(reached(proc, device, 2) == both, "both still play from channel 2 after the reload");

    mp::MasterpieceProcessor fresh;
    MP_CHECK(fresh.loadOrgan(odf, 0, false).ok, "the organ loads in a new session");
    MP_CHECK(reached(fresh, device, 2) == both, "and in a new session");

    if (hadSettings) settings.replaceWithText(keptSettings);
    else settings.deleteFile();
    if (hadMap) map.replaceWithText(keptMap);
    else map.deleteFile();
  }
};
static SharedChannelReloadTest g_sharedChannelReload;

#endif
