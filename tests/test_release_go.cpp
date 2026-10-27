// GrandOrgue's release techniques, each switched on alone on a synthetic pipe:
// the start found with its table of levels and slopes, the release scaled
// down after a key let go during the attack, and the release faded to
// silence after a staccato note.
#include "MpTest.h"
#include "Bumpometer.h"

#include "../src/mp_sampler/VoiceEngine.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

constexpr double kRate = 48000.0;
constexpr double kHz = 200.0;  // MIDI 55: GrandOrgue takes its attack as 306.25 ms
constexpr double kPi = 3.141592653589793;

double tone(double t, double phase) {
  const double w = 2.0 * kPi * kHz * t + phase;
  return 0.6 * std::sin(w) + 0.25 * std::sin(2.0 * w + 0.7) + 0.12 * std::sin(3.0 * w + 1.9);
}

mp::SampleBuffer makeBuffer(bool release) {
  mp::SampleBuffer b;
  b.numChannels = 1;
  b.sampleRate = kRate;
  b.numFrames = 72000;
  b.frames.resize(static_cast<size_t>(b.numFrames));
  for (int i = 0; i < b.numFrames; ++i) {
    const double t = i / kRate;
    const double env = release ? std::exp(-t / 0.25) : 1.0 - std::exp(-t / 0.06);
    b.frames[static_cast<size_t>(i)] = static_cast<float>(env * tone(t, release ? kPi / 2.0 : 0.0));
  }
  if (!release) {
    b.loopStart = 48000;
    b.loopEnd = 72000;
  }
  return b;
}

struct Rig {
  mp::SampleBuffer attack = makeBuffer(false);
  mp::SampleBuffer tail = makeBuffer(true);
  mp::Pipe pipe;
  Rig() {
    pipe.pipeId = 100;
    pipe.midiNote = 55;
    mp::PipeLayer layer;
    layer.layerId = 1;
    mp::AttackSample a;
    a.id = 11;
    a.sample.sampleId = 1;
    a.sample.resolvedPitchHz = kHz;
    layer.attacks.push_back(a);
    mp::ReleaseSample r;
    r.id = 21;
    r.sample.sampleId = 2;
    r.sample.resolvedPitchHz = kHz;
    r.releaseCrossfadeMs = 10.0;
    layer.releases.push_back(r);
    pipe.layers.push_back(std::move(layer));
  }
};

struct Take {
  std::vector<float> y;
  bool aligned = false;
  int voicesAtEnd = 0;
};

enum Switch { None = 0, Table = 1, ShortGain = 2, Staccato = 4 };

// The key let go at `at`, `after` frames rendered past it, in 256-frame blocks.
Take play(Rig& rig, int switches, int64_t at, int64_t after, bool align = true) {
  auto provider = [&](mp::Id id) -> const mp::SampleBuffer* {
    return id == 1 ? &rig.attack : id == 2 ? &rig.tail : nullptr;
  };
  mp::VoiceEngine eng;
  eng.prepare(kRate, 4, 1);
  eng.setSampleProvider(provider);
  eng.setReleasePhaseAlign(align);
  eng.setReleaseAlignTable((switches & Table) != 0);
  eng.setReleaseShortNoteGain((switches & ShortGain) != 0);
  eng.setReleaseStaccatoFade((switches & Staccato) != 0);
  eng.setReleaseLogging(true);
  mp::VoiceStart start;
  start.pipe = &rig.pipe;
  start.layer = &rig.pipe.layers[0];
  start.velocity = 90;
  start.ratio = 1.0;
  start.gain = 1.0f;
  Take take;
  take.y.assign(static_cast<size_t>(at + after), 0.0f);
  if (eng.startVoice(start, 7) < 0) return take;
  auto render = [&](int64_t from, int64_t to) {
    while (from < to) {
      const int n = static_cast<int>(std::min<int64_t>(256, to - from));
      float* out[1] = {take.y.data() + from};
      eng.render(out, 1, n);
      from += n;
    }
  };
  render(0, at);
  mp::NoteRelease rel;
  rel.velocity = 64;
  eng.noteOff(7, rel);
  std::vector<mp::VoiceEngine::ReleaseEvent> events;
  eng.takeReleaseEvents(events);
  take.aligned = !events.empty() && events.back().aligned;
  render(at, at + after);
  take.voicesAtEnd = eng.activeVoiceCount();
  return take;
}

double rms(const std::vector<float>& y, int64_t from, int64_t to) { return mp::bump::rmsOf(y, from, to); }

class ReleaseAlignTableTest final : public mp::test::Test {
public:
  ReleaseAlignTableTest() : Test("functional.voice.release-align-table", Category::Functional) {}
  void run() override {
    Rig rig;
    std::vector<mp::bump::Joint> table, none;
    int aligned = 0;
    const int64_t xfade = 480;
    for (int k = 0; k < 60; ++k) {
      const int64_t at = 57600 + (static_cast<int64_t>(k) * 240) / 60;  // one period, settled
      const Take t = play(rig, Table, at, xfade + 4800);
      if (t.aligned) ++aligned;
      table.push_back(mp::bump::measure(t.y, kRate, at, xfade, kRate / kHz));
      const Take u = play(rig, None, at, xfade + 4800, /*align*/ false);
      none.push_back(mp::bump::measure(u.y, kRate, at, xfade, kRate / kHz));
    }
    const auto s = mp::bump::summarise(table);
    const auto n = mp::bump::summarise(none);
    MP_CHECK(aligned == 60, "every release placed by the table: " + std::to_string(aligned) + " of 60");
    MP_CHECK(n.worstBumpDb > 3.0, "sanity: unaligned, the joint bumps by " + std::to_string(n.worstBumpDb) + " dB");
    MP_CHECK(s.worstBumpDb < n.worstBumpDb / 2.0,
             "the table halves the worst bump or better: " + std::to_string(s.worstBumpDb) + " dB against " +
                 std::to_string(n.worstBumpDb));
  }
};
static ReleaseAlignTableTest g_releaseAlignTable;

class ReleaseShortNoteGainTest final : public mp::test::Test {
public:
  ReleaseShortNoteGainTest() : Test("functional.voice.release-short-note-gain", Category::Functional) {}
  void run() override {
    Rig rig;
    // Let go at 100 ms of a 306.25 ms attack: a = 0.3265, gain 0.2 + 0.8 (2a - a^2) = 0.637.
    const int64_t at = 4800;
    const Take on = play(rig, ShortGain, at, 9600);
    const Take off = play(rig, None, at, 9600);
    const double ratio = rms(on.y, at + 2400, at + 7200) / std::max(1e-9, rms(off.y, at + 2400, at + 7200));
    MP_CHECK(std::fabs(ratio - 0.637) < 0.02, "the release at GrandOrgue's level for 100 ms held: " + std::to_string(ratio));
    // Past the attack, the level is the release's own.
    const int64_t late = 24000;
    const Take on2 = play(rig, ShortGain, late, 9600);
    const Take off2 = play(rig, None, late, 9600);
    const double ratio2 = rms(on2.y, late + 2400, late + 7200) / std::max(1e-9, rms(off2.y, late + 2400, late + 7200));
    MP_CHECK(std::fabs(ratio2 - 1.0) < 0.001, "and its own level after 500 ms held: " + std::to_string(ratio2));
  }
};
static ReleaseShortNoteGainTest g_releaseShortNoteGain;

class ReleaseStaccatoFadeTest final : public mp::test::Test {
public:
  ReleaseStaccatoFadeTest() : Test("functional.voice.release-staccato-fade", Category::Functional) {}
  void run() override {
    Rig rig;
    // Held 20 ms; the release runs 1.5 s, so the room fills in 130 ms and the
    // release fades over 130 + 6000 x 20 / 130 = 1053 ms.
    const int64_t at = 960;
    const int64_t after = 52800;  // 1.1 s
    const Take on = play(rig, Staccato, at, after);
    const Take off = play(rig, None, at, after);
    MP_CHECK(on.voicesAtEnd == 0, "the release has faded out by 1.1 s");
    MP_CHECK(off.voicesAtEnd == 1, "sanity: left alone it still sounds at 1.1 s");
    const int64_t mid = at + 24000;  // 0.5 s in: the level is 1 - 500 / 1053 = 0.525 of its own
    const double ratio = rms(on.y, mid - 1200, mid + 1200) / std::max(1e-9, rms(off.y, mid - 1200, mid + 1200));
    MP_CHECK(std::fabs(ratio - 0.525) < 0.03, "half way down at 0.5 s: " + std::to_string(ratio));
    // Held past the room's 130 ms, the release is left alone.
    const Take longer = play(rig, Staccato, 9600, after);
    MP_CHECK(longer.voicesAtEnd == 1, "a note held 200 ms keeps its whole release");
  }
};
static ReleaseStaccatoFadeTest g_releaseStaccatoFade;

}  // namespace
